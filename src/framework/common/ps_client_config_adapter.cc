#include "framework/common/ps_client_config_adapter.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace recstore {

std::string NormalizePSType(std::string ps_type) {
  std::transform(
      ps_type.begin(), ps_type.end(), ps_type.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
      });
  return ps_type;
}

PSClientType ResolveFrameworkPSClientType(const json& config) {
  std::string ps_type = "GRPC";
  if (config.contains("cache_ps") && config["cache_ps"].contains("ps_type")) {
    ps_type = config["cache_ps"]["ps_type"].get<std::string>();
  }

  const std::string normalized = NormalizePSType(ps_type);
  if (normalized == "GRPC") {
    return PSClientType::kGrpc;
  }
  if (normalized == "BRPC") {
    return PSClientType::kBrpc;
  }
  if (normalized == "RDMA") {
    return PSClientType::kRdma;
  }
  if (normalized == "LOCAL_SHM") {
    return PSClientType::kLocalShm;
  }

  throw std::invalid_argument("Unknown ps_type for KVClientOp: " + ps_type);
}

json ResolveFrameworkPSClientTransportConfig(const json& config) {
  const PSClientType ps_type = ResolveFrameworkPSClientType(config);
  if (ps_type == PSClientType::kLocalShm) {
    if (config.contains("local_shm")) {
      return config["local_shm"];
    }
    return json{{"region_name", "recstore_local_ps"},
                {"slot_count", 64},
                {"slot_buffer_bytes", 8 * 1024 * 1024},
                {"client_timeout_ms", 30000}};
  }

  if (config.contains("client")) {
    return config["client"];
  }
  return json{{"host", "127.0.0.1"}, {"port", 15000}, {"shard", 0}};
}

PSClientCreateOptions
ResolvePSClientOptionsFromFrameworkConfig(const json& config) {
  return PSClientCreateOptions{
      .type             = ResolveFrameworkPSClientType(config),
      .transport_config = ResolveFrameworkPSClientTransportConfig(config),
      .raw_config       = config,
  };
}

} // namespace recstore
