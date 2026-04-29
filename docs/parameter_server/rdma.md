# RDMA 模块运行手册

本文档记录当前 RecStore RDMA 路径的启动、验证、benchmark 和排障方式。默认工作目录为仓库根目录：

```bash
cd /app/RecStore
```

## 当前边界

RecStore 里有两层 RDMA 入口：

| 层级 | 入口 | 作用 |
|------|------|------|
| PetPS RDMA | `petps_server` + `PetPSClient` | RDMA 数据面、协议、benchmark、PetPS integration |
| Framework Op-layer RDMA | `RDMAPSClientAdapter` + `KVClientOp` | 通过统一 op 接口测试 RDMA 后端 |

两层最终都会复用 PetPS/RDMA 数据面，但初始化入口不同。不要把两者的命令行参数混用。

## Transport Mode

当前 RDMA 支持两种 transport mode：

| mode | 状态 | 说明 |
|------|------|------|
| `raw_message` | 默认稳定路径 | 继续使用 Mayfly/DSM RawMessage 控制面 |
| `descriptor_doorbell` | 新增实验路径 | 使用 RecStore descriptor + raw verbs doorbell 控制面 |

默认不显式传参时就是 `raw_message`。

### 参数传播规则

`petps_server` 和 `PetPSClient` 支持 gflag：

```bash
--rdma_transport_mode=raw_message
--rdma_transport_mode=descriptor_doorbell
```

但 `ps_transport_benchmark` 本身不支持这个 gflag。benchmark runner 必须避免把 `--rdma_transport_mode` 直接传给 benchmark binary，否则会失败：

```text
ERROR: unknown command line flag 'rdma_transport_mode'
```

当前脚本约定：

- server 进程通过 `--rdma_transport_mode=...` 接收模式。
- benchmark client 通过环境变量 `RECSTORE_RDMA_TRANSPORT_MODE` 传给 op/client 初始化路径。
- raw-message 默认 benchmark 不传任何 transport mode 参数，保持旧行为。

## 快速基线：run.sh

`run.sh` 是当前最直接的 RDMA benchmark 基线入口：

```bash
bash run.sh
```

它会执行：

- 启动或复用本地 memcached
- 启动 1 个 `petps_server`
- 运行 `ps_transport_benchmark`
- 只跑 RDMA lane，跳过 GRPC/BRPC
- 默认使用 `raw_message` transport mode

当前已验证过的输出应包含类似内容：

```text
I open mlx5_0 :)
I connect server 0
transport=RDMA op=put phase=measure ...
transport=RDMA op=get phase=measure ...
```

如果输出 `I open mlx5_0 :)`，说明实际走到了 RDMA verbs 设备，不应再把问题归因成“环境没有 RDMA”。

## 构建

常用 RDMA 相关目标：

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

如果只跑 `run.sh`，至少需要：

```bash
cmake --build ./build --target ps_transport_benchmark petps_server -j
```

## Benchmark

`run.sh` 当前展开后等价于：

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

### raw_message 基线

raw-message 是默认模式。不要额外传 `--rdma-transport-mode raw_message`，除非你正在验证 runner 的 mode plumbing。

推荐基线：

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

当前一次本地验证结果示例：

```text
transport=RDMA op=put phase=measure summary rounds=20 iterations=300 batch_keys=500 ...
transport=RDMA op=get phase=measure summary rounds=20 iterations=300 batch_keys=500 ...

| transport | mode        | op  | rounds | iterations | batch_keys | mean_req_us | key_ops/s    |
| RDMA      | raw_message | put | 20     | 300        | 500        | 71.58       | 6,985,310.00 |
| RDMA      | raw_message | get | 20     | 300        | 500        | 56.70       | 8,818,390.00 |
```

数值会随机器、负载和 RDMA 设备状态变化。这里的重点是命令能完整跑通并产生 put/get measure summary。

### descriptor_doorbell 实验路径

descriptor mode 仍是实验路径。运行方式：

```bash
python3 src/test/scripts/run_rdma_transport_benchmarks.py \
  --benchmark-binary ./build/bin/ps_transport_benchmark \
  --rdma-only \
  --rdma-transport-mode descriptor_doorbell \
  --iterations 100 \
  --batch-keys 128 \
  --rounds 5 \
  --rdma-warmup-rounds 2 \
  --report-mode summary \
  --use-local-memcached auto
```

注意：

- `--rdma-transport-mode descriptor_doorbell` 是 runner 参数，不是 benchmark binary 参数。
- runner 会把 mode 传给 server，并通过环境变量传给 client 初始化路径。
- 当前 descriptor mode 目标是先保持正确性闭环，不应把它当成已证明有 async overlap 收益的路径。

### descriptor_doorbell 当前实现约束

descriptor mode 使用 raw verbs RDMA write 写 descriptor slot，再用 send-with-imm 作为 doorbell。服务端收到 doorbell 后从本地 slot 解码 descriptor，再根据 descriptor 中的 remote address 读取请求 payload 或写回 response/status。

