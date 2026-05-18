# BaseKV 与配置

`BaseKV` 是参数服务器看到的 KV 接口。内置组合式存储挂在这个接口下，上层代码不需要知道索引和值的存放位置。

## 接口语义

```cpp
class BaseKV {
 public:
  virtual void Get(uint64_t key, std::string& value, unsigned tid) = 0;
  virtual void Put(uint64_t key, const std::string_view& value, unsigned tid) = 0;
  virtual void BatchGet(base::ConstArray<uint64_t> keys,
                        std::vector<base::ConstArray<float>>* values,
                        unsigned tid) = 0;
  virtual void BatchPut(base::ConstArray<uint64_t> keys,
                        std::vector<base::ConstArray<float>>* values,
                        unsigned tid);
};
```

`Get` 未命中时清空 `value`。`BatchGet` 的返回值是 `ConstArray<float>` 视图：DRAM value 可能直接指向底层内存，SSD value 先读到线程局部 buffer。调用方不能把这些视图跨调用长期保存。

`tid` 保留在接口里，但 `KVEngineComposite` 主要依赖 key stripe lock 和组件内部同步。新增实现不能假设 `tid` 全局唯一。

## 配置入口

服务端配置通常写在 `cache_ps.base_kv_config`：

??? example "服务端配置示例"

    ```json
    {
      "cache_ps": {
        "num_threads": 32,
        "base_kv_config": {
          "capacity": 1000000,
          "index": {
            "type": "DRAM_EXTENDIBLE_HASH"
          },
          "value": {
            "type": "DRAM_VALUE_STORE",
            "default_value_size_hint": 512,
            "path": "/dev/shm/recstore_data/value",
            "dram_allocator": {
              "type": "PERSIST_LOOP_SLAB",
              "capacity_bytes": 512000000
            }
          }
        }
      }
    }
    ```

`CachePS` 把 `num_threads` 写入 `BaseKVConfig::num_threads_`，把 `base_kv_config` 写入 `BaseKVConfig::json_config_`，调用：

```cpp
auto resolved = base::ResolveEngine(kv_config);
auto* kv = base::Factory<BaseKV, const BaseKVConfig&>::NewInstance(
    resolved.engine, resolved.cfg);
```

嵌套配置解析为 `KVEngineComposite`。旧的顶层字段会被拒绝，包括 `path`、`index_type`、`value_type`、`value_size`、`io_backend_type`、`file_path`。

## 必填字段

| 字段 | 说明 |
|------|------|
| `capacity` | 索引容量或容量估计。`DRAM_PET_HASH` 会直接用它分配 hash 表。 |
| `index.type` | 索引实现名。合法值见 [kv_engines.md](./kv_engines.md)。 |
| `value.type` | value store 实现名。合法值见 [kv_engines.md](./kv_engines.md)。 |
| `value.default_value_size_hint` | `BulkLoad` 所需的固定 value 字节数。`Put` / `BatchPut` 可写变长值。 |

路径规则由 `ResolveEngine` 检查：

| 配置 | 路径要求 |
|------|----------|
| `DRAM_VALUE_STORE` | `ResolveEngine` 允许 `value.path` 为空或以 `/dev/shm` 开头；`DramValueStore` 构造函数目前需要非空路径，示例使用 `/dev/shm/...`。 |
| `SSD_VALUE_STORE` | `value.path` 必填且非空。 |
| `TIERED_VALUE_STORE` | 不允许 `value.path`；DRAM 路径写在 `value.dram_allocator.path`，SSD 路径写在 `value.ssd_allocator.path`。 |
| SSD index | `index.path` 必填且非空。 |

## 外部引擎

`external_engine_type` 只用于显式选择外部 KV 适配层：

```json
{
  "external_engine_type": "KVEngineHPSRocksDB",
  "path": "/data/rocksdb",
  "capacity": 1000000,
  "value_size": 512
}
```

允许的显式外部引擎是 `KVEngineFasterKV`、`KVEngineHPSHashMap`、`KVEngineHPSRocksDB`。它们不走 `KVEngineComposite` 的 `index/value` 组合规则。

旧字段 `engine_type` 仍作为兼容别名被接受，但新配置应使用 `external_engine_type`。如果两个字段同时出现，值必须一致。
