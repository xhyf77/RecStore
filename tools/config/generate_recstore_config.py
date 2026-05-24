import json
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
OUTPUT_PATH = REPO_ROOT / "recstore_config.json"


config = {}

config["cache_ps"] = {}
config["cache_ps"]["ps_type"] = "GRPC"
config["cache_ps"]["max_batch_keys_size"] = 65536
config["cache_ps"]["num_threads"] = 32
config["cache_ps"]["num_shards"] = 2
config["cache_ps"]["servers"] = [
    {"host": "127.0.0.1", "port": 15000, "shard": 0},
    {"host": "127.0.0.1", "port": 15001, "shard": 1},
]

config["distributed_client"] = {}
config["distributed_client"]["num_shards"] = 2
config["distributed_client"]["hash_method"] = "city_hash"
config["distributed_client"]["max_keys_per_request"] = 500
config["distributed_client"]["servers"] = [
    {"host": "127.0.0.1", "port": 15000, "shard": 0},
    {"host": "127.0.0.1", "port": 15001, "shard": 1},
]

config["client"] = {}
config["client"]["host"] = "127.0.0.1"
config["client"]["port"] = 15000
config["client"]["shard"] = 1

base_kv_config = {}
base_kv_config["kv_type"] = "KVEngineMap"
base_kv_config["path"] = f"/dev/shm/{base_kv_config['kv_type']}"
base_kv_config["capacity"] = 1 * (10**6)
base_kv_config["value_pool_size"] = 1 * 1024 * 1024 * 1024
base_kv_config["corotine_per_thread"] = 1

config["cache_ps"]["base_kv_config"] = base_kv_config

with OUTPUT_PATH.open("w", encoding="utf-8") as f:
    json.dump(config, f, ensure_ascii=False, indent=4)
