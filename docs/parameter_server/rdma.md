# RDMA 模块运行手册

本文档只记录当前 RDMA 路径的边界、构建、运行和验证入口。默认工作目录为仓库根目录：

```bash
cd /app/RecStore
```

## 当前边界

RecStore 目前有两层 RDMA 入口：

| 层级 | 入口 | 用途 |
|------|------|------|
| PetPS RDMA | `petps_server` + `PetPSClient` | RDMA 数据面、协议验证、transport benchmark |
| Op-layer RDMA | `RDMAPSClientAdapter` + `KVClientOp` | 通过统一 op 接口验证 RDMA 后端 |

两层复用 PetPS/RDMA 数据面，但初始化方式不同：

- PetPS integration 和 benchmark 主要通过 C++ gflags 传参。
- Op-layer / Python client 主要通过环境变量和测试配置传参。
- 不要把脚本参数、C++ gflag 和环境变量混用到错误入口。

Op-layer RDMA 还不是 gRPC/bRPC 的完整替代：`AsyncGetParameter` 和 `Command` 未实现，`UpdateParameter` 走同步 read-modify-write，当前更适合作为 correctness / integration 路径。

## Transport Mode

当前支持两种模式：

| mode | 状态 | 说明 |
|------|------|------|
| `raw_message` | 默认稳定路径 | Mayfly/DSM RawMessage 控制面 |
| `descriptor_doorbell` | 稳定支持路径 | raw verbs 写 descriptor slot，再用 doorbell 通知服务端 |

默认不显式传参时是 `raw_message`。

PetPS server、PetPS client 和 `ps_transport_benchmark` 支持 C++ gflag：

```bash
--rdma_transport_mode=raw_message
--rdma_transport_mode=descriptor_doorbell
```

脚本层通常使用横线参数：

```bash
--rdma-transport-mode descriptor_doorbell
```

Op-layer RDMA 使用环境变量：

```bash
RECSTORE_RDMA_TRANSPORT_MODE=descriptor_doorbell
```

## 构建

常用 RDMA 目标：

```bash
cmake --build ./build --target \
  ps_transport_benchmark \
  petps_server \
  petps_integration_test \
  recstore_torch_ops \
  test_rdma_protocol \
  test_allshards_ps_client \
  -j
```

如果刚改过 `src/ps/rdma/*`、`src/test/scripts/*rdma*` 或 op-layer 相关代码，先重编对应目标再判断行为。旧的 `petps_server` / `recstore_torch_ops` 二进制很容易造成“源码已改但测试仍卡住”的假象。

## Benchmark

推荐先跑 raw-message 基线：

```bash
python3 src/test/scripts/run_rdma_transport_benchmarks.py \
  --benchmark-binary ./build/bin/ps_transport_benchmark \
  --iterations 300 \
  --batch-keys 500 \
  --rounds 20 \
  --rdma-warmup-rounds 10 \
  --report-mode summary \
  --rdma-only \
  --rdma-thread-num 1 \
  --rdma-put-protocol-version 2 \
  --rdma-put-v2-transfer-mode read \
  --rdma-wait-timeout-ms 20000 \
  --rdma-client-timeout-sec 60 \
  --show-runner-logs \
  --use-local-memcached auto
```

descriptor-doorbell 对应命令：

```bash
python3 src/test/scripts/run_rdma_transport_benchmarks.py \
  --benchmark-binary ./build/bin/ps_transport_benchmark \
  --rdma-only \
  --rdma-transport-mode descriptor_doorbell \
  --iterations 300 \
  --batch-keys 500 \
  --rounds 20 \
  --rdma-warmup-rounds 10 \
  --rdma-thread-num 1 \
  --rdma-put-protocol-version 2 \
  --rdma-put-v2-transfer-mode read \
  --rdma-wait-timeout-ms 20000 \
  --rdma-client-timeout-sec 60 \
  --report-mode summary \
  --show-runner-logs \
  --use-local-memcached auto
```

