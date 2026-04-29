#include <folly/init/Init.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <future>
#include <string>
#include <vector>

#include "base/base.h"
#include "base/factory.h"
#include "base/log.h"
#include "base/timer.h"
#include "ps/base/Postoffice.h"
#include "ps/base/base_ps_server.h"
#include "ps/base/cache_ps_impl.h"
#include "ps/base/parameters.h"
#include "ps/base/shard_manager.h"
#include "mayfly_config.h"
#include "memory/epoch_manager.h"
#include "memory/shm_file.h"
#include "ps/rdma/raw_verbs_transport.h"
#include "ps/rdma/rdma_protocol.h"
#include "ps/rdma/rdma_status.h"
#include "ps/rdma/rdma_transport_mode.h"
#include "petps_magic.h"
#include "recstore_config.h"
#include "third_party/Mayfly-main/include/DSM.h"
#include "third_party/json/single_include/nlohmann/json.hpp"

DEFINE_string(config_path,
              RECSTORE_PATH "/recstore_config.json",
              "config file path");

DEFINE_double(warmup_ratio,
              0.8,
              "bulk load (warmup_ratio * key_space) kvs in DB");

DEFINE_int32(thread_num, 1, "");
DEFINE_bool(use_sglist, true, "");
DEFINE_bool(preload, false, "");
DEFINE_bool(use_dram, false, "");
DEFINE_int32(numa_id, 0, "");
DEFINE_uint64(rdma_per_thread_response_limit_bytes,
              1 * 1024 * 1024,
              "Per-thread max response bytes for RDMA GET replies");
DEFINE_uint64(rdma_put_server_scratch_bytes,
              1 * 1024 * 1024,
              "Per-thread max bytes used for RDMA PUT-v2 payload scratch");
DEFINE_uint64(rdma_put_v2_push_slot_bytes,
              256 * 1024,
              "RDMA PUT-v2(push) per-slot bytes reserved in server DSM");
DEFINE_int32(rdma_put_v2_push_slots_per_client,
             8,
             "RDMA PUT-v2(push) slot count reserved for each client node");
DEFINE_uint64(
    rdma_put_v2_push_region_offset,
    64 * 1024 * 1024,
    "RDMA PUT-v2(push) base offset in server DSM used by client payload slots");
DEFINE_string(rdma_transport_mode,
              "raw_message",
              "RDMA transport mode: raw_message|descriptor_doorbell");
DEFINE_int32(
    rdma_wait_timeout_ms,
    60000,
    "Timeout for waiting a single raw verbs completion; <=0 means no timeout");

DECLARE_int32(value_size);
DECLARE_int32(max_kv_num_per_request);

namespace {
constexpr std::size_t kRdmaThreadBufferBytes = 1 * define::MB;

bool ShouldValidateRouting() {
  const char* env = std::getenv("RECSTORE_RDMA_VALIDATE_ROUTING");
  return env != nullptr && std::string(env) != "0";
}
} // namespace

