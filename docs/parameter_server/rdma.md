# RDMA 模块

## 概述

!!! warning "边界说明"
    本仓库当前同时存在两条 RDMA 使用路径：
    1) PetPS 直连路径：`BaseParameterClient`（`PetPSClient` / `AllShardsParameterClientWrapper`）
    2) Framework Op-layer 路径：`BasePSClient`（`RDMAPSClientAdapter`，通过 `cache_ps.ps_type=RDMA` 进入）
    两条路径复用同一套 PetPS/RDMA 数据面，但初始化与调用入口不同。

RDMA 模块基于 Mayfly/DSM，提供独立 `petps_server` 数据面。  
本页聚焦“怎么启动、怎么测、怎么排障”，不展开实现细节。

## 路径选择

| 路径 | 入口 | 典型用途 |
|------|------|----------|
| PetPS 直连 | `src/ps/rdma/petps_client.cc`、`src/ps/rdma/allshards_ps_client.cc` | RDMA 协议/传输专项验证、integration 测试 |
| Op-layer RDMA | `src/ps/rdma/rdma_ps_client_adapter.cc` + `KVClientOp` | 通过统一框架接口验证 init/write/read/update/prefetch |

!!! note
    若目标是验证 framework 侧切换，请使用 `cache_ps.ps_type=RDMA` 与
    `src/test/configs/recstore_config.op_rdma.json`。
    若目标是验证 PetPS 传输链路本身，请优先使用 `recstore_config.rdma_test.json`
    或 `recstore_config.rdma_multishard_test.json`。

## 配置

RDMA 专项配置位于：

| 配置文件 | 用途 |
|----------|------|
| `src/test/configs/recstore_config.rdma_test.json` | 单分片测试 |
| `src/test/configs/recstore_config.rdma_multishard_test.json` | 多分片测试 |

!!! note
    `recstore_config.rdma_test.json` / `recstore_config.rdma_multishard_test.json`
    主要用于 PetPS 专项链路测试；Op-layer 验证请使用
    `recstore_config.op_rdma.json`（其中 `cache_ps.ps_type` 为 `RDMA`）。

## 快速验证

!!! note
    本节命令默认在仓库根目录（`/app/RecStore`）执行。

### memcached

```bash
memcached -l 127.0.0.1 -p 21211 -c 10000 -vv
```

`--use-local-memcached` 控制 memcached 来源：

| 参数值 | 行为 |
|--------|------|
| `auto` | 先尝试外部 memcached；如果不可用或 reset 失败，则启动系统 `memcached` 二进制（推荐默认模式） |
| `never` | 只使用已在 `127.0.0.1:21211` 启动的外部 memcached，适用于你需要严格绑定外部 memcached 的场景 |
| `always` | 直接启动系统 `memcached` 二进制 |

!!! note
    推荐优先使用 `--use-local-memcached=auto`，由脚本统一管理 memcached 生命周期。
    如需完整 runner 日志，可追加 `--show-runner-logs`。

### 启动 RDMA Server

为降低 `petps_server` 直接启动时的参数复杂度（`global_id` /
`num_server_processes` / `num_client_processes` 等），推荐使用：

```bash
python3 src/test/scripts/run_petps_server.py \
  --config-path ./src/test/configs/recstore_config.rdma_test.json \
  --use-local-memcached=auto
```

当前脚本实际支持的常用调优参数只有：
`--rdma-per-thread-response-limit-bytes`、
`--rdma-server-ready-timeout-sec`、
`--rdma-server-ready-poll-ms`、
`--rdma-client-receive-arena-bytes`、
`--validate-routing`。

该入口会：

- 根据配置推断 `server-count`（也可显式传 `--server-count`）
- 自动注入 RDMA 所需运行参数
- 统一处理 memcached（`auto/always/never`）

### 单分片 integration

```bash
python3 src/test/scripts/run_petps_integration.py \
  --server-count 1 \
  --config-path ./src/test/configs/recstore_config.rdma_test.json \
  --test-binary ./build/bin/petps_integration_test \
  --gtest-filter=PetPSIntegrationTest.PutGetRoundTripSingleShard:PetPSIntegrationTest.MissingKeysReturnZeroSlots \
  --client-timeout 15 \
  --use-local-memcached=auto
```

!!! note
    这条命令已经按当前代码实际验证通过。

### 多分片 integration

```bash
python3 src/test/scripts/run_petps_integration.py \
  --server-count 2 \
  --config-path ./src/test/configs/recstore_config.rdma_multishard_test.json \
  --test-binary ./build/bin/petps_integration_test \
  --gtest-filter=PetPSIntegrationTest.PutGetRoundTripMultiShard \
  --client-timeout 30 \
  --use-local-memcached=auto
```

### RDMA transport benchmark

