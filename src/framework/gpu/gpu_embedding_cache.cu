#include "framework/gpu/gpu_embedding_cache.h"

#include <chrono>
#include <limits>
#include <memory>
#include <mutex>

#include <ATen/cuda/CUDAContext.h>
#include <c10/cuda/CUDAGuard.h>
#include <cuda_runtime_api.h>

#include "storage/gpu_cache/nv_gpu_cache.hpp"

namespace recstore::framework::gpu {
namespace {

using CacheImpl = gpu_cache::gpu_cache<int64_t,
                                       uint64_t,
                                       std::numeric_limits<int64_t>::max(),
                                       SET_ASSOCIATIVITY,
                                       SLAB_SIZE>;

std::mutex g_mu;
std::unique_ptr<CacheImpl> g_cache;
int64_t g_capacity = 0;
int64_t g_embedding_dim = 0;
int g_device_index = -1;
thread_local GpuCacheProfile g_last_profile;

std::chrono::steady_clock::time_point Now() {
  return std::chrono::steady_clock::now();
}

double MsSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
             Now() - start)
      .count();
}

void RequireCudaTensor(const torch::Tensor& tensor, const char* name) {
  TORCH_CHECK(tensor.is_cuda(), name, " must be a CUDA tensor");
  TORCH_CHECK(tensor.is_contiguous(), name, " must be contiguous");
}

void ValidateCacheMutationTensors(const torch::Tensor& keys,
                                  const torch::Tensor& values) {
  RequireCudaTensor(keys, "keys");
  TORCH_CHECK(keys.scalar_type() == torch::kInt64, "keys must have dtype int64");
  RequireCudaTensor(values, "values");
  TORCH_CHECK(values.scalar_type() == torch::kFloat32,
              "values must have dtype float32");
  TORCH_CHECK(values.dim() == 2, "values must be a 2-D tensor");
  TORCH_CHECK(keys.size(0) == values.size(0),
              "keys and values row count mismatch");
  TORCH_CHECK(values.size(1) == g_embedding_dim,
              "values embedding_dim mismatch");
}

void RequireCacheDevice(const torch::Tensor& tensor, const char* name) {
  TORCH_CHECK(g_device_index >= 0, "gpu cache device is not initialized");
  TORCH_CHECK(tensor.device().index() == g_device_index,
              name,
              " must be on gpu cache device cuda:",
              g_device_index,
              ", got ",
              tensor.device());
}

void ResetCacheOnRecordedDeviceLocked() {
  if (g_cache && g_device_index >= 0) {
    c10::cuda::CUDAGuard device_guard(g_device_index);
    g_cache.reset();
    return;
  }
  g_cache.reset();
}

} // namespace

bool EnableGpuCache(int64_t capacity, int64_t embedding_dim) {
  TORCH_CHECK(capacity > 0, "gpu cache capacity must be positive");
  TORCH_CHECK(embedding_dim > 0, "gpu cache embedding_dim must be positive");
  std::lock_guard<std::mutex> guard(g_mu);
  const int device_index = c10::cuda::current_device();
  if (g_cache && g_capacity == capacity && g_embedding_dim == embedding_dim &&
      g_device_index == device_index) {
    return true;
  }
  ResetCacheOnRecordedDeviceLocked();
  {
    c10::cuda::CUDAGuard device_guard(device_index);
    g_cache = std::make_unique<CacheImpl>(
        static_cast<size_t>(capacity), static_cast<size_t>(embedding_dim));
  }
  g_capacity = capacity;
  g_embedding_dim = embedding_dim;
  g_device_index = device_index;
  return true;
}

void DisableGpuCache() {
  std::lock_guard<std::mutex> guard(g_mu);
  ResetCacheOnRecordedDeviceLocked();
  g_capacity = 0;
  g_embedding_dim = 0;
  g_device_index = -1;
  g_last_profile = GpuCacheProfile{};
}