namespace recstore {
class PetPSServer : public BaseParameterServer {
public:
  PetPSServer(CachePS* cache_ps, int thread_count)
      : cache_ps_(cache_ps),
        thread_count_(thread_count),
        get_parameter_timer_("GetParameter", 1),
        index_timer_("Index Part", 1),
        value_timer_("Value Part", 1),
        epoch_manager_(base::epoch::EpochManager::GetInstance()) {
    CHECK_LE(thread_count, kMaxThread);

    ClusterInfo cluster;
    cluster.serverNR = XPostoffice::GetInstance()->NumServers();
    cluster.clientNR = XPostoffice::GetInstance()->NumClients();

    DSMConfig config(CacheConfig(), cluster, 0, false);
    if (FLAGS_use_sglist) {
      // TODO: Need to implement PM address registration for cache_ps
      LOG(WARNING) << "PM address registration not implemented for cache_ps, "
                      "using default DRAM allocation";
      config.dsmSize  = 100 * define::MB;
      config.baseAddr = (uint64_t)hugePageAlloc(config.dsmSize);
      LOG(INFO) << "Using DRAM space instead of PM space";
    } else {
      config.dsmSize  = 100 * define::MB;
      config.baseAddr = (uint64_t)hugePageAlloc(config.dsmSize);
      LOG(INFO) << "WE DONT register PM space to RNIC";
    }
    LOG(INFO) << "register MR start =" << (void*)config.baseAddr
              << ", end = " << (void*)(config.baseAddr + config.dsmSize)
              << ", size = " << config.dsmSize;

    config.NIC_name = '0' + FLAGS_numa_id;
    {
      dsm_ = DSM::getInstance(config, XPostoffice::GetInstance()->GlobalID());
      CHECK_EQ(dsm_->getMyNodeID(), XPostoffice::GetInstance()->GlobalID())
          << "inconsistent postoffice and wq dsm";
      LOG(INFO) << "xmh: finish construct DSM";
    }
    sourcelists_.resize(thread_count);
    for (int i = 0; i < thread_count; i++) {
      sourcelists_[i].resize(FLAGS_max_kv_num_per_request);
    }
    std::string mode_error;
    CHECK(petps::ParseRdmaTransportMode(
              FLAGS_rdma_transport_mode, &transport_mode_, &mode_error))
        << mode_error;
    if (transport_mode_ == petps::RdmaTransportMode::kDescriptorDoorbell) {
      CHECK_GT(FLAGS_rdma_put_v2_push_slots_per_client, 0)
          << "descriptor_doorbell requires positive slot count";
      CHECK_GT(FLAGS_rdma_put_v2_push_slot_bytes, 0)
          << "descriptor_doorbell requires positive slot bytes";
      petps::RawVerbsConfig raw_config;
      raw_config.global_id = XPostoffice::GetInstance()->GlobalID();
      raw_config.num_servers = XPostoffice::GetInstance()->NumServers();
      raw_config.num_clients = XPostoffice::GetInstance()->NumClients();
      raw_config.numa_id = FLAGS_numa_id;
      raw_config.local_base_addr = dsm_->get_conf()->baseAddr;
      raw_config.local_region_bytes = dsm_->get_conf()->dsmSize;
      raw_transport_ = std::make_unique<petps::RawVerbsTransport>(raw_config);
    }
  }

  void Run() {
    for (int i = 0; i < thread_count_; i++) {
      LOG(INFO) << "Starts PS polling thread " << i;
      threads_.emplace_back(&PetPSServer::PollingThread, this, i);
      tp[i][0] = 0;
    }
  }

  uint64_t GetThroughputCounterSum() const {
    uint64_t sum = 0;
    for (int i = 0; i < thread_count_; i++)
      sum += tp[i][0];
    return sum;
  }

private:
  void RpcGetServerServingThreadIDs(RawMessage* recv) {
    CHECK_EQ(recv->type, GET_SERVER_THREADIDS);
    VLOG(1) << "component=rdma_server event=get_server_threadids_recv node_id="
            << static_cast<int>(recv->node_id)
            << " tid=" << static_cast<int>(recv->t_id);
    static std::atomic_int serving_thread_id{0};
    auto m  = RawMessage::get_new_msg();
    m->type = RESP_GET_SERVER_THREADIDS;
    std::vector<int> thread_ids;
    thread_ids.reserve(static_cast<std::size_t>(thread_count_));
    const int start = serving_thread_id.fetch_add(1);
    for (int i = 0; i < thread_count_; ++i) {
      thread_ids.push_back((start + i) % thread_count_);
    }
    dsm_->rpc_call(
        m,
        recv->node_id,
        recv->t_id,
        Slice((char*)thread_ids.data(), thread_ids.size() * sizeof(int)));
    VLOG(1) << "component=rdma_server event=get_server_threadids_reply node_id="
            << static_cast<int>(recv->node_id) << " tid="
            << static_cast<int>(recv->t_id) << " threads=" << thread_ids.size();
  }

