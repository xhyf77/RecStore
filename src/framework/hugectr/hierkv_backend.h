#pragma once

#include "base/json.h"
#include "../third_party/HugeCTR/HugeCTR/include/tensor2.hpp"

namespace recstore {
namespace framework {

class HugeCTRBackend {
public:
  virtual ~HugeCTRBackend() = default;

  virtual void Read(const HugeCTR::Tensor2<long long>& keys,
                    HugeCTR::Tensor2<float>& values) = 0;

  virtual void Update(const HugeCTR::Tensor2<long long>& keys,
                      const HugeCTR::Tensor2<float>& grads) = 0;
};

class HugeCTRHierKVBackend : public HugeCTRBackend {
public:
  explicit HugeCTRHierKVBackend(const json& config);

  void Read(const HugeCTR::Tensor2<long long>& keys,
            HugeCTR::Tensor2<float>& values) override;

  void Update(const HugeCTR::Tensor2<long long>& keys,
              const HugeCTR::Tensor2<float>& grads) override;

private:
  void ValidateConfig(const json& config);

  json config_;
};

} // namespace framework
} // namespace recstore
