# YCSB 运行说明

YCSB 用于给 RecStore 的 KV 后端和 YCSB 自带的基准流程做本地负载验证。当前仓库里的入口在 `third_party/ycsb/core/ycsbc.cc`，顶层 CMake 会注册两个目标：

- `ycsb_basic`：只使用 YCSB 自带的 `basic` DB，适合快速确认 YCSB 解析 workload、执行 load/run 和打印指标。
- `ycsb`：链接 RecStore KV 后端，支持 `-db kvdb` 和 `-db cceh`，用于实际后端压测。

## 前置条件

先在仓库根目录完成 CMake 配置，并确保 `build/` 已存在：

```bash
cmake -S . -B build
```

## 快速 smoke

这个命令不依赖 RecStore 后端，适合先确认 YCSB 本身可构建可运行：

```bash
cmake --build build --target ycsb_basic -j
./build/bin/ycsb_basic -load -run -db basic \
  -P third_party/ycsb/workloads/workloada \
  -p recordcount=10 -p operationcount=10 \
  -p measurementtype=basic -p basic.silent=true
```

## RecStore KV 后端

构建 RecStore 绑定目标：

```bash
cmake --build build --target ycsb -j
```

运行 HybridKV 绑定：

```bash
./build/bin/ycsb -load -run -db kvdb \
  -P third_party/ycsb/workloads/workloada \
  -P third_party/ycsb/db/kv_db.properties \
  -p recordcount=1000 -p operationcount=1000 \
  -p measurementtype=basic
```

运行 CCEH 绑定：

```bash
./build/bin/ycsb -load -run -db cceh \
  -P third_party/ycsb/workloads/workloada \
  -P third_party/ycsb/db/cceh.properties \
  -p recordcount=1000 -p operationcount=1000 \
  -p measurementtype=basic
```

`ycsb` 目标依赖当前仓库的 RecStore 存储目标。如果存储目标自身编译失败，需要先修复对应范围内的编译错误，再运行后端压测。