  void RpcPsPut(RawMessage* recv, int thread_id) {
    Cursor cursor;
    Slice extra_data = recv->get_string(cursor);

    std::string error;
    petps::RpcStatus status = petps::RpcStatus::kOk;
    const std::string_view payload_view(extra_data.s, extra_data.len);

    petps::PutRemotePayloadV2 control{};
    if (petps::DecodePutRemoteControlV2(payload_view, &control, &error)) {
      if (control.embedding_dim * sizeof(float) != FLAGS_value_size) {
        LOG(ERROR) << "RpcPsPut(v2) value size mismatch, embedding_dim="
                   << control.embedding_dim
                   << " FLAGS_value_size=" << FLAGS_value_size;
        status = petps::RpcStatus::kValueSizeMismatch;
      } else if (control.key_count >
                 static_cast<std::uint32_t>(FLAGS_max_kv_num_per_request)) {
        LOG(ERROR) << "RpcPsPut(v2) batch too large, key_count="
                   << control.key_count << " max_kv_num_per_request="
                   << FLAGS_max_kv_num_per_request;
        status = petps::RpcStatus::kBatchTooLarge;
      } else {
        const char* payload_ptr = nullptr;
        if (control.transfer_mode == petps::kPutV2TransferModeRead) {
          const std::size_t scratch_limit = std::min<std::size_t>(
              FLAGS_rdma_put_server_scratch_bytes, kRdmaThreadBufferBytes);
          if (control.payload_bytes > scratch_limit) {
            LOG(ERROR) << "RpcPsPut(v2-read) scratch overflow, payload_bytes="
                       << control.payload_bytes
                       << " scratch_limit=" << scratch_limit;
            status = petps::RpcStatus::kBatchTooLarge;
          } else {
            char* put_scratch = dsm_->get_rdma_buffer();
            dsm_->read_sync(
                put_scratch, control.payload_gaddr, control.payload_bytes);
            payload_ptr = put_scratch;
          }
        } else if (control.transfer_mode == petps::kPutV2TransferModePush) {
          if (FLAGS_rdma_put_v2_push_slot_bytes == 0 ||
              FLAGS_rdma_put_v2_push_slots_per_client <= 0) {
            LOG(ERROR) << "RpcPsPut(v2-push) invalid slot config: slot_bytes="
                       << FLAGS_rdma_put_v2_push_slot_bytes
                       << " slots_per_client="
                       << FLAGS_rdma_put_v2_push_slots_per_client;
            status = petps::RpcStatus::kInvalidPayload;
          } else if (control.payload_bytes >
                     FLAGS_rdma_put_v2_push_slot_bytes) {
            LOG(ERROR)
                << "RpcPsPut(v2-push) payload larger than slot: payload_bytes="
                << control.payload_bytes
                << " slot_bytes=" << FLAGS_rdma_put_v2_push_slot_bytes;
            status = petps::RpcStatus::kBatchTooLarge;
          } else if (control.payload_gaddr.nodeID != dsm_->getMyNodeID()) {
            LOG(ERROR)
                << "RpcPsPut(v2-push) payload node mismatch: payload_node="
                << control.payload_gaddr.nodeID
                << " local_node=" << dsm_->getMyNodeID();
            status = petps::RpcStatus::kInvalidPayload;
          } else {
            const std::uint64_t slots_per_client = static_cast<std::uint64_t>(
                FLAGS_rdma_put_v2_push_slots_per_client);
            const std::uint64_t total_slots =
                static_cast<std::uint64_t>(dsm_->get_conf()->machineNR) *
                slots_per_client;
            const std::uint64_t region_bytes =
                total_slots * FLAGS_rdma_put_v2_push_slot_bytes;
            const std::uint64_t region_begin =
                FLAGS_rdma_put_v2_push_region_offset;
            const std::uint64_t region_end    = region_begin + region_bytes;
            const std::uint64_t payload_begin = control.payload_gaddr.offset;
            const std::uint64_t payload_end =
                payload_begin +
                static_cast<std::uint64_t>(control.payload_bytes);
            const std::uint64_t sender_lane_begin =
                region_begin +
                static_cast<std::uint64_t>(recv->node_id) * slots_per_client *
                    FLAGS_rdma_put_v2_push_slot_bytes;
            const std::uint64_t sender_lane_end =
                sender_lane_begin +
                slots_per_client * FLAGS_rdma_put_v2_push_slot_bytes;

            if (region_end <= region_begin) {
              LOG(ERROR) << "RpcPsPut(v2-push) invalid region range begin="
                         << region_begin << " end=" << region_end;
              status = petps::RpcStatus::kInvalidPayload;
            } else if (payload_begin < region_begin ||
                       payload_end > region_end ||
                       payload_end < payload_begin) {
              LOG(ERROR) << "RpcPsPut(v2-push) payload out of range: payload=["
                         << payload_begin << "," << payload_end << ") region=["
                         << region_begin << "," << region_end << ")";
              status = petps::RpcStatus::kInvalidPayload;
            } else if ((payload_begin - region_begin) %
                           FLAGS_rdma_put_v2_push_slot_bytes !=
                       0) {
              LOG(ERROR) << "RpcPsPut(v2-push) payload not slot-aligned: "
                            "payload_begin="
                         << payload_begin << " region_begin=" << region_begin
                         << " slot_bytes=" << FLAGS_rdma_put_v2_push_slot_bytes;
              status = petps::RpcStatus::kInvalidPayload;
            } else if (payload_begin < sender_lane_begin ||
                       payload_end > sender_lane_end ||
                       sender_lane_end < sender_lane_begin) {
              LOG(ERROR)
                  << "RpcPsPut(v2-push) payload not in sender lane: sender="
                  << static_cast<int>(recv->node_id) << " payload=["
                  << payload_begin << "," << payload_end << ") sender_lane=["
                  << sender_lane_begin << "," << sender_lane_end << ")";
              status = petps::RpcStatus::kInvalidPayload;
            } else if (payload_end >
                       static_cast<std::uint64_t>(dsm_->get_conf()->dsmSize)) {
              LOG(ERROR)
                  << "RpcPsPut(v2-push) payload exceeds DSM size: payload_end="
                  << payload_end << " dsm_size=" << dsm_->get_conf()->dsmSize;
              status = petps::RpcStatus::kInvalidPayload;
            } else {
              payload_ptr = dsm_->addr(control.payload_gaddr);
            }
          }
        } else {
          LOG(ERROR) << "RpcPsPut(v2) unknown transfer_mode="
                     << control.transfer_mode;
          status = petps::RpcStatus::kInvalidPayload;
        }

        if (status == petps::RpcStatus::kOk && payload_ptr != nullptr) {
          const auto* keys =
              reinterpret_cast<const std::uint64_t*>(payload_ptr);
          const char* value_bytes =
              payload_ptr + static_cast<std::size_t>(control.key_count) *
                                sizeof(std::uint64_t);
          cache_ps_->PutDenseParameterBatch(
              keys,
              reinterpret_cast<const float*>(value_bytes),
              static_cast<int>(control.key_count),
              static_cast<int>(control.embedding_dim),
              thread_id);
        } else if (status == petps::RpcStatus::kOk) {
          status = petps::RpcStatus::kInvalidPayload;
        }
      }
    } else {
      petps::DecodedPutPayload decoded;
      if (!petps::DecodePutPayload(payload_view, &decoded, &error)) {
        LOG(ERROR) << "RpcPsPut decode error: " << error;
        status = petps::RpcStatus::kInvalidPayload;
      } else if (decoded.embedding_dim * sizeof(float) != FLAGS_value_size) {
        LOG(ERROR) << "RpcPsPut(v1) value size mismatch, embedding_dim="
                   << decoded.embedding_dim
                   << " FLAGS_value_size=" << FLAGS_value_size;
        status = petps::RpcStatus::kValueSizeMismatch;
      } else if (decoded.keys.size() >
                 static_cast<std::size_t>(FLAGS_max_kv_num_per_request)) {
        LOG(ERROR) << "RpcPsPut(v1) batch too large, key_count="
                   << decoded.keys.size() << " max_kv_num_per_request="
                   << FLAGS_max_kv_num_per_request;
        status = petps::RpcStatus::kBatchTooLarge;
      } else {
        for (std::size_t i = 0; i < decoded.keys.size(); ++i) {
          cache_ps_->PutSingleParameter(
              decoded.keys[i],
              decoded.values.data() + i * decoded.embedding_dim,
              decoded.embedding_dim,
              thread_id);
        }
      }
    }

    const std::int32_t code = static_cast<std::int32_t>(status);
    auto* ack_buf           = dsm_->get_rdma_buffer();
    std::memcpy(ack_buf, &code, sizeof(code));
    dsm_->write(
        ack_buf, recv->receive_gaddr, sizeof(code), true, petps::WR_ID_PUT);
  }

