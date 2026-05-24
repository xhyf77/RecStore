# 计算层接口

这一页记录计算层的边界接口：Python client、PyTorch op、C++ facade。模型代码通常只用 `RecStoreClient` 或 embedding 模块，不直接碰 `KVClientOp`。

## Python client

文件：`src/python/pytorch/recstore/KVClient.py`

```python
from recstore.KVClient import RecStoreClient

client = RecStoreClient(library_path="/path/to/lib_recstore_ops.so")
```

`RecStoreClient` 按 role 缓存实例。未传 `library_path` 时，代码按构建目录约定查找 `lib_recstore_ops.so`。

| API | 作用 |
|-----|------|
| `init_data(name, shape, dtype, init_func=None)` | 初始化后端表，记录本地元数据，写入初始值。 |
| `pull(name, ids)` | 读取 `[N, D]` embedding。 |
| `push(name, ids, data)` | 写入 `[N, D]` embedding。 |
| `prefetch(ids)` / `wait_for_prefetch(handle)` | 异步预取。 |
| `update(name, ids, grads)` | 发送 sparse gradients。 |
| `set_ps_config(host, port)` | 重建底层 PS client。 |
| `set_ps_backend(backend)` | 切换 RPC、本地共享内存或 HierKV 路径。 |

??? warning "delete_data 的边界"

    `delete_data` 只清理 Python 本地元数据，不删除后端数据。不要把它当成存储清理接口。

## PyTorch op

文件：`src/framework/pytorch/op_torch.cc`

| op | 作用 |
|----|------|
| `emb_read(keys, embedding_dim)` | 同步读取，返回 `[N, D]`。 |
| `emb_write(keys, values)` | 同步写入。 |
| `emb_update_table(table, keys, grads)` | 带表名更新。 |
| `init_embedding_table(name, num_embeddings, embedding_dim)` | 创建后端表。 |
| `emb_prefetch(keys)` / `emb_wait_result(handle, embedding_dim)` | 预取。 |
| `set_ps_config(host, port)` / `set_ps_backend(backend)` | 连接与后端选择。 |
| `local_lookup_flat(keys, embedding_dim)` | 本地快路径读取。 |
| `local_update_flat(table, keys, grads)` | 本地快路径更新。 |

张量要求：

| 参数 | 要求 |
|------|------|
| `keys` | int64，1D，contiguous。 |
| `values` | float32，2D，contiguous，行数等于 `keys.numel()`。 |
| `grads` | float32，2D，contiguous。 |
| `embedding_dim` | 大于 0。 |

CUDA tensor 会复制到 CPU 后调用 `CommonOp`。读取结果按输入设备返回。

## CommonOp / KVClientOp

文件：`src/framework/op.h`、`src/framework/op.cc`

```cpp
class CommonOp {
 public:
  virtual void EmbRead(const RecTensor& keys, RecTensor& values) = 0;
  virtual void EmbWrite(const RecTensor& keys, const RecTensor& values) = 0;
  virtual void EmbUpdate(const std::string& table_name,
                         const RecTensor& keys,
                         const RecTensor& grads) = 0;
  virtual bool InitEmbeddingTable(const std::string& table_name,
                                  const EmbeddingTableConfig& config) = 0;
};
```

`KVClientOp` 是默认实现。它根据配置选择远程 PS client 或本地 HierKV runtime。

??? note "新增接口时要改哪些文件"

    至少同步修改 `op.h`、`op.cc`、`op_torch.cc`。mock 路径也要补齐，否则 Python 单测可能只覆盖假实现。

??? warning "占位接口"

    `EmbWriteAsync`、`IsWriteDone`、`WaitForWrite`、`SaveToFile`、`LoadFromFile` 仍是占位接口。不要写成可用能力。
