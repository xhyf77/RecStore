#pragma once

#include <string>

namespace petps {

enum class RdmaTransportMode {
  kRawMessage,
  kDescriptorDoorbell,
};

inline bool ParseRdmaTransportMode(const std::string& value,
                                   RdmaTransportMode* mode,
                                   std::string* error) {
  if (mode == nullptr) {
    if (error != nullptr) {
      *error = "mode output is null";
    }
    return false;
  }
  if (value == "raw_message") {
    *mode = RdmaTransportMode::kRawMessage;
    return true;
  }
  if (value == "descriptor_doorbell") {
    *mode = RdmaTransportMode::kDescriptorDoorbell;
    return true;
  }
  if (error != nullptr) {
    *error = "unknown RDMA transport mode: " + value;
  }
  return false;
}

} // namespace petps
