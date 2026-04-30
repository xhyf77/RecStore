#pragma once

#include "base/tensor.h"
#include "base/json.h"
#include "../third_party/HugeCTR/HugeCTR/include/tensor2.hpp"

namespace recstore {
namespace framework {

void emb_read_hugectr(const HugeCTR::Tensor2<long long>& keys,
                      HugeCTR::Tensor2<float>& values);

void emb_update_hugectr(const HugeCTR::Tensor2<long long>& keys,
                        const HugeCTR::Tensor2<float>& grads);

enum class HugeCTRBackendKind { RecStore, HierKV };

HugeCTRBackendKind ParseHugeCTRBackendKind(const json& config);

} // namespace framework
} // namespace recstore