  void RpcPsGet(RawMessage* recv, int thread_id) {
    const bool perf_condition = (thread_id == 0);
    auto& sourcelist          = sourcelists_[thread_id];

    epoch_manager_->Protect();

    if (perf_condition)
      get_parameter_timer_.start();
    Cursor cursor;
    Slice extra_data = recv->get_string(cursor);

    int batch_get_kv_count = extra_data.len / sizeof(uint64_t);
    tp[thread_id][0] += batch_get_kv_count;
    base::ConstArray<uint64_t> keys(
        (uint64_t*)extra_data.s, batch_get_kv_count);
    if (ShouldValidateRouting()) {
      for (auto each : keys) {
        CHECK_EQ(XPostoffice::GetInstance()->ServerID(),
                 ShardManager::KeyPartition(each))
            << each << " not belong to this PS; "
            << "sended from client node_id = " << (int)recv->node_id;
      }
    }
#ifdef RPC_DEBUG
    LOG(INFO) << "recv->msg_size=" << extra_data.len;
    LOG(INFO) << "server batch gets: " << keys.Debug();
#endif
    CHECK_LE(batch_get_kv_count, FLAGS_max_kv_num_per_request);

    const int embedding_dim = FLAGS_value_size / sizeof(float);
    const std::size_t response_bytes =
        petps::FixedSlotResponseBytes(batch_get_kv_count, FLAGS_value_size);
    auto* buf = dsm_->get_rdma_buffer();

    if (perf_condition)
      index_timer_.start();
    bool flat_get_ok = true;
    if (response_bytes <= FLAGS_rdma_per_thread_response_limit_bytes) {
      flat_get_ok = cache_ps_->GetParameterFlat(keys,
                                                reinterpret_cast<float*>(buf),
                                                batch_get_kv_count,
                                                embedding_dim,
                                                thread_id);
    }
    if (perf_condition)
      index_timer_.end();

#ifdef RPC_DEBUG
    if (response_bytes <= FLAGS_rdma_per_thread_response_limit_bytes &&
        flat_get_ok) {
      for (int i = 0; i < batch_get_kv_count; i++) {
        float* slot = reinterpret_cast<float*>(buf + i * FLAGS_value_size);
        XDebug::AssertTensorEq(
            slot,
            embedding_dim,
            keys[i],
            folly::sformat("server embedding check error, key is {}", keys[i]));
      }
    }
#endif
    if (perf_condition)
      value_timer_.start();

    if (response_bytes > FLAGS_rdma_per_thread_response_limit_bytes) {
      LOG(ERROR) << "component=rdma_server event=batch_too_large shard="
                 << XPostoffice::GetInstance()->ServerID() << " thread_id="
                 << thread_id << " key_count=" << batch_get_kv_count
                 << " response_bytes=" << response_bytes << " limit_bytes="
                 << FLAGS_rdma_per_thread_response_limit_bytes;
      auto* status_word =
          reinterpret_cast<std::int32_t*>(dsm_->get_rdma_buffer());
      *status_word =
          static_cast<std::int32_t>(petps::RpcStatus::kBatchTooLarge);
      dsm_->write(reinterpret_cast<const char*>(status_word),
                  recv->receive_gaddr,
                  sizeof(std::int32_t),
                  true,
                  petps::WR_ID_GET);
      epoch_manager_->UnProtect();
      return;
    }

    auto* status_word = reinterpret_cast<std::int32_t*>(
        buf + batch_get_kv_count * FLAGS_value_size);
    if (flat_get_ok) {
      *status_word = static_cast<std::int32_t>(petps::RpcStatus::kOk);
    } else {
      *status_word =
          static_cast<std::int32_t>(petps::RpcStatus::kValueSizeMismatch);
    }

    epoch_manager_->UnProtect();
    GlobalAddress gaddr = recv->receive_gaddr;
    dsm_->write(buf, gaddr, response_bytes, true, petps::WR_ID_GET);
    if (perf_condition)
      value_timer_.end();

#ifdef RPC_DEBUG
    LOG(INFO) << "RPC done";
#endif
    if (perf_condition)
      get_parameter_timer_.end();
  }

