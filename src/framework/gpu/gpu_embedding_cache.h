#pragma once

#include <cstdint>

#include <torch/extension.h>

namespace recstore::framework::gpu {

struct GpuCacheProfile {
  double query_ms = 0.0;
  double backend_lookup_ms = 0.0;
  double fill_ms = 0.0;
  double update_ms = 0.0;
  double hit_count = 0.0;
};

bool EnableGpuCache(int64_t capacity, int64_t embedding_dim);
void DisableGpuCache();
void ClearGpuCache();
bool IsGpuCacheEnabled();
GpuCacheProfile GetLastGpuCacheProfile();
void ResetLastGpuCacheProfile();

bool CanUseGpuCache(const torch::Tensor& keys, int64_t embedding_dim);

struct GpuCacheLookupResult {
  torch::Tensor values;
  torch::Tensor missing_keys_cpu;
  torch::Tensor missing_positions_cpu;
  int64_t missing_count = 0;
};

GpuCacheLookupResult QueryGpuCache(const torch::Tensor& keys,
                                   int64_t embedding_dim);
void FillGpuCache(const torch::Tensor& keys_cuda,
                  const torch::Tensor& values_cuda);
void ScatterMissValues(torch::Tensor* output_values,
                       const torch::Tensor& missing_positions_cpu,
                       const torch::Tensor& miss_values_cuda);
void AddGpuCacheBackendLookupMs(double ms);
void UpdateGpuCache(const torch::Tensor& keys_cuda,
                    const torch::Tensor& values_cuda);

} // namespace recstore::framework::gpu