对比 raw-message 和 descriptor-doorbell 时，必须保持 `thread_num`、`put_v2`、`iterations`、`batch-keys`、`rounds` 和 warmup 一致。summary 表中的 `put_v2` 列用于确认 PUT-v2 payload transfer mode；`read` 和 `push` 的结果不能直接混比。

真实 RDMA 路径通常会输出类似：

```text
I open mlx5_0 :)
I connect server 0
transport=RDMA op=put phase=measure ...
transport=RDMA op=get phase=measure ...
```

descriptor-doorbell 已作为稳定支持路径纳入 integration 和 benchmark 验证。性能对比仍需要保持同口径参数，不能只凭控制面差异推断收益。

## PetPS Integration

单分片 raw-message：

```bash
python3 src/test/scripts/run_petps_integration.py \
  --server-count 1 \
  --config-path ./src/test/configs/recstore_config.rdma_test.json \
  --test-binary ./build/bin/petps_integration_test \
  --gtest-filter=PetPSIntegrationTest.PutGetRoundTripSingleShard:PetPSIntegrationTest.MissingKeysReturnZeroSlots \
  --client-timeout 15 \
  --cluster-timeout 15 \
  --use-local-memcached auto
```

单分片 descriptor-doorbell：

```bash
python3 src/test/scripts/run_petps_integration.py \
  --server-count 1 \
  --config-path ./src/test/configs/recstore_config.rdma_test.json \
  --test-binary ./build/bin/petps_integration_test \
  --gtest-filter=PetPSIntegrationTest.PutGetRoundTripSingleShard:PetPSIntegrationTest.MissingKeysReturnZeroSlots \
  --rdma-transport-mode descriptor_doorbell \
  --client-timeout 15 \
  --cluster-timeout 15 \
  --use-local-memcached auto
```

多分片 raw-message：

```bash
python3 src/test/scripts/run_petps_integration.py \
  --server-count 2 \
  --client-count 1 \
  --config-path ./src/test/configs/recstore_config.rdma_multishard_test.json \
  --test-binary ./build/bin/petps_integration_test \
  --gtest-filter=PetPSIntegrationTest.PutGetRoundTripMultiShard \
  --client-timeout 30 \
  --cluster-timeout 20 \
  --use-local-memcached auto
```

多分片 descriptor-doorbell：

```bash
python3 src/test/scripts/run_petps_integration.py \
  --server-count 2 \
  --client-count 1 \
  --config-path ./src/test/configs/recstore_config.rdma_multishard_test.json \
  --test-binary ./build/bin/petps_integration_test \
  --gtest-filter=PetPSIntegrationTest.PutGetRoundTripMultiShard \
  --rdma-transport-mode descriptor_doorbell \
  --client-timeout 30 \
  --cluster-timeout 20 \
  --use-local-memcached auto
```

多分片排障时优先检查 `distributed_client.num_shards`、`distributed_client.servers`、`server-count`、`num_server_processes` 和 key 到 shard 的路由是否一致。

## Op-layer RDMA

Op-layer RDMA 使用配置：

```text
src/test/configs/recstore_config.op_rdma.json
```

常用 ctest：

```bash
ctest --test-dir ./build -R "^pytorch_client_test_rdma_basic$" -VV
ctest --test-dir ./build -R "^pytorch_client_test_rdma$|^pytorch_client_test_rdma_auto$" -VV
ctest --test-dir ./build -R "^pytorch_client_test_rdma_descriptor_basic$" -VV
```

也可以手工运行 descriptor basic：

```bash
RECSTORE_CONFIG=./src/test/configs/recstore_config.op_rdma.json \
RECSTORE_CLIENT_TEST_PHASE=basic \
RECSTORE_USE_LOCAL_MEMCACHED=auto \
RECSTORE_RDMA_TRANSPORT_MODE=descriptor_doorbell \
python3 src/test/framework/pytorch/test_client.py ./build/lib/lib_recstore_ops.so
```

这些测试的 `SKIP_RETURN_CODE` 是 `77`。skip 只说明 helper 的前置检查没有通过，不等价于真实 RDMA benchmark 一定不可运行。

