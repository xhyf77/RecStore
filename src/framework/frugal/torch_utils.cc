#include "torch_utils.h"

#include "base/string.h"

namespace recstore {

namespace {
template <typename T>
std::string toStringInner(const torch::Tensor& tensor, bool simplified = true) {
  std::stringstream ss;
  if (tensor.dim() == 1) {
    if (simplified) {
      ss << "tensor([";
      for (int i = 0; i < std::min(tensor.size(0), (int64_t)3); ++i) {
        ss << base::SFormat("{},", tensor[i].item<T>());
      }
      ss << base::SFormat("], shape=[{}])", tensor.size(0));
    } else {
      ss << "tensor([";
      for (int i = 0; i < tensor.size(0); ++i) {
        ss << base::SFormat("{},", tensor[i].item<T>());
      }
      ss << "])";
    }
  } else if (tensor.dim() == 2) {
    ss << "tensor([";
    for (int i = 0; i < tensor.size(0); i++) {
      ss << "[";
      for (int j = 0; j < tensor.size(1); j++) {
        ss << base::SFormat("{},", tensor[i][j].item<T>());
      }
      ss << "],";
    }
    ss << base::SFormat("], shape=[{}, {}])", tensor.size(0), tensor.size(1));
  } else {
    LOG(FATAL) << "for dim >=3, not implemented yet";
  }
  return ss.str();
}
} // namespace

std::string toString(const torch::Tensor& tensor, bool simplified) {
  if (tensor.scalar_type() == torch::kFloat32)
    return toStringInner<float>(tensor, simplified);
  else if (tensor.scalar_type() == torch::kInt64)
    return toStringInner<int64_t>(tensor, simplified);
  else if (tensor.scalar_type() == at::ScalarType::Bool)
    return toStringInner<bool>(tensor, simplified);
  else
    LOG(FATAL) << "to String, not supported type " << tensor.scalar_type();
}

std::string
toString(c10::intrusive_ptr<recstore::SlicedTensor> tensor, bool simplified) {
  return toString(tensor->GetSlicedTensor(), simplified);
}

std::string toString(const SubGradTensor& tensor, bool simplified) {
  return toString(tensor.to_tensor(), simplified);
}

} // namespace recstore