void ClearGpuCache() {
  std::lock_guard<std::mutex> guard(g_mu);
  if (!g_cache) {
    g_last_profile = GpuCacheProfile{};
    return;
  }
  TORCH_CHECK(g_capacity > 0, "gpu cache capacity must be positive");
  TORCH_CHECK(g_embedding_dim > 0, "gpu cache embedding_dim must be positive");
  TORCH_CHECK(g_device_index >= 0, "gpu cache device is not initialized");
  ResetCacheOnRecordedDeviceLocked();
  {
    c10::cuda::CUDAGuard device_guard(g_device_index);
    g_cache = std::make_unique<CacheImpl>(
        static_cast<size_t>(g_capacity), static_cast<size_t>(g_embedding_dim));
  }
  g_last_profile = GpuCacheProfile{};
}

bool IsGpuCacheEnabled() {
  std::lock_guard<std::mutex> guard(g_mu);
  return g_cache != nullptr;
}

GpuCacheProfile GetLastGpuCacheProfile() { return g_last_profile; }

void ResetLastGpuCacheProfile() { g_last_profile = GpuCacheProfile{}; }

bool CanUseGpuCache(const torch::Tensor& keys, int64_t embedding_dim) {
  std::lock_guard<std::mutex> guard(g_mu);
  return g_cache != nullptr && keys.is_cuda() && keys.is_contiguous() &&
         keys.scalar_type() == torch::kInt64 &&
         keys.device().index() == g_device_index &&
         embedding_dim == g_embedding_dim;
}

GpuCacheLookupResult QueryGpuCache(const torch::Tensor& keys,
                                   int64_t embedding_dim) {
  std::lock_guard<std::mutex> guard(g_mu);
  RequireCudaTensor(keys, "keys");
  TORCH_CHECK(keys.scalar_type() == torch::kInt64, "keys must have dtype int64");
  TORCH_CHECK(g_cache != nullptr, "gpu cache is not enabled");
  TORCH_CHECK(embedding_dim == g_embedding_dim,
              "gpu cache embedding_dim mismatch");
  RequireCacheDevice(keys, "keys");

  c10::cuda::CUDAGuard device_guard(keys.device());
  const auto query_start = Now();
  const auto stream      = at::cuda::getCurrentCUDAStream();
  auto options = keys.options().dtype(torch::kFloat32);
  auto values  = torch::empty({keys.numel(), embedding_dim}, options);
  auto missing_index =
      torch::empty({keys.numel()}, keys.options().dtype(torch::kInt64));
  auto missing_keys = torch::empty_like(keys);

  size_t* missing_len_device = nullptr;
  C10_CUDA_CHECK(
      cudaMalloc(reinterpret_cast<void**>(&missing_len_device), sizeof(size_t)));
  try {
    C10_CUDA_CHECK(
        cudaMemsetAsync(missing_len_device, 0, sizeof(size_t), stream.stream()));
    g_cache->Query(keys.data_ptr<int64_t>(),
                   static_cast<size_t>(keys.numel()),
                   values.data_ptr<float>(),
                   reinterpret_cast<uint64_t*>(missing_index.data_ptr<int64_t>()),
                   missing_keys.data_ptr<int64_t>(),
                   missing_len_device,
                   stream.stream());
    size_t missing_count_host = 0;
    C10_CUDA_CHECK(cudaMemcpyAsync(&missing_count_host,
                                   missing_len_device,
                                   sizeof(size_t),
                                   cudaMemcpyDeviceToHost,
                                   stream.stream()));
    C10_CUDA_CHECK(cudaStreamSynchronize(stream.stream()));
    C10_CUDA_CHECK(cudaFree(missing_len_device));
    missing_len_device = nullptr;

    TORCH_CHECK(missing_count_host <= static_cast<size_t>(keys.numel()),
                "gpu cache returned invalid missing count");
    const int64_t missing_count = static_cast<int64_t>(missing_count_host);
    auto missing_keys_cpu =
        missing_keys.slice(0, 0, missing_count).to(torch::kCPU);
    auto missing_positions_cpu =
        missing_index.slice(0, 0, missing_count).to(torch::kCPU);

    g_last_profile.query_ms += MsSince(query_start);
    g_last_profile.hit_count +=
        static_cast<double>(keys.numel() - missing_count);
    g_last_profile.request_count += static_cast<double>(keys.numel());
    g_last_profile.miss_count += static_cast<double>(missing_count);

    return GpuCacheLookupResult{
        values,
        missing_keys_cpu,
        missing_positions_cpu,
        missing_count,
    };
  } catch (...) {
    if (missing_len_device != nullptr) {
      cudaFree(missing_len_device);
    }
    throw;
  }
}

