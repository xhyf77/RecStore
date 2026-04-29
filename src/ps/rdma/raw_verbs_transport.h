#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
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

} // namespace petps
