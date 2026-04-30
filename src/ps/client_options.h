#pragma once

#include "base/json.h"

namespace recstore {

enum class PSClientType {
  kGrpc,
  kBrpc,
  kRdma,
  kLocalShm,
};

struct PSClientCreateOptions {
  PSClientType type;
  json transport_config;
  json raw_config;
};

} // namespace recstore