  void RpcPsDescriptorDoorbell(const petps::RdmaDescriptorRequest& request,
                               int thread_id) {
    if (request.op ==
        static_cast<std::uint16_t>(petps::RdmaDescriptorOp::kGet)) {
      const int key_count = static_cast<int>(request.key_count);
      const int embedding_dim = static_cast<int>(request.embedding_dim);
      const std::size_t key_bytes =
          static_cast<std::size_t>(key_count) * sizeof(std::uint64_t);
      auto* keys = static_cast<std::uint64_t*>(
          raw_transport_->AllocateRegistered(key_bytes));
      raw_transport_->Read(keys,
                           request.keys_gaddr,
                           key_bytes,
                           request.request_id,
                           true);
      petps::RawVerbsCompletion completion{};
      while (!raw_transport_->Poll(&completion, FLAGS_rdma_wait_timeout_ms)) {
        std::this_thread::yield();
      }

      const std::size_t value_bytes =
          static_cast<std::size_t>(key_count) * FLAGS_value_size;
      auto* values =
          static_cast<float*>(raw_transport_->AllocateRegistered(value_bytes));
      std::memset(values, 0, value_bytes);
      petps::RpcStatus status = petps::RpcStatus::kOk;
      if (embedding_dim * static_cast<int>(sizeof(float)) != FLAGS_value_size) {
        status = petps::RpcStatus::kValueSizeMismatch;
      } else if (key_count > FLAGS_max_kv_num_per_request) {
        status = petps::RpcStatus::kBatchTooLarge;
      } else {
        const bool ok = cache_ps_->GetParameterFlat(
            base::ConstArray<std::uint64_t>(keys, key_count),
            values,
            key_count,
            embedding_dim,
            thread_id);
        if (!ok) {
          status = petps::RpcStatus::kValueSizeMismatch;
        }
      }

      if (status == petps::RpcStatus::kOk) {
        raw_transport_->Write(values,
                              request.response_gaddr,
                              value_bytes,
                              request.request_id,
                              true);
        while (!raw_transport_->Poll(&completion, FLAGS_rdma_wait_timeout_ms)) {
          std::this_thread::yield();
        }
      }
      auto* code =
          static_cast<std::int32_t*>(raw_transport_->AllocateRegistered(
              sizeof(std::int32_t)));
      *code = static_cast<std::int32_t>(status);
      raw_transport_->Write(code,
                            request.status_gaddr,
                            sizeof(*code),
                            request.request_id,
                            true);
      while (!raw_transport_->Poll(&completion, FLAGS_rdma_wait_timeout_ms)) {
        std::this_thread::yield();
      }
    } else if (request.op ==
               static_cast<std::uint16_t>(petps::RdmaDescriptorOp::kPut)) {
      const int key_count = static_cast<int>(request.key_count);
      const int embedding_dim = static_cast<int>(request.embedding_dim);
      auto* payload =
          static_cast<char*>(raw_transport_->AllocateRegistered(
              request.payload_bytes));
      petps::RpcStatus status = petps::RpcStatus::kOk;
      if (embedding_dim * static_cast<int>(sizeof(float)) != FLAGS_value_size) {
        status = petps::RpcStatus::kValueSizeMismatch;
      } else if (key_count > FLAGS_max_kv_num_per_request) {
        status = petps::RpcStatus::kBatchTooLarge;
      } else if (request.payload_bytes !=
                 petps::PutPayloadBytes(key_count, FLAGS_value_size)) {
        status = petps::RpcStatus::kInvalidPayload;
      } else {
        raw_transport_->Read(payload,
                             request.payload_gaddr,
                             request.payload_bytes,
                             request.request_id,
                             true);
        petps::RawVerbsCompletion completion{};
        while (!raw_transport_->Poll(&completion, FLAGS_rdma_wait_timeout_ms)) {
          std::this_thread::yield();
        }
        const auto* keys =
            reinterpret_cast<const std::uint64_t*>(payload);
        const auto* values = reinterpret_cast<const float*>(
            payload + static_cast<std::size_t>(key_count) *
                          sizeof(std::uint64_t));
        cache_ps_->PutDenseParameterBatch(
            keys, values, key_count, embedding_dim, thread_id);
      }
      auto* code =
          static_cast<std::int32_t*>(raw_transport_->AllocateRegistered(
              sizeof(std::int32_t)));
      *code = static_cast<std::int32_t>(status);
      raw_transport_->Write(code,
                            request.status_gaddr,
                            sizeof(*code),
                            request.request_id,
                            true);
      petps::RawVerbsCompletion completion{};
      while (!raw_transport_->Poll(&completion, FLAGS_rdma_wait_timeout_ms)) {
        std::this_thread::yield();
      }
    } else {
      LOG(ERROR) << "unknown descriptor op=" << request.op;
    }
  }

