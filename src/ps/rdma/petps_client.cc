#include "petps_client.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

#include "petps_magic.h"
#include "ps/base/Postoffice.h"
#include "ps/base/shard_manager.h"
#include "base/string.h"

DECLARE_int32(value_size);
DECLARE_int32(max_kv_num_per_request);
DEFINE_int32(numa_id, 0, "");
DEFINE_int32(rdma_server_ready_timeout_sec,
             30,
             "Timeout in seconds waiting for petps server ready key");
DEFINE_int32(rdma_server_ready_poll_ms,
             10,
             "Polling interval in milliseconds for petps server ready key");
DEFINE_uint64(
    rdma_client_receive_arena_bytes,
    256 * 1024 * 1024,
    "Per-process max bytes allocated by PetPSClient::GetReceiveBuffer");
DEFINE_uint64(
    rdma_put_client_send_arena_bytes,
    256 * 1024 * 1024,
    "Per-process max bytes allocated by PetPSClient PUT-v2 payload buffers");
DEFINE_int32(
    rdma_put_protocol_version,
    2,
    "RDMA PUT protocol version: 2=remote-payload(default), 1=legacy-inline");
DEFINE_string(rdma_put_v2_transfer_mode,
              "push",
              "RDMA PUT-v2 payload transfer mode: read|push");
DEFINE_uint64(rdma_put_v2_push_slot_bytes,
              256 * 1024,
              "RDMA PUT-v2(push) per-slot bytes reserved on server DSM");
DEFINE_int32(rdma_put_v2_push_slots_per_client,
             8,
             "RDMA PUT-v2(push) slot count reserved per client node");
DEFINE_uint64(
    rdma_put_v2_push_region_offset,
    64 * 1024 * 1024,
    "RDMA PUT-v2(push) base offset in server DSM for client payload slots");
DEFINE_int32(rdma_wait_spin_loops,
             1000,
             "Busy-spin loops before yielding in PetPSClient::WaitRPCFinish");
DEFINE_int32(rdma_wait_yield_loops,
             2000,
             "Yield loops before sleeping in PetPSClient::WaitRPCFinish");
DEFINE_int32(
    rdma_wait_sleep_us,
    50,
    "Sleep duration in microseconds after spin/yield phases in WaitRPCFinish");
DEFINE_int32(
    rdma_wait_timeout_ms,
    60000,
    "Timeout for waiting a single RDMA RPC completion; <=0 means no timeout");

namespace {
constexpr std::uint64_t kClientDsmSizeBytes        = 1 * 1000 * define::MB;
constexpr std::size_t kClientRdmaThreadBufferBytes = 1 * define::MB;

bool ShouldValidateRouting() {
  const char* env = std::getenv("RECSTORE_RDMA_VALIDATE_ROUTING");
  return env != nullptr && std::string(env) != "0";
}

int MaxPutKeysPerRpc(int value_size_bytes) {
  const std::size_t usable_payload =
      MESSAGE_SIZE - 40 - sizeof(RawMessage) - sizeof(std::uint32_t);
  if (value_size_bytes <= 0 ||
      usable_payload <= sizeof(petps::PutPayloadHeader)) {
    return 0;
  }
  const std::size_t per_key_bytes =
      sizeof(std::uint64_t) + static_cast<std::size_t>(value_size_bytes);
  return static_cast<int>(
      (usable_payload - sizeof(petps::PutPayloadHeader)) / per_key_bytes);
}

struct alignas(64) PutAckSlot {
  std::atomic<int32_t> status;
  char pad[64 - sizeof(std::atomic<int32_t>)];
};

std::int32_t WaitPutAck(std::atomic<int32_t>* ack) {
  int wait_loops   = 0;
  const auto start = std::chrono::steady_clock::now();
  while (ack->load(std::memory_order_acquire) ==
         static_cast<std::int32_t>(petps::RpcStatus::kPending)) {
    if (wait_loops < FLAGS_rdma_wait_spin_loops) {
      ++wait_loops;
      continue;
    }
    if (wait_loops < FLAGS_rdma_wait_spin_loops + FLAGS_rdma_wait_yield_loops) {
      ++wait_loops;
      std::this_thread::yield();
      continue;
    }
    if (FLAGS_rdma_wait_sleep_us > 0) {
      std::this_thread::sleep_for(
          std::chrono::microseconds(FLAGS_rdma_wait_sleep_us));
    } else {
      std::this_thread::yield();
    }
    if (FLAGS_rdma_wait_timeout_ms > 0) {
      const auto elapsed_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - start)
              .count();
      if (elapsed_ms > FLAGS_rdma_wait_timeout_ms) {
        throw std::runtime_error("PUT ack wait timeout timeout_ms=" +
                                 std::to_string(FLAGS_rdma_wait_timeout_ms));
      }
    }
  }
  return ack->load(std::memory_order_acquire);
}
} // namespace