当前必须满足这些约束：

- raw verbs 的本地 read/write buffer 必须来自 `RawVerbsTransport::AllocateRegistered()`，不能用栈变量或普通 `std::vector` 作为 verbs local buffer。
- descriptor mode 服务端只允许一个线程 poll raw verbs CQ。当前由 `thread_id == 0` 负责 `PublishAndConnect()`、doorbell poll 和 descriptor 处理；其他 polling threads 不参与 raw CQ。
- descriptor basic 目前覆盖同步 write/read 正确性。async GET 在第一阶段未启用。
- descriptor mode 与 raw-message 共享 PetPS/RDMA 数据面配置，但控制面不同；不要混合排障结论。

如果 descriptor 测试表现为卡住，优先检查 server 是否提前崩溃或是否有 CQ completion 被错误线程消费。典型日志包括：

```text
raw verbs CQ error: local protection error
PetPSClient::PutParameterDescriptor
```

这通常说明某个 raw verbs local buffer 没有落在 registered MR 内，或者多个线程同时 poll 同一个 CQ。

## PetPS Integration

### 单分片

```bash
python3 src/test/scripts/run_petps_integration.py \
  --server-count 1 \
  --config-path ./src/test/configs/recstore_config.rdma_test.json \
  --test-binary ./build/bin/petps_integration_test \
  --gtest-filter=PetPSIntegrationTest.PutGetRoundTripSingleShard:PetPSIntegrationTest.MissingKeysReturnZeroSlots \
  --client-timeout 15 \
  --use-local-memcached auto
```

descriptor mode：

```bash
python3 src/test/scripts/run_petps_integration.py \
  --server-count 1 \
  --config-path ./src/test/configs/recstore_config.rdma_test.json \
  --test-binary ./build/bin/petps_integration_test \
  --gtest-filter=PetPSIntegrationTest.PutGetRoundTripSingleShard:PetPSIntegrationTest.MissingKeysReturnZeroSlots \
  --rdma-transport-mode descriptor_doorbell \
  --client-timeout 15 \
  --use-local-memcached auto
```

### 多分片

```bash
python3 src/test/scripts/run_petps_integration.py \
  --server-count 2 \
  --client-count 1 \
  --config-path ./src/test/configs/recstore_config.rdma_multishard_test.json \
  --test-binary ./build/bin/petps_integration_test \
  --gtest-filter=PetPSIntegrationTest.PutGetRoundTripMultiShard \
  --client-timeout 30 \
  --use-local-memcached auto
```

多分片排障时优先检查：

- `distributed_client.num_shards`
- `distributed_client.servers`
- `server-count`
- `num_server_processes`
- key 到 shard 的路由是否一致

## Op-layer RDMA

Op-layer RDMA 使用：

```text
src/test/configs/recstore_config.op_rdma.json
```

核心入口：

```bash
ctest --test-dir ./build -R "^pytorch_client_test_rdma_basic$" -VV
```

完整 raw-message op-layer：

```bash
ctest --test-dir ./build -R "^pytorch_client_test_rdma$|^pytorch_client_test_rdma_auto$" -VV
```

descriptor mode 入口：

```bash
ctest --test-dir ./build -R "^pytorch_client_test_rdma_descriptor_basic$" -VV
```

也可以手工指定：

```bash
RECSTORE_CONFIG=./src/test/configs/recstore_config.op_rdma.json \
RECSTORE_CLIENT_TEST_PHASE=basic \
RECSTORE_USE_LOCAL_MEMCACHED=auto \
RECSTORE_RDMA_TRANSPORT_MODE=descriptor_doorbell \
python3 src/test/framework/pytorch/test_client.py ./build/lib/lib_recstore_ops.so
```

Op-layer RDMA 测试会在 reexec 后追加 C++ gflags，例如：

```text
--global_id=1
--num_server_processes=1
--num_client_processes=1
--value_size=512
--max_kv_num_per_request=500
```

这些参数需要保留给 C++/gflags 初始化，但不能交给 Python `unittest` 解析。若看到：

```text
test_client.py: error: unrecognized arguments: --global_id=1 ...
```

说明 Python 测试入口没有正确过滤 C++ gflags。

注意：某些 ctest helper 会先检查 `/dev/infiniband` 并返回 skip code 77。skip 只说明该 helper 的前置检查没有通过，不等价于 `run.sh` 真实 RDMA benchmark 不可运行。判断 RDMA 是否可用时，应优先跑 `bash run.sh` 或直接看 `I open mlx5_0 :)` / benchmark summary。

## 当前已验证的 RDMA 测试

以下命令在真实 RDMA 环境中用于确认当前修复后的状态：

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

```bash
ctest --test-dir ./build -R "^pytorch_client_test_rdma_basic$" -VV
ctest --test-dir ./build -R "^pytorch_client_test_rdma$|^pytorch_client_test_rdma_auto$" -VV
ctest --test-dir ./build -R "^pytorch_client_test_rdma_descriptor_basic$" -VV
```

结果矩阵：

