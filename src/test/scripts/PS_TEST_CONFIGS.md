# PS Test Configs

本目录中的参数服务器测试配置按角色划分如下：

## 默认主配置

- `../../recstore_config.json`

角色：

- 默认 GRPC 主配置
- 面向通用开发和默认主路径验证

## RDMA 专项配置

- `recstore_config.rdma_test.json`
  - RDMA 单分片 integration / benchmark server 配置
- `recstore_config.rdma_multishard_test.json`
  - RDMA 多分片 integration 配置

约束：

- RDMA 测试不要回落到根目录默认 `recstore_config.json`

## BRPC 专项配置

- `recstore_config.brpc.json`

角色：

- transport benchmark 中的 BRPC 专项配置

## 脚本入口

- `run_petps_integration.py`
  - 根据 `server-count` 自动选择 RDMA 单分片或多分片配置
- `run_rdma_transport_benchmarks.py`
  - RDMA 使用单分片 RDMA 专项配置
  - GRPC / BRPC 从各自配置中的 `client.host` / `client.port` 解析 endpoint

## 共享配置模块

- `ps_test_config.py`

统一维护：

- 配置路径常量
- RDMA integration 默认配置选择逻辑
- benchmark endpoint 解析逻辑