namespace petps {

void PetPSClient::Init() {
  static uint64_t dsm_base_addr = (uint64_t)hugePageAlloc(kClientDsmSizeBytes);

  ClusterInfo cluster;
  cluster.serverNR = XPostoffice::GetInstance()->NumServers();
  cluster.clientNR = XPostoffice::GetInstance()->NumClients();
  DSMConfig config(CacheConfig(), cluster, 0, true);
  config.NIC_name = '0' + FLAGS_numa_id;
  config.dsmSize  = kClientDsmSizeBytes;
  config.baseAddr = (uint64_t)dsm_base_addr;
  dsm_ = DSM::getInstance(config, XPostoffice::GetInstance()->GlobalID());

  CHECK_EQ(dsm_->getMyNodeID(), XPostoffice::GetInstance()->GlobalID())
      << "inconsistent postoffice and wq dsm";

  extern int global_socket_id;
  ::global_socket_id = FLAGS_numa_id;
  LOG(INFO) << "set NUMA ID = " << FLAGS_numa_id;
}

int PetPSClient::GetParameter(base::ConstArray<uint64_t> keys,
                              std::vector<std::vector<float>>* values) {
  values->clear();
  if (keys.Size() == 0) {
    return 0;
  }

  const int embedding_dim = FLAGS_value_size / sizeof(float);
  std::vector<float> flat(keys.Size() * embedding_dim + 1, 0.0f);
  int rpc_id = GetParameter(keys, flat.data(), false, 0);
  WaitRPCFinish(rpc_id);

  // Check status word at the end of the buffer
  const std::int32_t status = *reinterpret_cast<std::int32_t*>(
      flat.data() + keys.Size() * embedding_dim);
  if (status != static_cast<std::int32_t>(RpcStatus::kOk)) {
    LOG(ERROR) << "GetParameter failed with status: "
               << RpcStatusToString(static_cast<RpcStatus>(status));
    RevokeRPCResource(rpc_id);
    return -1;
  }

  values->reserve(keys.Size());
  for (int i = 0; i < keys.Size(); ++i) {
    std::vector<float> row(embedding_dim);
    std::memcpy(row.data(), flat.data() + i * embedding_dim, FLAGS_value_size);
    values->push_back(std::move(row));
  }
  RevokeRPCResource(rpc_id);
  return 0;
}

void PetPSClient::WaitForServerReady() {
  const std::string key = "petps-server-ready-" + std::to_string(shard_);
  const auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::seconds(FLAGS_rdma_server_ready_timeout_sec);
  std::string value;
  while (std::chrono::steady_clock::now() < deadline) {
    if (XPostoffice::GetInstance()->MemCachedTryGet(key, &value) &&
        value == "1") {
      return;
    }
    std::this_thread::sleep_for(
        std::chrono::milliseconds(FLAGS_rdma_server_ready_poll_ms));
  }
  throw std::runtime_error(
      "Timed out waiting for RDMA server ready key: " + key);
}

std::vector<int> PetPSClient::GetServerThreadIDs() {
  auto m     = RawMessage::get_new_msg();
  m->node_id = dsm_->getMyNodeID();
  m->t_id    = dsm_->getMyThreadID();
  m->type    = GET_SERVER_THREADIDS;
  // RPC to the server ID <shard_>
  dsm_->rpc_call(m, shard_, 0);
  uint64_t wr_id;
  while (1) {
    auto recv = dsm_->rpc_fast_wait(&wr_id);
    // FB_LOG_EVERY_MS(WARNING, 5000)
    //     << "client wait the result of GetServerThreadIDs";
    if (recv) {
      CHECK_EQ(recv->type, RESP_GET_SERVER_THREADIDS);
      Cursor cursor;
      Slice extra_data = recv->get_string(cursor);
      base::ConstArray<int> cores(
          (int*)extra_data.s, extra_data.len / sizeof(int));
      LOG(INFO) << base::SFormat(
          "client{} {}th thread are routed to PS{} ({})th thread",
          m->node_id,
          (int)m->t_id,
          shard_,
          cores.Debug());
      return cores.ToVector();
    }
  }
  LOG(FATAL) << "not reach here";
  return std::vector<int>();
}

int PetPSClient::SelectServerThreadID() const {
  CHECK(!serverThreadIdsRoutedTo_.empty());
  const uint32_t slot = round_robin_.fetch_add(1, std::memory_order_relaxed) %
                        serverThreadIdsRoutedTo_.size();
  int ret = serverThreadIdsRoutedTo_[slot];
  return ret;
}

std::atomic<int32_t>* PetPSClient::GetPollSlot(uint64_t rpc_id) const {
  std::lock_guard<std::mutex> guard(rpc_mu_);
  auto it = rpcId2PollMap_.find(rpc_id);
  CHECK(it != rpcId2PollMap_.end()) << "unknown rpc_id=" << rpc_id;
  return it->second;
}

std::size_t PetPSClient::ResponseBufferBytes(std::size_t key_count) const {
  return FixedSlotResponseBytes(key_count, FLAGS_value_size);
}

void* PetPSClient::GetReceiveBuffer(size_t size) {
  static std::atomic<uint64_t> client_memory_offset_acc{0};
  const uint64_t old_offset =
      client_memory_offset_acc.fetch_add(static_cast<uint64_t>(size));
  const uint64_t new_offset = old_offset + static_cast<uint64_t>(size);
  if (new_offset > FLAGS_rdma_client_receive_arena_bytes) {
    throw std::runtime_error(
        "RDMA receive arena exhausted: request=" + std::to_string(size) +
        " old_offset=" + std::to_string(old_offset) +
        " limit=" + std::to_string(FLAGS_rdma_client_receive_arena_bytes));
  }
  GlobalAddress gaddr;
  gaddr.nodeID = dsm_->getMyNodeID();
  gaddr.offset = old_offset;
  return dsm_->addr(gaddr);
}

void* PetPSClient::GetSendBuffer(size_t size) {
  // PUT-v2 in current benchmark path is synchronous (wait for ack before next
  // PUT). Reuse a fixed DSM window instead of monotonically consuming arena.
  if (size > FLAGS_rdma_put_client_send_arena_bytes) {
    throw std::runtime_error(
        "RDMA put send arena exhausted: request=" + std::to_string(size) +
        " old_offset=0" +
        " limit=" + std::to_string(FLAGS_rdma_put_client_send_arena_bytes));
  }
  const uint64_t base_offset = FLAGS_rdma_client_receive_arena_bytes;
  if (base_offset + static_cast<uint64_t>(size) > kClientDsmSizeBytes) {
    throw std::runtime_error(
        "RDMA put send arena exceeds DSM size: request=" +
        std::to_string(size) +
        " old_offset=0 base_offset=" + std::to_string(base_offset) +
        " dsm_size=" + std::to_string(kClientDsmSizeBytes));
  }
  GlobalAddress gaddr;
  gaddr.nodeID = dsm_->getMyNodeID();
  gaddr.offset = base_offset;
  return dsm_->addr(gaddr);
}

// this interface assume all keys with the same embedding dimension
int PetPSClient::GetParameter(base::ConstArray<uint64_t> keys,
                              float* values,
                              bool isAsync,
                              int async_req_id) {
  thread_local auto m = RawMessage::get_new_msg();
  m->clear();
  GlobalAddress gaddr = dsm_->gaddr(values);
  m->type             = RpcType::GET;
  m->receive_gaddr    = gaddr;

  auto* poll = reinterpret_cast<std::atomic<int32_t>*>(
      reinterpret_cast<char*>(values) + keys.Size() * FLAGS_value_size);
  const uint64_t rpc_id = rpcIDAcc_.fetch_add(1, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> guard(rpc_mu_);
    rpcId2PollMap_[rpc_id] = poll;
  }
  poll->store(static_cast<std::int32_t>(RpcStatus::kPending),
              std::memory_order_release);

  if (ShouldValidateRouting()) {
    for (auto each : keys) {
      CHECK_EQ(shard_, ShardManager::KeyPartition(each))
          << each << " not belong to this PS";
    }
  }

  dsm_->rpc_call(m,
                 shard_,
                 SelectServerThreadID(),
                 Slice(keys.binary_data(), keys.binary_size()));

  if (rpc_id > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
    {
      std::lock_guard<std::mutex> guard(rpc_mu_);
      rpcId2PollMap_.erase(rpc_id);
    }
    throw std::runtime_error(
        "rpc_id overflow int range: " + std::to_string(rpc_id));
  }

  if (!isAsync) {
    WaitRPCFinish(static_cast<int>(rpc_id));
  }
  return static_cast<int>(rpc_id);
}

bool PetPSClient::QueryRPCFinished(int rpc_id) {
  auto* poll = GetPollSlot(static_cast<uint64_t>(rpc_id));
  return poll->load(std::memory_order_acquire) !=
         static_cast<std::int32_t>(RpcStatus::kPending);
}

void PetPSClient::WaitRPCFinish(int rpc_id) {
  auto* poll       = GetPollSlot(static_cast<uint64_t>(rpc_id));
  int wait_loops   = 0;
  const auto start = std::chrono::steady_clock::now();
  while (poll->load(std::memory_order_acquire) ==
         static_cast<std::int32_t>(RpcStatus::kPending)) {
#ifdef RPC_DEBUG
    FB_LOG_EVERY_MS(INFO, 1000) << "poll = " << *poll << "\n";
    // << "values[0]=" << values[0] << "\n"
    // << "values[1]=" << values[1] << "\n"
    // << "values[2]=" << values[2] << "\n"
    // << "values[3]=" << values[3] << "\n"
    // << "values[4]=" << values[4] << "\n"
    // << "values[5]=" << values[5] << "\n";
#endif
    if (wait_loops < FLAGS_rdma_wait_spin_loops) {
      ++wait_loops;
      continue;
    }
    if (wait_loops < FLAGS_rdma_wait_spin_loops + FLAGS_rdma_wait_yield_loops) {
      ++wait_loops;
      std::this_thread::yield();
      continue;
    }
    if (FLAGS_rdma_wait_sleep_us > 0) {
      std::this_thread::sleep_for(
          std::chrono::microseconds(FLAGS_rdma_wait_sleep_us));
    } else {
      std::this_thread::yield();
    }
    if (FLAGS_rdma_wait_timeout_ms > 0) {
      const auto elapsed_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - start)
              .count();
      if (elapsed_ms > FLAGS_rdma_wait_timeout_ms) {
        throw std::runtime_error(
            "WaitRPCFinish timeout rpc_id=" + std::to_string(rpc_id) +
            " timeout_ms=" + std::to_string(FLAGS_rdma_wait_timeout_ms));
      }
    }
  }
  return;
}

