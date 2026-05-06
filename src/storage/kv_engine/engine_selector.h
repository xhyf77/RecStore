#pragma once

#include <set>
#include <stdexcept>
#include <string>

#include "base_kv.h"

namespace base {

struct EngineResolved {
  std::string engine;
  BaseKVConfig cfg;
};

inline EngineResolved ResolveEngine(BaseKVConfig cfg) {
  auto& j = cfg.json_config_;

  for (const char* k : {"path", "capacity", "index", "value"}) {
    if (!j.contains(k)) {
      throw std::invalid_argument(std::string(k) + " is required");
    }
  }

  for (const char* k :
       {"index_type",
        "value_type",
        "allocator_type",
        "value_memory_management",
        "io_backend_type",
        "value_size",
        "engine_type",
        "mode",
        "value_layout",
        "shmcapacity",
        "ssdcapacity"}) {
    if (j.contains(k)) {
      throw std::invalid_argument(
          "legacy field '" + std::string(k) +
          "' not allowed; use nested index/value config");
    }
  }

  const auto& idx_j = j.at("index");
  const auto& val_j = j.at("value");
  if (!idx_j.contains("type")) {
    throw std::invalid_argument("index.type is required");
  }
  if (!val_j.contains("type")) {
    throw std::invalid_argument("value.type is required");
  }

  const std::string idx_type = idx_j.at("type").get<std::string>();
  const std::string val_type = val_j.at("type").get<std::string>();

  static const std::set<std::string> kDramIndex = {
      "DRAM_EXTENDIBLE_HASH", "DRAM_UNORDERED_MAP", "DRAM_PET_HASH"};
  static const std::set<std::string> kSsdIndex = {"SSD", "SSD_EXTENDIBLE_HASH"};
  static const std::set<std::string> kValTypes = {
      "DRAM_VALUE_STORE", "SSD_VALUE_STORE", "TIERED_VALUE_STORE"};

  if (!kDramIndex.count(idx_type) && !kSsdIndex.count(idx_type)) {
    throw std::invalid_argument("unknown index.type: " + idx_type);
  }
  if (!kValTypes.count(val_type)) {
    throw std::invalid_argument("unknown value.type: " + val_type);
  }
  if (kSsdIndex.count(idx_type) && val_type != "SSD_VALUE_STORE") {
    throw std::invalid_argument("SSD index only supports SSD_VALUE_STORE");
  }
  if (kSsdIndex.count(idx_type) && !idx_j.contains("io")) {
    throw std::invalid_argument("SSD index requires index.io config");
  }

  if (val_type == "DRAM_VALUE_STORE") {
    if (!val_j.contains("dram_allocator")) {
      throw std::invalid_argument(
          "DRAM_VALUE_STORE requires value.dram_allocator");
    }
    if (val_j.contains("ssd_allocator")) {
      throw std::invalid_argument(
          "DRAM_VALUE_STORE must not have ssd_allocator");
    }
  } else if (val_type == "SSD_VALUE_STORE") {
    if (!val_j.contains("ssd_allocator")) {
      throw std::invalid_argument(
          "SSD_VALUE_STORE requires value.ssd_allocator");
    }
    if (val_j.contains("dram_allocator")) {
      throw std::invalid_argument(
          "SSD_VALUE_STORE must not have dram_allocator");
    }
  } else if (val_type == "TIERED_VALUE_STORE") {
    if (!val_j.contains("dram_allocator") || !val_j.contains("ssd_allocator")) {
      throw std::invalid_argument(
          "TIERED_VALUE_STORE requires dram_allocator and ssd_allocator");
    }
  }

  return EngineResolved{"KVEngine", std::move(cfg)};
}

} // namespace base
