# RecStore 原生 GPU Cache Invalidation 设计

## 背景

当前 `feat/gpu-training-cache` 分支已经在 PyTorch op 和模型层之间加入 GPU training cache。实验显示 64K row cache 与 no-cache 基本持平，8K row cache 变慢。主要原因是当前实现为了保持 write-through 一致性，在 sparse update 成功后会重新从后端 lookup 更新后的 embedding，再拷回 GPU 并刷新 cache。这条维护路径抵消了 lookup 命中的收益。

本设计目标是保留 RecStore 原生 GPU cache 方向，不依赖 HierKV，不把 HierKV 作为 cache 后端。第一版改为 read-only hot cache + update invalidation，用更小的写路径成本验证 lookup cache 是否能带来端到端收益。

## 目标

- GPU cache 位于 PyTorch op 与模型 embedding module 之间。
- GPU cache 是 RecStore 原生组件，不经过 HierKV。
- lookup 命中直接返回 GPU resident embedding values。
- lookup miss 继续走现有 RecStore 后端，再将 miss rows fill 到 GPU cache。
- sparse update 成功后只 invalidate 本次更新涉及的 keys，不主动读回刷新 cache。
- 保证下一次 lookup 不会返回 update 前的旧 cached value。

## 非目标

- 不实现 dirty value 合并。
- 不实现异步 overlap 或跨 step 延迟 flush。
- 不替换 RecStore 后端存储语义。
- 不接入 NVIDIA HierarchicalKV 的 `nv::merlin::HashTable`。
- 不改变模型层 sparse optimizer 的外部 API。

## 数据流

### Lookup

1. 模型层传入 CUDA ids 到 `local_lookup_flat`。
2. op 层检查 GPU cache 是否启用、device 是否匹配、embedding dim 是否匹配。
3. 查询 GPU cache。
4. 对 hit rows，直接使用 cache 返回的 CUDA values。
5. 对 miss rows，将 missing keys 交给现有 RecStore 后端 lookup。
6. 将 miss values scatter 回输出 CUDA tensor。
7. 将 miss keys 和 miss values fill 到 GPU cache。

### Update

1. 模型层 sparse optimizer 调用 `local_update_flat(table_name, ids, grads)`。
2. op 层按现有路径将 update 提交给 RecStore 后端。
3. 后端 update 成功后，op 层调用 GPU cache invalidate。
4. invalidate 只移除或标记本次 update keys 对应的 cache entries。
5. 不再为了刷新 cache 额外执行后端 lookup。

### Table/Write 边界

- `init_data`、`emb_write`、`push`、切换 table 时继续 clear cache。
- 第一版 cache 仍按单 active table 处理，避免跨 table key 冲突。
- 发生 cache 维护异常时，清空 cache 并继续依赖后端结果，避免返回脏值。

## 组件变更

### `src/framework/gpu/gpu_embedding_cache.*`

新增 native invalidate API：

- `void InvalidateGpuCache(const torch::Tensor& keys_cuda);`
- profile 增加 `invalidate_ms`。

实现要求：

- 输入必须是 CUDA contiguous int64 tensor，device 必须匹配 cache device。
- invalidate 对不存在的 key 是 no-op。
- invalidate 后同一 key 的后续 `QueryGpuCache` 必须 miss。
- 如果底层 `gpu_cache` 不支持直接删除，第一版允许用 tombstone/valid-bit side table 或重建为支持 erase 的结构；不能通过读取后端刷新来模拟 invalidate。

### `src/framework/pytorch/op_torch.cc`

修改 `local_update_flat_torch`：

- 保留现有后端 update。
- 删除 update 后的 `BackendLocalLookupFlat` refresh。
- 后端 update 成功后，如果 GPU cache 可用且 ids 在 CUDA 上，则调用 `InvalidateGpuCache(keys)`。
- 如果 ids 不在 CUDA 上但 cache 已启用，第一版采用 clear cache 的保守策略。

### Python wrapper / runner

- `KVClient.get_last_gpu_cache_profile()` 增加 invalidate 字段。
- `rs_demo` CSV 增加 `update_gpu_cache_invalidate_ms`。
- 保留已有 `gpu_cache_query_ms`、`gpu_cache_backend_lookup_ms`、`gpu_cache_fill_ms`、`gpu_cache_hit_count`。
- 增加 request count / miss count，避免只看 hit count。

## 正确性语义

第一版提供以下语义：

- Cache hit 返回的是最近一次未被 invalidated 的后端值。
- 对某个 key 的 update 成功后，该 key 在 GPU cache 中立即不可命中。
- 后续 lookup 会从后端读取 update 后的值，并重新 fill cache。
- 如果 invalidate 失败，必须 clear 整个 cache 或抛错；不能静默保留旧值。

这个语义比 write-through 少一次 update 后 refresh，但仍避免读到 update 前旧值。

## 测试计划

### 单元测试

- lookup miss 后 fill，第二次 lookup hit。
- update cached key 后，下一次 lookup miss，并返回后端更新后的值。
- invalidate 不存在的 key 不影响其他 cached key。
- duplicate ids update 后，对应 key 均不可命中。
- CPU ids update 且 cache 启用时，cache 被 clear。
- table switch / push / init 后 cache clear 行为保持不变。

### 性能验证

复用当前 `rs_demo` 参数，至少比较：

- no-cache
- gpu-cache-8k-invalidate
- gpu-cache-64k-invalidate

重点观察：

- `step_total_ms`
- `embed_lookup_local_ms`
- `sparse_update_ms`
- `lookup_gpu_cache_query_ms`
- `lookup_gpu_cache_miss_count`
- `update_gpu_cache_invalidate_ms`

预期第一版至少应降低 `sparse_update_ms` 相对当前 write-through cache 的额外成本。如果 lookup query 仍保持约 2.4 ms，则端到端收益仍可能有限，但瓶颈会更清晰地集中到 query 同步和 hit rate。

## 风险与后续

- 如果 workload 是 read-after-update 高频模式，invalidate 会降低下一次 lookup hit rate。
- 当前 query 路径仍有 host missing-count 同步；invalidate 方案不解决这个读路径同步成本。
- 如果底层 GPU cache erase 成本较高，可能需要改为 valid-bit/tombstone 方案。
- 后续可在 invalidate 方案稳定后再评估 GPU-side dirty delta、异步 query、或真正 GPU resident optimizer。
