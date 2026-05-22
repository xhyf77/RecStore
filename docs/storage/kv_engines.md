# KV 引擎实现

## 概述

RecStore 提供多种 KV 引擎实现，分别适用于不同的存储配置。所有引擎均继承自 `BaseKV`。

## 引擎列表

| 引擎名称 | 说明 | 文件 |
|---------|------|------|
| KVEngineComposite | 可组合 DRAM 索引 + DRAM/SSD/Tiered 值存储 | engine_composite.h |
| KVEnginePetKV | 基于持久化内存 (PetKV) | engine_petkv.h |

## 各引擎详细说明

### KVEngineComposite

通过 `index.type` + `value.type` 组合 DRAM 索引与 DRAM/SSD/Tiered 值存储，是 YCSB benchmark 与 `test_kvengine` 的默认路径。

??? example "配置示例（DRAM 索引 + SSD 值）"
    ```json
    {
        "index": {"type": "DRAM_PET_HASH"},
        "value": {"type": "SSD_VALUE_STORE", "path": "/data/recstore/value.db",
                  "ssd_allocator": {"type": "SSD_SLAB", "capacity_bytes": 1073741824}},
        "capacity": 1000000,
        "value_size": 128
    }
    ```

### Tiered 值存储

DRAM/SSD 分层值存储不再通过旧的独立 Hybrid 引擎实现，而是作为 `KVEngineComposite` 的 `TIERED_VALUE_STORE` 值存储模式提供。

??? example "配置示例"
    ```json
    {
        "index": {"type": "DRAM_EXTENDIBLE_HASH"},
        "value": {
            "type": "TIERED_VALUE_STORE",
            "dram_allocator": {"type": "PersistLoopShmMalloc"},
            "ssd_allocator": {
                "type": "SSD_SLAB",
                "path": "/data/recstore/value.db",
                "capacity_bytes": 107374182400
            }
        },
        "capacity": 1000000,
        "value_size": 128
    }
    ```

### KVEnginePetKV

基于持久化内存 (Persistent Memory) 的实现。

**特点**

- 使用 PetMultiKV 作为底层存储
- 支持多个 shard，降低锁竞争
- 支持 RDMA 内存注册
- 提供预取优化

**核心组件**

- `PetMultiKV* shm_kv` - 分片 KV 存储
- `shard_num = 16` - 固定 16 个分片

??? example "配置示例"
    ```json
    {
        "path": "/mnt/pmem/recstore/value",
        "capacity": 1000000,
        "value_size": 128
    }
    ```

**预取方法** (通过 FLAGS_prefetch_method 控制)
- `0`: 逐个 Get
- `1`: 使用 BatchGet 预取

## 线程安全

各引擎的线程安全实现：

| 引擎 | 同步机制 |
|------|---------|
| KVEngineComposite | per-key stripe `shared_mutex` |
| PetKV | 内部分片锁 |

## 工厂注册

所有引擎通过宏注册到工厂：

```cpp
FACTORY_REGISTER(BaseKV, KVEngineComposite, KVEngineComposite, const BaseKVConfig&);
FACTORY_REGISTER(BaseKV, KVEnginePetKV, ...);
```

使用 `#include "storage/kv_engine/engine_factory.h"` 即可完成所有引擎的注册。