```bash
python3 src/test/scripts/run_rdma_transport_benchmarks.py \
  --benchmark-binary ./build/bin/ps_transport_benchmark \
  --iterations 300 \
  --batch-keys 500 \
  --rounds 10 \
  --rdma-warmup-rounds 2 \
  --report-mode summary \
  --rdma-only \
  --rdma-thread-num 1 \
  --rdma-put-protocol-version 2 \
  --rdma-put-v2-transfer-mode read \
  --rdma-wait-timeout-ms 10000 \
  --rdma-client-timeout-sec 60 \
  --use-local-memcached=auto
```

!!! note
    `run_petps_integration.py` 中 `--client-timeout` 与 `--cluster-timeout`
    会按你传入的值生效（要求 > 0）。
    `--report-mode` 支持：
    `summary`（默认，输出聚合延迟与吞吐，日志最少）、
    `per_round`（逐轮延迟）、
    `both`（逐轮 + 聚合）。
    `run_rdma_transport_benchmarks.py` 在三种 transport 全部完成后，
    会额外打印一张 `measure` 阶段汇总表（包含延迟与吞吐）。
    仅关注 RDMA 时可加 `--rdma-only`，跳过 GRPC/BRPC 阶段。
    当前默认 PUT 协议为 v2，benchmark 脚本默认 `--rdma-put-v2-transfer-mode=push`。
    但按当前实现的稳定性，建议优先使用 `read` 模式做基线压测。

### 建议基线

推荐先固定以下组合：

- `--batch-keys=500`
- `--rdma-thread-num=4`
- `--rdma-put-protocol-version=2`
- `--rdma-put-v2-transfer-mode=read`

已知现象（`value_size=16`）：

- 将 `batch-keys` 提升到 `1000` 时，可能触发 Mayfly `MESSAGE_SIZE` 上限并报错
  `messeage size too large`。
- 建议在默认消息大小配置下，将 `batch-keys` 控制在 `500` 附近进行稳定压测。

### benchmark 输出解读

`report-mode=summary` 或 `both` 时，脚本末尾会输出：

- `mean_us / p50_us / p95_us / p99_us`：单轮（一个 round）总耗时统计
- `batch_keys`：单个 Put/Get RPC 内携带的 key 数
- `ops/s`：每秒完成的请求对数量（`Put + Get` 合并统计）
- `key_ops/s`：每秒处理的 key 数量（按每次请求 key 数折算）

当前 `ps_transport_benchmark` 的单轮工作量是：

- 每轮执行 `iterations` 次循环
- 每次循环执行 `1 Put + 1 Get`
- 每次请求处理 `batch_keys` 个 key（默认 4，可通过 `--batch-keys` 调整）

因此吞吐计算可写为：

```text
ops/s = (iterations * 2) / (mean_us / 1e6)
key_ops/s = (iterations * 2 * batch_keys) / (mean_us / 1e6)
```

### ctest 入口

推荐按“冒烟 -> 集成”顺序执行：

```bash
# 1) RDMA 单测冒烟（快）
ctest --test-dir ./build -R "^test_rdma_protocol$|^test_allshards_ps_client$" -VV

# 2) Op-layer RDMA 集成（当前 CI 默认覆盖的核心）
ctest --test-dir ./build -L rdma_integration -VV
```

!!! note
    `rdma_integration` 标签当前至少包含：
    `pytorch_client_test_rdma_basic`、
    `pytorch_client_test_rdma`、
    `pytorch_client_test_rdma_auto`。

若构建时额外启用 `ENABLE_RDMA_INTEGRATION_TESTS=ON`，还可以运行 PetPS 专项集成：

```bash
ctest --test-dir ./build -R "petps_single_shard_test|petps_multi_shard_test" -VV
```

### Op-layer 验证

当 `cache_ps.ps_type` 设置为 `RDMA` 时，framework op layer 会通过
`RDMAPSClientAdapter` 复用 PetPS/RDMA 数据面。可使用现有 PyTorch client
测试验证该配置切换路径：

```bash
ctest --test-dir ./build -R "^pytorch_client_test_rdma_auto$" -VV
```

上述测试使用 `src/test/configs/recstore_config.op_rdma.json`，覆盖
init、write、read、update 与 prefetch 正确性。

如需手工指定 memcached 策略：

```bash
export RECSTORE_USE_LOCAL_MEMCACHED=auto   # 或 always / never
ctest --test-dir ./build -R "^pytorch_client_test_rdma$" -VV
```

## 排障

常见问题优先看这几项：

- 如果卡在 `memcached-wait`，先检查 `127.0.0.1:21211` 是否可达。
- 如果卡在 `startup-wait`，优先看 `petps_server` 是否已启动、是否有残留旧进程。
- 多分片失败时，先核对 `recstore_config.rdma_multishard_test.json` 中的 `num_shards/servers` 与测试参数是否一致。
- 如需看完整 runner 状态日志，可在命令后追加 `--show-runner-logs`。

常用检查命令：

```bash
ss -tnp | grep ':21211'
lsof -nP -iTCP:21211
fuser -v 21211/tcp
```
