import argparse
import os
import sys
import threading
import traceback
import torch
import torch.multiprocessing as mp
import time
from torchrec import EmbeddingBagCollection
from torchrec.sparse.jagged_tensor import KeyedJaggedTensor
from torchrec.modules.embedding_configs import EmbeddingBagConfig

SRC_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '../../../..'))
if SRC_ROOT not in sys.path:
    sys.path.insert(0, SRC_ROOT)

from python.pytorch.torchrec_kv.EmbeddingBag import RecStoreEmbeddingBagCollection
from python.pytorch.recstore.KVClient import get_kv_client
from python.pytorch.recstore.optimizer import SparseSGD
from python.pytorch.recstore.unittest.test_ebc_precision import get_eb_configs, compare_tensors, LEARNING_RATE, NUM_TEST_ROUNDS

if mp.get_start_method(allow_none=True) != 'spawn':
    try:
        mp.set_start_method('spawn', force=True)
    except RuntimeError:
        pass

BARRIER_TIMEOUT_SECONDS = 30
PROCESS_JOIN_TIMEOUT_SECONDS = 45

def generate_rank_batch(num_embeddings_per_rank, batch_size, device, rank, total_ranks):
    start_key = rank * num_embeddings_per_rank
    end_key = (rank + 1) * num_embeddings_per_rank
    num_embeddings_in_range = end_key - start_key
    
    avg_len = max(1, (num_embeddings_in_range // batch_size) // 2)
    lengths = torch.randint(1, avg_len * 2, (batch_size,), device=device, dtype=torch.int32)
    values = torch.randint(0, num_embeddings_in_range, (lengths.sum().item(),), device=device, dtype=torch.int64)
    values = values + start_key
    
    return KeyedJaggedTensor.from_lengths_sync(
        keys=["feature_0"],
        values=values,
        lengths=lengths,
    )

def worker(rank, world_size, args, barrier, table_name):
    try:
        torch.manual_seed(args.seed)

        device = "cpu"
        if not args.cpu and torch.cuda.is_available():
            device = "cuda"

        print(f"[Rank {rank}] Starting worker on {device}...")

        num_embeddings_per_rank = args.num_embeddings
        total_embeddings = num_embeddings_per_rank * world_size

        eb_configs = get_eb_configs(
            num_embeddings=total_embeddings,
            embedding_dim=args.embedding_dim,
            table_name=table_name,
        )

        standard_ebc = EmbeddingBagCollection(tables=eb_configs, device=device)

        kv_client = get_kv_client()
        if args.ps_host and args.ps_port:
            print(f"[Rank {rank}] Configuring PS Client to {args.ps_host}:{args.ps_port}")
            kv_client.set_ps_config(args.ps_host, args.ps_port)

        config = eb_configs[0]

        barrier.wait(timeout=BARRIER_TIMEOUT_SECONDS)

        if rank == 0:
            print(f"[Rank {rank}] Initializing backend table '{config.name}' with size {total_embeddings}...")

            with torch.no_grad():
                initial_weights = standard_ebc.state_dict()[f"embedding_bags.{config.name}.weight"]
                if initial_weights.device.type != 'cpu':
                    initial_weights = initial_weights.cpu()
                initial_weights = initial_weights.contiguous().clone()

            success = kv_client.ops.init_embedding_table(config.name, int(total_embeddings), int(config.embedding_dim))
            if not success:
                raise RuntimeError(f"Failed to initialize embedding table '{config.name}'")

            all_keys = torch.arange(total_embeddings, dtype=torch.int64)
            kv_client.ops.emb_write(all_keys, initial_weights)
            print(f"[Rank {rank}] Backend table initialized and weights written.")

        barrier.wait(timeout=BARRIER_TIMEOUT_SECONDS)

        recstore_eb_configs_dict = [
            {"name": c.name, "embedding_dim": c.embedding_dim, "num_embeddings": c.num_embeddings, "feature_names": c.feature_names}
            for c in eb_configs
        ]

        kv_client._tensor_meta[config.name] = {'shape': (total_embeddings, config.embedding_dim), 'dtype': torch.float32}
        kv_client._full_data_shape[config.name] = (total_embeddings, config.embedding_dim)
        kv_client._data_name_list.add(config.name)
        kv_client._gdata_name_list.add(config.name)

        recstore_ebc = RecStoreEmbeddingBagCollection(embedding_bag_configs=recstore_eb_configs_dict, lr=LEARNING_RATE, enable_fusion=False).to(device)

        local_start = rank * num_embeddings_per_rank
        local_end = (rank + 1) * num_embeddings_per_rank
        local_keys = torch.arange(local_start, local_end, dtype=torch.int64, device="cpu")

        with torch.no_grad():
            pulled_weights = kv_client.pull(name=config.name, ids=local_keys)
            std_weights = standard_ebc.state_dict()[f"embedding_bags.{config.name}.weight"][local_start:local_end].cpu()

            if not compare_tensors(std_weights, pulled_weights, f"Rank {rank} Init Sync"):
                print(f"🔥🔥🔥 [Rank {rank}] Initial sync failed!")
                sys.exit(1)

        print(f"[Rank {rank}] Initial sync verified.")

        standard_optimizer = torch.optim.SGD(standard_ebc.parameters(), lr=LEARNING_RATE)
        sparse_optimizer = SparseSGD([recstore_ebc], lr=LEARNING_RATE)

        torch.manual_seed(args.seed + rank + 1)

        for i in range(NUM_TEST_ROUNDS):
            barrier.wait(timeout=BARRIER_TIMEOUT_SECONDS)
            if rank == 0:
                print(f"\n--- Round {i+1} ---")

            batch = generate_rank_batch(num_embeddings_per_rank, args.batch_size, device, rank, world_size)

            standard_output_kt = standard_ebc(batch)
            recstore_output_kt = recstore_ebc(batch)

            if not compare_tensors(standard_output_kt.values(), recstore_output_kt.values(), f"Rank {rank} R{i+1} Fwd"):
                print(f"🔥🔥🔥 [Rank {rank}] Forward pass failed at round {i+1}")
                sys.exit(1)

            loss_std = standard_output_kt.values().sum()
            loss_rec = recstore_output_kt.values().sum()

            standard_optimizer.zero_grad()
            sparse_optimizer.zero_grad()

            loss_std.backward()
            loss_rec.backward()

            standard_optimizer.step()
            sparse_optimizer.step()
            sparse_optimizer.flush()

            with torch.no_grad():
                updated_std_weights = standard_ebc.state_dict()[f"embedding_bags.{config.name}.weight"][local_start:local_end].cpu()
                updated_rec_weights = kv_client.pull(name=config.name, ids=local_keys)

                if not compare_tensors(updated_std_weights, updated_rec_weights, f"Rank {rank} R{i+1} Wgt"):
                    print(f"🔥🔥🔥 [Rank {rank}] Weight update failed at round {i+1}")
                    sys.exit(1)

        barrier.wait(timeout=BARRIER_TIMEOUT_SECONDS)
        if rank == 0:
            print("\nAll rounds passed on all ranks!")
    except mp.context.TimeoutError:
        print(f"[Rank {rank}] Timed out waiting on multiprocessing barrier")
        traceback.print_exc()
        sys.exit(2)
    except threading.BrokenBarrierError:
        print(f"[Rank {rank}] Barrier broken because another rank failed")
        traceback.print_exc()
        sys.exit(3)
    except Exception:
        print(f"[Rank {rank}] Worker failed unexpectedly")
        traceback.print_exc()
        sys.exit(4)

def main(args):
    """
    Main entry point for multiprocess test.
    """
    if args.cpu:
        os.environ["CUDA_VISIBLE_DEVICES"] = ""
    
    world_size = args.world_size
    mp.set_start_method('spawn', force=True)
    
    print(f"Starting {world_size} workers for multiprocess precision test.")
    print(f"Total embeddings: {args.num_embeddings * world_size} ({args.num_embeddings} per rank)")
    
    barrier = mp.Barrier(world_size)
    processes = []
    
    # Generate unique table name here to share across ranks
    import time
    table_name = f"table_mp_{int(time.time())}_{os.getpid()}"
    
    for rank in range(world_size):
        p = mp.Process(target=worker, args=(rank, world_size, args, barrier, table_name))
        p.start()
        processes.append(p)
        
    failed = False
    for p in processes:
        p.join(PROCESS_JOIN_TIMEOUT_SECONDS)
        if p.is_alive():
            print(f"Worker PID {p.pid} did not finish within {PROCESS_JOIN_TIMEOUT_SECONDS}s; terminating.")
            p.terminate()
            p.join(5)
            failed = True
        if p.exitcode != 0:
            failed = True
            
    if failed:
        sys.exit(1)
    else:
        print("✅ Multiprocess test completed successfully.")

def main_wrapper():
    """Wrapper for main to be called from if __name__ == "__main__"."""
    if __name__ == "__main__":
        parser = argparse.ArgumentParser(description="Multiprocess precision test.")
        parser.add_argument("--num-embeddings", type=int, default=1000, help="Number of embeddings PER RANK.")
        parser.add_argument("--embedding-dim", type=int, default=128, help="Dimension of embeddings.")
        parser.add_argument("--batch-size", type=int, default=32, help="Batch size per rank.")
        parser.add_argument("--seed", type=int, default=42, help="Seed.")
        parser.add_argument("--cpu", action="store_true", help="Force CPU.")
        parser.add_argument("--world-size", type=int, default=2, help="Number of processes.")
        parser.add_argument("--ps-host", type=str, default=None, help="PS Host")
        parser.add_argument("--ps-port", type=int, default=None, help="PS Port")
        
        args = parser.parse_args()
        main(args)

if __name__ == "__main__":
    main_wrapper()