## Unit 和脚本测试

协议 helper 和 wrapper：

```bash
ctest --test-dir ./build -R "^test_rdma_protocol$|^test_allshards_ps_client$" -VV
```

runner 参数拼接：

```bash
python3 -m unittest src/test/scripts/test_petps_cluster_runner.py
python3 -m unittest src/test/scripts/test_run_rdma_transport_benchmarks.py
```

这些测试不证明 RDMA 数据面可用，只证明协议编码、分片 wrapper 和脚本 plumbing 没有明显回归。

## memcached

RDMA 脚本通过 memcached 交换 Mayfly/DSM 元数据。推荐使用：

```bash
--use-local-memcached auto
```

| 值 | 行为 |
|----|------|
| `auto` | 优先复用外部 memcached；不可用时启动本地 memcached |
| `always` | 总是启动本地 memcached |
| `never` | 只使用已经存在的外部 memcached |

手工启动：

```bash
memcached -u root -l 127.0.0.1 -p 21211 -c 10000
```

检查端口：

```bash
ss -ltnp | grep ':21211'
```

## descriptor-doorbell 约束

descriptor-doorbell 只替代控制面通知，不改变 GET/PUT 的业务语义：

- GET：descriptor 后携带 keys payload，服务端复用 `CachePS::GetParameterFlat`，再写回 values/status。
- PUT：descriptor 后携带 PUT control payload，服务端复用 raw-message PUT 的 v1/v2 decode、校验和写入逻辑。
- PUT-v2 大 payload 仍由 `read|push` transfer mode 决定所有权和拷贝方式。

当前实现约束：

- raw verbs 本地 read/write buffer 必须来自 registered memory，不能用栈变量或普通 `std::vector` 作为 verbs local buffer。
- descriptor serving thread 由 `GetRdmaDescriptorServingThreadIDs()` 决定；不要让多个线程误 poll 同一个 raw verbs CQ。
- descriptor async / prefetch 路径仍要通过 opaque request id 和 pending map 管理，不能暴露后端局部 handle。
- descriptor-doorbell 是稳定支持路径；async overlap 或性能收益仍需要用同口径 benchmark 单独证明。

## 排障顺序

1. 确认二进制是最新构建的目标，尤其是 `petps_server`、`ps_transport_benchmark`、`petps_integration_test` 和 `recstore_torch_ops`。
2. 确认参数传到了正确入口：runner 用 `--rdma-transport-mode`，C++ binary 用 `--rdma_transport_mode`，op-layer 用 `RECSTORE_RDMA_TRANSPORT_MODE`。
3. 确认 RDMA 设备和 memcached 可用：检查 `/dev/infiniband`、`ibv_devices`、`ss -ltnp | grep ':21211'`。
4. 如果 runner 卡在 `memcached-wait` 或 `startup-wait`，先看 runner 捕获的 server 日志。
5. 如果看到 `unknown command line flag 'rdma_transport_mode'`，通常是跑到了旧 binary，或者当前目标没有链接 RDMA client 相关对象。
6. 如果看到 `messeage size too large`，先把 `batch-keys` 降到 500 或更小建立稳定基线。
7. 如果 descriptor mode 卡住，重点检查 raw verbs buffer 是否注册、CQ 是否被错误线程消费、server/client mode 是否一致。

最小日常验证顺序：

```bash
cmake --build ./build --target ps_transport_benchmark petps_server recstore_torch_ops -j
python3 src/test/scripts/run_rdma_transport_benchmarks.py \
  --benchmark-binary ./build/bin/ps_transport_benchmark \
  --rdma-only \
  --iterations 300 \
  --batch-keys 500 \
  --rounds 20 \
  --rdma-warmup-rounds 10 \
  --report-mode summary \
  --use-local-memcached auto
ctest --test-dir ./build -R "^pytorch_client_test_rdma_basic$" -VV
ctest --test-dir ./build -R "^pytorch_client_test_rdma_descriptor_basic$" -VV
```