| 测试 | mode | 覆盖点 | 当前状态 |
|------|------|--------|----------|
| PetPS single-shard integration | `raw_message` | Put/Get 单分片、missing key | 通过 |
| PetPS multi-shard integration | `raw_message` | 多分片 Put/Get 路由 | 通过 |
| `pytorch_client_test_rdma_basic` | `raw_message` | Op-layer init/write/read basic | 通过 |
| `pytorch_client_test_rdma` | `raw_message` | Op-layer full phase | 通过 |
| `pytorch_client_test_rdma_auto` | `raw_message` | Op-layer full phase + memcached auto | 通过 |
| `pytorch_client_test_rdma_descriptor_basic` | `descriptor_doorbell` | Op-layer descriptor basic | 通过 |

这些结果只说明当前 correctness smoke/integration 已闭环，不代表 descriptor mode 已有稳定性能收益。

## 单元测试

快速检查协议和 wrapper：

```bash
ctest --test-dir ./build -R "^test_rdma_protocol$|^test_allshards_ps_client$" -VV
```

脚本 plumbing：

```bash
python3 -m unittest src/test/scripts/test_petps_cluster_runner.py
python3 -m unittest src/test/scripts/test_run_rdma_transport_benchmarks.py
```

这些测试不证明真实 RDMA 数据面可用，只证明协议 helper、分片 wrapper 和 runner 参数拼接没有明显回归。

## memcached

RDMA 脚本默认通过 memcached 交换 Mayfly/DSM 元数据。

推荐使用：

```bash
--use-local-memcached auto
```

模式说明：

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

## 排障顺序

### 1. 先确认是不是命令行 flag 问题

如果看到：

```text
ERROR: unknown command line flag 'rdma_transport_mode'
```

说明 `--rdma_transport_mode` 被传给了不支持该 gflag 的 binary，常见是 `ps_transport_benchmark`。

修正方向：

- benchmark runner 使用 `--rdma-transport-mode` 作为脚本参数。
- 不要把 `--rdma_transport_mode` 直接加到 `ps_transport_benchmark` 命令后。
- raw-message 基线不要显式传 mode。

### 2. 再确认 RDMA 设备是否真的打开

真实 benchmark 输出中应看到：

```text
I open mlx5_0 :)
I connect server 0
```

如果没有，检查：

```bash
ls -l /dev/infiniband
ibv_devices
```

### 3. 检查 memcached

常见卡点：

```text
[petps-status] phase=memcached-wait
```

检查：

```bash
ss -ltnp | grep ':21211'
```

必要时杀掉旧 memcached 或改用 `--use-local-memcached always`。

### 4. 检查 server ready

常见卡点：

```text
[petps-status] phase=startup-wait
```

优先看 server 日志，确认是否出现：

```text
component=rdma_server event=polling_thread_ready thread_id=0
```

如果 server 提前退出，runner 会打印：

```text
petps_server exited early
Captured output from petps_server[0]
```

先读这段日志，不要先猜测 RDMA 不可用。

### 5. 检查请求大小

默认 `MESSAGE_SIZE` 下，`batch-keys=1000` 可能触发：

```text
messeage size too large
```

建议先用：

```text
batch-keys=500
```

做稳定基线。

### 6. descriptor mode 卡住但 server 已退出

descriptor mode 如果在 ctest 中看起来卡住，先检查实际日志：

```bash
tail -200 build/Testing/Temporary/LastTest.log
ps -eo pid,ppid,stat,wchan:32,cmd | rg 'ctest|test_client.py|petps_server|memcached'
```

如果只剩 Python 父进程，而 `petps_server` 或 `memcached` 是 `<defunct>`，通常不是网络卡住，而是子进程已经崩溃，父进程没有干净回收。下一步应看 server/client stack trace。

descriptor mode 的重点排查项：

- server 端 raw verbs local buffer 是否都来自 `AllocateRegistered()`。
- 是否只有一个线程 poll raw verbs CQ。
- client 端 `unittest` 是否过滤了 reexec 追加的 C++ gflags。
- `RECSTORE_RDMA_TRANSPORT_MODE=descriptor_doorbell` 是否传到了 client，同时 `--rdma_transport_mode=descriptor_doorbell` 是否传到了 server。

## 当前建议

日常验证按这个顺序：

```bash
cmake --build ./build --target ps_transport_benchmark petps_server -j
bash run.sh
ctest --test-dir ./build -R "^pytorch_client_test_rdma_basic$" -VV
ctest --test-dir ./build -R "^pytorch_client_test_rdma_descriptor_basic$" -VV
python3 -m unittest src/test/scripts/test_petps_cluster_runner.py
python3 -m unittest src/test/scripts/test_run_rdma_transport_benchmarks.py
ctest --test-dir ./build -R "^test_rdma_protocol$|^test_allshards_ps_client$" -VV
```

如果 `bash run.sh` 通过，但某个 ctest RDMA helper skipped，不要直接判定 RDMA 环境不可用；应检查该 helper 的 skip 条件和启动路径。
