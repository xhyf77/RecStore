#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <infiniband/verbs.h>

#include "third_party/Mayfly-main/include/GlobalAddress.h"

namespace petps {

struct RawVerbsConfig {
  int global_id = 0;
  int num_servers = 1;
  int num_clients = 1;
  int numa_id = 0;
  std::size_t local_region_bytes = 128 * 1024 * 1024;
  std::uint64_t local_base_addr = 0;
  std::uint64_t allocation_start_offset = 0;
  std::uint64_t reserved_region_offset = 0;
  std::uint64_t reserved_region_bytes = 0;
};

struct RawVerbsReservedRegion {
  std::uint64_t offset = 0;
  std::uint64_t bytes = 0;
};

class RawVerbsRegionAllocator {
public:
  explicit RawVerbsRegionAllocator(std::uint64_t limit_bytes,
                                   std::uint64_t allocation_start_offset = 0)
      : limit_bytes_(limit_bytes),
        allocation_offset_(allocation_start_offset) {
    if (allocation_start_offset > limit_bytes) {
      throw std::runtime_error(
          "raw verbs allocation start outside local memory");
    }
  }

  void SetReservedRegion(RawVerbsReservedRegion reserved) {
    if (reserved.bytes != 0) {
      const std::uint64_t reserved_end = reserved.offset + reserved.bytes;
      if (reserved_end < reserved.offset || reserved_end > limit_bytes_) {
        throw std::runtime_error(
            "raw verbs reserved region outside local memory");
      }
    }
    reserved_ = reserved;
    const std::uint64_t reserved_end = reserved.offset + reserved.bytes;
    if (reserved.bytes != 0 && allocation_offset_ >= reserved.offset &&
        allocation_offset_ < reserved_end) {
      allocation_offset_ = reserved_end;
    }
  }

  std::uint64_t Allocate(std::size_t bytes) {
    const std::uint64_t aligned = Align(bytes);
    std::uint64_t offset = allocation_offset_;
    const std::uint64_t reserved_begin = reserved_.offset;
    const std::uint64_t reserved_end = reserved_.offset + reserved_.bytes;
    if (reserved_.bytes != 0) {
      if (offset >= reserved_begin && offset < reserved_end) {
        offset = reserved_end;
      } else if (offset < reserved_begin &&
                 offset + aligned > reserved_begin) {
        offset = reserved_end;
      }
    }
    if (offset + aligned < offset || offset + aligned > limit_bytes_) {
      throw std::runtime_error("raw verbs registered region exhausted");
    }
    allocation_offset_ = offset + aligned;
    return offset;
  }

  std::uint64_t Checkpoint() const { return allocation_offset_; }

  void Restore(std::uint64_t checkpoint) {
    if (checkpoint > limit_bytes_) {
      throw std::runtime_error(
          "raw verbs allocation checkpoint outside local memory");
    }
    allocation_offset_ = checkpoint;
  }

  std::uint64_t current_offset() const { return allocation_offset_; }

private:
  static std::uint64_t Align(std::size_t bytes) {
    return (static_cast<std::uint64_t>(bytes) + 63) & ~std::uint64_t{63};
  }

  std::uint64_t limit_bytes_ = 0;
  std::uint64_t allocation_offset_ = 0;
  RawVerbsReservedRegion reserved_{};
};

class RawVerbsRegionAllocatorScope {
public:
  explicit RawVerbsRegionAllocatorScope(RawVerbsRegionAllocator* allocator)
      : allocator_(allocator),
        checkpoint_(allocator != nullptr ? allocator->Checkpoint() : 0) {}

  ~RawVerbsRegionAllocatorScope() {
    if (allocator_ != nullptr) {
      allocator_->Restore(checkpoint_);
    }
  }

  RawVerbsRegionAllocatorScope(const RawVerbsRegionAllocatorScope&) = delete;
  RawVerbsRegionAllocatorScope& operator=(const RawVerbsRegionAllocatorScope&) =
      delete;

private:
  RawVerbsRegionAllocator* allocator_ = nullptr;
  std::uint64_t checkpoint_ = 0;
};

struct RawVerbsRemoteMemory {
  std::uint16_t node_id = 0;
  std::uint64_t base_addr = 0;
  std::uint32_t rkey = 0;
};

struct RawVerbsCompletion {
  std::uint64_t wr_id = 0;
  std::uint32_t imm_data = 0;
  bool has_imm = false;
  ibv_wc_opcode opcode = IBV_WC_SEND;
};

struct RawVerbsNodeMeta {
  std::uint16_t node_id = 0;
  std::uint16_t lid = 0;
  std::uint32_t qpn = 0;
  std::uint32_t psn = 3185;
  std::uint32_t rkey = 0;
  std::uint64_t base_addr = 0;
  std::uint8_t gid[16] = {};
};

class RawVerbsTransport {
public:
  explicit RawVerbsTransport(const RawVerbsConfig& config);
  ~RawVerbsTransport();

  RawVerbsTransport(const RawVerbsTransport&) = delete;
  RawVerbsTransport& operator=(const RawVerbsTransport&) = delete;

  void RegisterThread();
  void* AllocateRegistered(std::size_t bytes);
  std::uint64_t SaveAllocationState() const;
  void RestoreAllocationState(std::uint64_t checkpoint);
  GlobalAddress LocalAddress(void* ptr) const;
  void* LocalPointer(GlobalAddress address) const;

  void PublishAndConnect();
  RawVerbsNodeMeta LocalMeta() const;

  void Write(const void* local, GlobalAddress remote, std::size_t bytes,
             std::uint64_t wr_id, bool signaled);
  void Read(void* local, GlobalAddress remote, std::size_t bytes,
            std::uint64_t wr_id, bool signaled);
  void SendDoorbell(std::uint16_t node_id, std::uint32_t imm_data,
                    std::uint64_t wr_id);
  bool Poll(RawVerbsCompletion* completion, int timeout_ms);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class RawVerbsTransportAllocationScope {
public:
  explicit RawVerbsTransportAllocationScope(RawVerbsTransport* transport)
      : transport_(transport),
        checkpoint_(transport != nullptr ? transport->SaveAllocationState() : 0) {}

  ~RawVerbsTransportAllocationScope() {
    if (transport_ != nullptr) {
      transport_->RestoreAllocationState(checkpoint_);
    }
  }

  RawVerbsTransportAllocationScope(
      const RawVerbsTransportAllocationScope&) = delete;
  RawVerbsTransportAllocationScope& operator=(
      const RawVerbsTransportAllocationScope&) = delete;

private:
  RawVerbsTransport* transport_ = nullptr;
  std::uint64_t checkpoint_ = 0;
};

} // namespace petps