void PetPSClient::RevokeRPCResource(int rpc_id) {
  std::lock_guard<std::mutex> guard(rpc_mu_);
  rpcId2PollMap_.erase(static_cast<uint64_t>(rpc_id));
};

int PetPSClient::PutParameter(const std::vector<uint64_t>& keys,
                              const std::vector<std::vector<float>>& values) {
  std::lock_guard<std::mutex> put_guard(put_mu_);
  CHECK_EQ(keys.size(), values.size());
  if (keys.empty()) {
    return 0;
  }

  const std::size_t embedding_dim = FLAGS_value_size / sizeof(float);
  for (const auto& row : values) {
    if (row.size() != embedding_dim) {
      LOG(ERROR) << "PutParameter value size mismatch, expected dim="
                 << embedding_dim << " got=" << row.size();
      return -1;
    }
  }

  if (FLAGS_rdma_put_protocol_version != 1 &&
      FLAGS_rdma_put_protocol_version != 2) {
    LOG(ERROR) << "Unknown rdma_put_protocol_version="
               << FLAGS_rdma_put_protocol_version;
    return -1;
  }

  if (FLAGS_rdma_put_protocol_version == 2) {
    const std::uint32_t transfer_mode = [&]() -> std::uint32_t {
      if (FLAGS_rdma_put_v2_transfer_mode == "read") {
        return kPutV2TransferModeRead;
      }
      if (FLAGS_rdma_put_v2_transfer_mode == "push") {
        return kPutV2TransferModePush;
      }
      return std::numeric_limits<std::uint32_t>::max();
    }();
    if (transfer_mode == std::numeric_limits<std::uint32_t>::max()) {
      LOG(ERROR) << "Unknown rdma_put_v2_transfer_mode="
                 << FLAGS_rdma_put_v2_transfer_mode << ", expected read|push";
      return -1;
    }

    const std::size_t payload_bytes =
        PutPayloadBytes(keys.size(), FLAGS_value_size);
    if (payload_bytes > std::numeric_limits<std::uint32_t>::max()) {
      LOG(ERROR) << "PUT-v2 payload too large, bytes=" << payload_bytes;
      return -1;
    }

    char* payload_buf = nullptr;
    if (transfer_mode == kPutV2TransferModeRead) {
      try {
        payload_buf = static_cast<char*>(GetSendBuffer(payload_bytes));
      } catch (const std::exception& ex) {
        LOG(ERROR) << "PUT-v2(read) allocate payload buffer failed: "
                   << ex.what();
        return -1;
      }
    } else if (transfer_mode == kPutV2TransferModePush) {
      if (payload_bytes > kClientRdmaThreadBufferBytes) {
        LOG(ERROR)
            << "PUT-v2(push) payload exceeds thread RDMA buffer: payload_bytes="
            << payload_bytes
            << " thread_buffer_bytes=" << kClientRdmaThreadBufferBytes;
        return -1;
      }
      // dsm_->write[_sync] uses cacheLKey for local SGE. The local source
      // buffer must come from the registered per-thread RDMA cache buffer.
      payload_buf = dsm_->get_rdma_buffer();
    } else {
      LOG(ERROR) << "PUT-v2 unknown transfer mode=" << transfer_mode;
      return -1;
    }

    std::memcpy(payload_buf, keys.data(), keys.size() * sizeof(std::uint64_t));
    char* values_dst = payload_buf + keys.size() * sizeof(std::uint64_t);
    for (std::size_t i = 0; i < values.size(); ++i) {
      std::memcpy(values_dst + i * FLAGS_value_size,
                  values[i].data(),
                  FLAGS_value_size);
    }

    GlobalAddress payload_gaddr = dsm_->gaddr(payload_buf);
    if (transfer_mode == kPutV2TransferModePush) {
      if (FLAGS_rdma_put_v2_push_slot_bytes == 0 ||
          FLAGS_rdma_put_v2_push_slots_per_client <= 0) {
        LOG(ERROR) << "PUT-v2(push) invalid slot config: slot_bytes="
                   << FLAGS_rdma_put_v2_push_slot_bytes << " slots_per_client="
                   << FLAGS_rdma_put_v2_push_slots_per_client;
        return -1;
      }
      if (payload_bytes > FLAGS_rdma_put_v2_push_slot_bytes) {
        LOG(ERROR) << "PUT-v2(push) payload larger than slot: payload_bytes="
                   << payload_bytes
                   << " slot_bytes=" << FLAGS_rdma_put_v2_push_slot_bytes;
        return -1;
      }
      thread_local std::uint64_t slot_cursor = 0;
      const std::uint64_t slot_id =
          slot_cursor %
          static_cast<std::uint64_t>(FLAGS_rdma_put_v2_push_slots_per_client);
      ++slot_cursor;
      const std::uint64_t lane =
          static_cast<std::uint64_t>(dsm_->getMyNodeID());
      const std::uint64_t remote_slot =
          lane * static_cast<std::uint64_t>(
                     FLAGS_rdma_put_v2_push_slots_per_client) +
          slot_id;
      const std::uint64_t slot_offset =
          FLAGS_rdma_put_v2_push_region_offset +
          remote_slot * FLAGS_rdma_put_v2_push_slot_bytes;
      payload_gaddr.nodeID = static_cast<std::uint16_t>(shard_);
      payload_gaddr.offset = slot_offset;
      dsm_->write_sync(payload_buf, payload_gaddr, payload_bytes);
    }

    PutRemotePayloadV2 control{
        kPutRemotePayloadMagic,
        kPutProtocolVersionV2,
        0,
        static_cast<std::uint32_t>(keys.size()),
        static_cast<std::uint32_t>(embedding_dim),
        payload_gaddr,
        static_cast<std::uint32_t>(payload_bytes),
        transfer_mode,
        0,
    };
    std::string control_payload;
    std::string encode_error;
    if (!EncodePutRemoteControlV2(control, &control_payload, &encode_error)) {
      LOG(ERROR) << "PUT-v2 encode control failed: " << encode_error;
      return -1;
    }

    constexpr std::size_t kPutAckSlotCount = 4096;
    thread_local PutAckSlot* ack_slots     = nullptr;
    thread_local std::size_t ack_cursor    = 0;
    if (ack_slots == nullptr) {
      ack_slots = static_cast<PutAckSlot*>(
          GetReceiveBuffer(sizeof(PutAckSlot) * kPutAckSlotCount));
    }
    PutAckSlot* slot = ack_slots + (ack_cursor % kPutAckSlotCount);
    ++ack_cursor;
    auto* ack = &slot->status;
    ack->store(static_cast<std::int32_t>(RpcStatus::kPending),
               std::memory_order_release);

    thread_local auto m = RawMessage::get_new_msg();
    m->clear();
    m->type          = RpcType::PUT;
    m->receive_gaddr = dsm_->gaddr(slot);
    dsm_->rpc_call(m,
                   shard_,
                   SelectServerThreadID(),
                   Slice(control_payload.data(), control_payload.size()));
    const std::int32_t status = WaitPutAck(ack);
    if (status != static_cast<std::int32_t>(RpcStatus::kOk)) {
      LOG(ERROR) << "PUT-v2 failed with status="
                 << RpcStatusToString(static_cast<RpcStatus>(status))
                 << " protocol=2 key_count=" << keys.size()
                 << " payload_bytes=" << payload_bytes;
      return -1;
    }
    return 0;
  }

  const int max_put_keys = MaxPutKeysPerRpc(FLAGS_value_size);
  if (max_put_keys <= 0) {
    LOG(ERROR) << "Invalid max_put_keys computed for value_size="
               << FLAGS_value_size;
    return -1;
  }

  for (std::size_t offset = 0; offset < keys.size();
       offset += static_cast<std::size_t>(max_put_keys)) {
    const std::size_t end =
        std::min(offset + static_cast<std::size_t>(max_put_keys), keys.size());
    std::vector<uint64_t> key_slice(keys.begin() + offset, keys.begin() + end);
    std::vector<std::vector<float>> value_slice(
        values.begin() + offset, values.begin() + end);

    std::string payload = EncodePutPayload(key_slice, value_slice);
    if (payload.empty()) {
      LOG(ERROR) << "EncodePutPayload failed for slice [" << offset << ", "
                 << end << "]";
      return -1;
    }

    constexpr std::size_t kPutAckSlotCount = 4096;
    thread_local PutAckSlot* ack_slots     = nullptr;
    thread_local std::size_t ack_cursor    = 0;
    if (ack_slots == nullptr) {
      ack_slots = static_cast<PutAckSlot*>(
          GetReceiveBuffer(sizeof(PutAckSlot) * kPutAckSlotCount));
    }
    PutAckSlot* slot = ack_slots + (ack_cursor % kPutAckSlotCount);
    ++ack_cursor;
    auto* ack = &slot->status;
    ack->store(static_cast<std::int32_t>(RpcStatus::kPending),
               std::memory_order_release);

    thread_local auto m = RawMessage::get_new_msg();
    m->clear();
    m->type          = RpcType::PUT;
    m->receive_gaddr = dsm_->gaddr(slot);
    dsm_->rpc_call(m,
                   shard_,
                   SelectServerThreadID(),
                   Slice(payload.data(), payload.size()));
    const std::int32_t status = WaitPutAck(ack);
    if (status != static_cast<std::int32_t>(RpcStatus::kOk)) {
      LOG(ERROR) << "PUT-v1 slice failed with status="
                 << RpcStatusToString(static_cast<RpcStatus>(status))
                 << " protocol=1 slice=[" << offset << ", " << end << ")";
      return -1;
    }
  }
  return 0;
}

