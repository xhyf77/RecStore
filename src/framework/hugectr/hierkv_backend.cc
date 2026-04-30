#include "framework/hugectr/hierkv_backend.h"

#include <stdexcept>

namespace recstore {
namespace framework {

namespace {

void ValidateTensorRank(const std::vector<size_t>& dims,
                        size_t expected_rank,
                        const char* tensor_name) {
  if (dims.size() != expected_rank) {
    throw std::invalid_argument(
        std::string(tensor_name) + " tensor must be " +
        std::to_string(expected_rank) + "-dimensional.");
  }
}

} // namespace

HugeCTRHierKVBackend::HugeCTRHierKVBackend(const json& config)
    : config_(config) {
  ValidateConfig(config_);
}

void HugeCTRHierKVBackend::ValidateConfig(const json& config) {
  if (!config.contains("hierkv") || !config["hierkv"].is_object()) {
    throw std::invalid_argument(
        "HierKV backend selected but missing 'hierkv' config block.");
  }

  const json& hierkv = config["hierkv"];
  if (!hierkv.contains("max_capacity")) {
    throw std::invalid_argument(
        "HierKV backend selected but missing 'hierkv.max_capacity'.");
  }
  if (!hierkv.contains("max_hbm_for_vectors")) {
    throw std::invalid_argument(
        "HierKV backend selected but missing 'hierkv.max_hbm_for_vectors'.");
  }
  if (!hierkv.contains("dim")) {
    throw std::invalid_argument(
        "HierKV backend selected but missing 'hierkv.dim'.");
  }
  if (!hierkv["max_capacity"].is_number_unsigned() &&
      !hierkv["max_capacity"].is_number_integer()) {
    throw std::invalid_argument("'hierkv.max_capacity' must be an integer.");
  }
  if (!hierkv["max_hbm_for_vectors"].is_number_unsigned() &&
      !hierkv["max_hbm_for_vectors"].is_number_integer()) {
    throw std::invalid_argument(
        "'hierkv.max_hbm_for_vectors' must be an integer.");
  }
  if (!hierkv["dim"].is_number_unsigned() &&
      !hierkv["dim"].is_number_integer()) {
    throw std::invalid_argument("'hierkv.dim' must be an integer.");
  }
  if (hierkv["max_capacity"].get<int64_t>() <= 0) {
    throw std::invalid_argument("'hierkv.max_capacity' must be positive.");
  }
  if (hierkv["max_hbm_for_vectors"].get<int64_t>() < 0) {
    throw std::invalid_argument(
        "'hierkv.max_hbm_for_vectors' must be non-negative.");
  }
  if (hierkv["dim"].get<int64_t>() <= 0) {
    throw std::invalid_argument("'hierkv.dim' must be positive.");
  }
}

void HugeCTRHierKVBackend::Read(const HugeCTR::Tensor2<long long>& keys,
                                HugeCTR::Tensor2<float>& values) {
  ValidateTensorRank(keys.get_dimensions(), 1, "Keys");
  ValidateTensorRank(values.get_dimensions(), 2, "Values");
  const auto expected_dim = config_["hierkv"]["dim"].get<size_t>();
  const auto& values_dims = values.get_dimensions();
  if (values_dims[1] != expected_dim) {
    throw std::invalid_argument(
        "Values tensor embedding dim does not match hierkv.dim.");
  }
  throw std::runtime_error(
      "HierKV backend selected but read path is not implemented yet.");
}

void HugeCTRHierKVBackend::Update(const HugeCTR::Tensor2<long long>& keys,
                                  const HugeCTR::Tensor2<float>& grads) {
  ValidateTensorRank(keys.get_dimensions(), 1, "Keys");
  ValidateTensorRank(grads.get_dimensions(), 2, "Grads");
  const auto expected_dim = config_["hierkv"]["dim"].get<size_t>();
  const auto& grads_dims  = grads.get_dimensions();
  if (grads_dims[1] != expected_dim) {
    throw std::invalid_argument(
        "Grads tensor embedding dim does not match hierkv.dim.");
  }
  throw std::runtime_error(
      "HierKV backend selected but update path is not implemented yet.");
}

} // namespace framework
} // namespace recstore