void FillGpuCache(const torch::Tensor& keys_cuda,
                  const torch::Tensor& values_cuda) {
  if (keys_cuda.numel() == 0) {
    return;
  }
  std::lock_guard<std::mutex> guard(g_mu);
  if (!g_cache) {
    return;
  }
  ValidateCacheMutationTensors(keys_cuda, values_cuda);
  RequireCacheDevice(keys_cuda, "keys_cuda");
  RequireCacheDevice(values_cuda, "values_cuda");

  c10::cuda::CUDAGuard device_guard(keys_cuda.device());
  const auto fill_start = Now();
  const auto stream     = at::cuda::getCurrentCUDAStream();
  g_cache->Replace(keys_cuda.data_ptr<int64_t>(),
                   static_cast<size_t>(keys_cuda.numel()),
                   values_cuda.data_ptr<float>(),
                   stream.stream());
  C10_CUDA_CHECK(cudaStreamSynchronize(stream.stream()));
  g_last_profile.fill_ms += MsSince(fill_start);
}

void ScatterMissValues(torch::Tensor* output_values,
                       const torch::Tensor& missing_positions_cpu,
                       const torch::Tensor& miss_values_cuda) {
  if (miss_values_cuda.numel() == 0) {
    return;
  }
  TORCH_CHECK(output_values != nullptr, "output_values must not be null");
  RequireCudaTensor(*output_values, "output_values");
  RequireCudaTensor(miss_values_cuda, "miss_values_cuda");
  TORCH_CHECK(missing_positions_cpu.device().is_cpu(),
              "missing_positions_cpu must be a CPU tensor");
  TORCH_CHECK(missing_positions_cpu.scalar_type() == torch::kInt64,
              "missing_positions_cpu must have dtype int64");
  c10::cuda::CUDAGuard device_guard(output_values->device());
  auto positions_cuda =
      missing_positions_cpu.to(output_values->device(), /*non_blocking=*/true);
  output_values->index_copy_(0, positions_cuda.to(torch::kLong), miss_values_cuda);
}

void AddGpuCacheBackendLookupMs(double ms) {
  g_last_profile.backend_lookup_ms += ms;
}

void UpdateGpuCache(const torch::Tensor& keys_cuda,
                    const torch::Tensor& values_cuda) {
  if (keys_cuda.numel() == 0) {
    return;
  }
  std::lock_guard<std::mutex> guard(g_mu);
  if (!g_cache) {
    return;
  }
  ValidateCacheMutationTensors(keys_cuda, values_cuda);
  RequireCacheDevice(keys_cuda, "keys_cuda");
  RequireCacheDevice(values_cuda, "values_cuda");

  c10::cuda::CUDAGuard device_guard(keys_cuda.device());
  const auto update_start = Now();
  const auto stream       = at::cuda::getCurrentCUDAStream();
  g_cache->Update(keys_cuda.data_ptr<int64_t>(),
                  static_cast<size_t>(keys_cuda.numel()),
                  values_cuda.data_ptr<float>(),
                  stream.stream());
  C10_CUDA_CHECK(cudaStreamSynchronize(stream.stream()));
  g_last_profile.update_ms += MsSince(update_start);
}

void InvalidateGpuCache(const torch::Tensor& keys_cuda) {
  if (keys_cuda.numel() == 0) {
    return;
  }
  std::lock_guard<std::mutex> guard(g_mu);
  if (!g_cache) {
    return;
  }
  RequireCudaTensor(keys_cuda, "keys_cuda");
  TORCH_CHECK(keys_cuda.scalar_type() == torch::kInt64,
              "keys_cuda must have dtype int64");
  RequireCacheDevice(keys_cuda, "keys_cuda");

  c10::cuda::CUDAGuard device_guard(keys_cuda.device());
  const auto invalidate_start = Now();
  const auto stream           = at::cuda::getCurrentCUDAStream();
  g_cache->Remove(keys_cuda.data_ptr<int64_t>(),
                  static_cast<size_t>(keys_cuda.numel()),
                  stream.stream());
  C10_CUDA_CHECK(cudaStreamSynchronize(stream.stream()));
  g_last_profile.invalidate_ms += MsSince(invalidate_start);
}

} // namespace recstore::framework::gpu