int PetPSClient::FakePutParameter(base::ConstArray<uint64_t> keys,
                                  float* values) {
  LOG(WARNING) << "FakePutParameter is a benchmark-only path and does not "
                  "carry full put payload values";
  thread_local auto m = RawMessage::get_new_msg();
  m->clear();
  GlobalAddress gaddr = dsm_->gaddr(values);
  m->type             = RpcType::PUT;
  m->receive_gaddr    = gaddr;

  // LOG(INFO) << "send PS Put";
  auto* poll            = reinterpret_cast<std::atomic<int32_t>*>(values);
  const uint64_t rpc_id = rpcIDAcc_.fetch_add(1, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> guard(rpc_mu_);
    rpcId2PollMap_[rpc_id] = poll;
  }
  poll->store(static_cast<std::int32_t>(RpcStatus::kPending),
              std::memory_order_release);

  if (ShouldValidateRouting()) {
    for (auto each : keys) {
      CHECK_EQ(shard_, ShardManager::KeyPartition(each))
          << each << " not belong to this PS";
    }
  }

  dsm_->rpc_call(m,
                 shard_,
                 SelectServerThreadID(),
                 Slice(keys.binary_data(), keys.binary_size()));

  if (rpc_id > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
    {
      std::lock_guard<std::mutex> guard(rpc_mu_);
      rpcId2PollMap_.erase(rpc_id);
    }
    throw std::runtime_error(
        "rpc_id overflow int range: " + std::to_string(rpc_id));
  }

  // if (!isAsync) {
  //   WaitRPCFinish(rpcIDAcc_);
  // }
  return static_cast<int>(rpc_id);
}

} // namespace petps