  void PollingThread(int thread_id) {
    auto_bind_core(0);
    dsm_->registerThread();
    VLOG(1) << "component=rdma_server event=polling_thread_ready thread_id="
            << thread_id;
    const int ready_threads = registered_polling_threads_.fetch_add(1) + 1;
    if (ready_threads == thread_count_) {
      const std::string key =
          "petps-server-ready-" +
          std::to_string(XPostoffice::GetInstance()->ServerID());
      XPostoffice::GetInstance()->MemCachedSet(key, "1");
      VLOG(1) << "component=rdma_server event=publish_ready_key key=" << key;
    }
    if (transport_mode_ == petps::RdmaTransportMode::kDescriptorDoorbell) {
      if (thread_id == 0) {
        raw_transport_->PublishAndConnect();
        raw_connected_.store(true, std::memory_order_release);
      } else {
        while (!raw_connected_.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        while (1) {
          std::this_thread::sleep_for(std::chrono::seconds(1));
        }
      }
      while (1) {
        petps::RawVerbsCompletion completion{};
        if (!raw_transport_->Poll(&completion, FLAGS_rdma_wait_timeout_ms)) {
          std::this_thread::yield();
          continue;
        }
        if (!completion.has_imm) {
          continue;
        }
        const std::uint32_t slot_id = completion.imm_data;
        petps::RdmaDescriptorRequest request{};
        request.client_node_id = static_cast<std::uint16_t>(completion.wr_id);
        request.slot_id = slot_id;
        request.descriptor_gaddr = GlobalAddress{
            static_cast<std::uint16_t>(dsm_->getMyNodeID()),
            FLAGS_rdma_put_v2_push_region_offset +
                (static_cast<std::uint64_t>(request.client_node_id) *
                     static_cast<std::uint64_t>(
                         FLAGS_rdma_put_v2_push_slots_per_client) +
                 slot_id) *
                    FLAGS_rdma_put_v2_push_slot_bytes};
        petps::RdmaDescriptorLaneConfig lane_config{};
        lane_config.region_offset = FLAGS_rdma_put_v2_push_region_offset;
        lane_config.slot_bytes = FLAGS_rdma_put_v2_push_slot_bytes;
        lane_config.slots_per_client =
            static_cast<std::uint32_t>(FLAGS_rdma_put_v2_push_slots_per_client);
        lane_config.machine_count =
            static_cast<std::uint32_t>(dsm_->get_conf()->machineNR);
        std::string error;
        if (!petps::ValidateDescriptorLane(request, lane_config, &error)) {
          LOG(ERROR) << "invalid descriptor lane: " << error;
          continue;
        }
        const char* payload = static_cast<const char*>(
            raw_transport_->LocalPointer(request.descriptor_gaddr));
        if (!petps::DecodeRdmaDescriptorRequest(
                std::string_view(payload, sizeof(petps::RdmaDescriptorRequest)),
                &request,
                &error)) {
          LOG(ERROR) << "descriptor decode error: " << error;
          continue;
        }
        RpcPsDescriptorDoorbell(request, thread_id);
      }
    }
    auto msg = RawMessage::get_new_msg();

    while (1) {
      msg->clear();
      uint64_t wr_id;
      RawMessage* recv;
      do {
        recv = dsm_->rpc_fast_wait(&wr_id);
        if (recv == nullptr && wr_id == petps::WR_ID_SG_GET) {
          // FB_LOG_EVERY_MS(ERROR, 1000)
          //     << "MaxPendingEpochNumPerThread = "
          //     << epoch_manager_->MaxPendingEpochNumPerThread();
          epoch_manager_->UnProtect();
        }
      } while (nullptr == recv);

      if (recv->type == GET_SERVER_THREADIDS) {
        RpcGetServerServingThreadIDs(recv);
      } else if (recv->type == PUT) {
        RpcPsPut(recv, thread_id);
      } else if (recv->type == GET) {
        RpcPsGet(recv, thread_id);
      } else {
        LOG(FATAL) << "unknown message type";
      }
    }
  }

private:
  std::vector<std::vector<SourceList>> sourcelists_;
  CachePS* cache_ps_;
  std::vector<std::thread> threads_;
  int thread_count_;
  DSM* dsm_;
  std::atomic_int registered_polling_threads_{0};
  xmh::Timer get_parameter_timer_;
  xmh::Timer index_timer_;
  xmh::Timer value_timer_;

  base::epoch::EpochManager* epoch_manager_;
  petps::RdmaTransportMode transport_mode_ =
      petps::RdmaTransportMode::kRawMessage;
  std::unique_ptr<petps::RawVerbsTransport> raw_transport_;
  std::atomic<bool> raw_connected_{false};

  constexpr static int kMaxThread = 128;
  uint64_t tp[kMaxThread][8];
};
} // namespace recstore

int main(int argc, char* argv[]) {
  folly::init(&argc, &argv);
  xmh::Reporter::StartReportThread();

  base::PMMmapRegisterCenter::GetConfig().use_dram = FLAGS_use_dram;
  base::PMMmapRegisterCenter::GetConfig().numa_id  = FLAGS_numa_id;

  extern int global_socket_id;
  global_socket_id = FLAGS_numa_id;
  LOG(INFO) << "set NUMA ID = " << FLAGS_numa_id;

  std::ifstream config_file(FLAGS_config_path);
  if (!config_file.is_open()) {
    LOG(FATAL) << "Cannot open config file: " << FLAGS_config_path;
  }
  nlohmann::json config;
  config_file >> config;

  auto cache_ps = std::make_unique<CachePS>(config["cache_ps"]);

  if (FLAGS_preload) {
    LOG(INFO) << "Loading fake data for preload";
    int64_t ps_capacity = config["cache_ps"]["base_kv_config"]["capacity"];
    cache_ps->LoadFakeData(ps_capacity * FLAGS_warmup_ratio, FLAGS_value_size);
  }

  recstore::PetPSServer parameterServiceImpl(cache_ps.get(), FLAGS_thread_num);
  parameterServiceImpl.Run();

  while (1) {
    auto micro_second1 = base::GetTimestamp();
    uint64_t tp_sum1   = parameterServiceImpl.GetThroughputCounterSum();
    std::this_thread::sleep_for(std::chrono::seconds(1));
    auto micro_second2 = base::GetTimestamp();
    uint64_t tp_sum2   = parameterServiceImpl.GetThroughputCounterSum();
    double tps = (tp_sum2 - tp_sum1) * 1.0 / (micro_second2 - micro_second1);
    printf("throughput %.4f Mkv/s\n", tps);
  }
  return 0;
}
