#include "core/db.h"
#include "core/db_factory.h"
#include "utils/properties.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../../../src/storage/kv_engine/base_kv.h"
#include "../../../src/storage/kv_engine/engine_factory.h"
#include "../../../src/storage/kv_engine/engine_selector.h"
#include "../../../src/memory/shm_file.h"   // PMMmapRegisterCenter

namespace ycsbc {

namespace {
// ---- per-thread tid management (stable and bounded) ----
static std::atomic<unsigned> g_next_tid{0};
static std::atomic<unsigned> g_tid_limit{0};       // 0 = unlimited (no modulo)
static std::atomic<bool>     g_force_tid_zero{false};
static inline unsigned ThreadTid() {
  if (g_force_tid_zero.load(std::memory_order_relaxed)) return 0u;
  thread_local unsigned t = g_next_tid.fetch_add(1, std::memory_order_relaxed);
  unsigned lim = g_tid_limit.load(std::memory_order_relaxed);
  return lim ? (t % lim) : t;
}

// Stable FNV-1a 64-bit hash for string keys.
static inline uint64_t FNV1a64(const std::string &s) {
  const uint64_t P = 1099511628211ULL;
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : s) { h ^= c; h *= P; }
  return h;
}

static inline uint64_t ToKey(const std::string &key) {
  // Fast numeric-tail parse; fallback to FNV1a
  const char *p = key.c_str();
  while (*p && (*p < '0' || *p > '9')) ++p;
  if (*p) {
    uint64_t v = 0;
    while (*p >= '0' && *p <= '9') { v = v*10 + (uint64_t)(*p - '0'); ++p; }
    return v;
  }
  return FNV1a64(key);
}

// Serialization for compat mode (name=value\x1F...)
static inline void SerializeRow(const std::vector<DB::Field> &values, std::string &out) {
  size_t total = 0;
  if (!values.empty()) {
    total = values.size() - 1; // separators
    for (const auto &f : values) total += f.name.size() + 1 /* '=' */ + f.value.size();
  }
  out.clear();
  out.reserve(total);
  const char SEP = '\x1f';
  for (size_t i = 0; i < values.size(); ++i) {
    const auto &f = values[i];
    out.append(f.name.data(), f.name.size());
    out.push_back('=');
    out.append(f.value.data(), f.value.size());
    if (i + 1 < values.size()) out.push_back(SEP);
  }
}

static inline void DeserializeRow(std::vector<DB::Field> *out, const char *data, size_t n) {
  out->clear();
  const char SEP = '\x1f';
  size_t i = 0;
  while (i < n) {
    size_t eq = i;
    while (eq < n && data[eq] != '=') ++eq;
    if (eq >= n) break;
    size_t j = eq + 1;
    while (j < n && data[j] != SEP) ++j;
    DB::Field f;
    f.name.assign(data + i, data + eq);
    f.value.assign(data + eq + 1, data + j);
    out->push_back(std::move(f));
    i = j + 1;
  }
}

constexpr const char *PROP_DB_PATH        = "hybridkv.path";
constexpr const char *PROP_SHM_CAP        = "hybridkv.shmcapacity";
constexpr const char *PROP_SSD_CAP        = "hybridkv.ssdcapacity";
constexpr const char *PROP_FORCE_TID_ZERO = "hybridkv.force_tid_zero";
constexpr const char *PROP_MODE           = "hybridkv.mode";           // perf | compat
constexpr const char *PROP_SYN_BYTES      = "hybridkv.synthetic_bytes"; // >0 in perf
constexpr const char *PROP_READ_RETURN    = "hybridkv.read_return";    // none | blob | parse
constexpr const char *DEF_THREADCOUNT    =  "16";
constexpr const char *DEF_DB_PATH = "third_party/ycsb/data-store";
constexpr const char *DEF_SHM_CAP = "268435456";     // 256MB default
constexpr const char *DEF_SSD_CAP = "0";

static inline bool IsTrue(const std::string& value) {
  return value == "1" || value == "true" || value == "TRUE" ||
         value == "yes" || value == "YES";
}

static inline uint64_t GetU64(const utils::Properties& props,
                              const char* key,
                              uint64_t default_value) {
  try {
    return std::stoull(props.GetProperty(key, std::to_string(default_value)));
  } catch (...) {
    return default_value;
  }
}

static inline uint32_t GetU32(const utils::Properties& props,
                              const char* key,
                              uint32_t default_value) {
  try {
    return static_cast<uint32_t>(
        std::stoul(props.GetProperty(key, std::to_string(default_value))));
  } catch (...) {
    return default_value;
  }
}

static inline double GetDouble(const utils::Properties& props,
                               const char* key,
                               double default_value) {
  try {
    return std::stod(props.GetProperty(key, std::to_string(default_value)));
  } catch (...) {
    return default_value;
  }
}

static BaseKVConfig BuildKVEngineConfig(const utils::Properties& props,
                                        const std::string& db_path,
                                        unsigned thread_count,
                                        uint32_t value_size_hint) {
  const uint64_t record_count =
      GetU64(props, "recordcount", GetU64(props, "kvengine.capacity", 1000000));
  const uint64_t capacity =
      GetU64(props, "kvengine.capacity", std::max<uint64_t>(record_count, 1));
  const uint64_t value_bytes =
      std::max<uint64_t>(value_size_hint == 0 ? 128 : value_size_hint, 1);
  const uint64_t default_dram_bytes =
      GetU64(props, PROP_SHM_CAP, capacity * value_bytes * 2);
  const uint64_t default_ssd_bytes =
      GetU64(props, PROP_SSD_CAP, capacity * value_bytes * 2);
  const std::string index_type = props.GetProperty(
      "kvengine.index.type", "DRAM_EXTENDIBLE_HASH");
  const std::string value_type = props.GetProperty(
      "kvengine.value.type", "DRAM_VALUE_STORE");
  const std::string dram_allocator = props.GetProperty(
      "kvengine.value.dram_allocator.type", "PERSIST_LOOP_SLAB");
  const std::string ssd_allocator = props.GetProperty(
      "kvengine.value.ssd_allocator.type", "SSD_BUDDY");
  const std::string io_backend =
      props.GetProperty("kvengine.ssd.io.type", "IOURING");
  const uint32_t queue_depth =
      GetU32(props, "kvengine.ssd.io.queue_depth", 512);
  const uint64_t ssd_base_offset =
      GetU64(props, "kvengine.ssd.io.base_offset_bytes", 4096);
  const uint64_t index_base_offset =
      GetU64(props, "kvengine.index.io.base_offset_bytes", 0);
  const std::string value_file = props.GetProperty(
      "kvengine.ssd.value_file", db_path + "/value_pages.db");
  const std::string index_file = props.GetProperty(
      "kvengine.ssd.index_file", db_path + "/index_pages.db");

  BaseKVConfig cfg;
  cfg.num_threads_ = static_cast<int>(thread_count);
  cfg.json_config_ = {
      {"path", db_path},
      {"capacity", capacity},
      {"index", {{"type", index_type}}},
      {"value",
       {{"type", value_type}, {"default_value_size_hint", value_bytes}}}};

  if (index_type == "SSD" || index_type == "SSD_EXTENDIBLE_HASH") {
    cfg.json_config_["index"]["io"] =
        {{"type", io_backend},
         {"file_path", index_file},
         {"queue_depth", queue_depth},
         {"base_offset_bytes", index_base_offset}};
  }

  if (value_type == "DRAM_VALUE_STORE" || value_type == "TIERED_VALUE_STORE") {
    cfg.json_config_["value"]["dram_allocator"] =
        {{"type", dram_allocator},
         {"capacity_bytes", GetU64(props,
                                    "kvengine.value.dram_allocator.capacity_bytes",
                                    default_dram_bytes)}};
  }

  if (value_type == "SSD_VALUE_STORE" || value_type == "TIERED_VALUE_STORE") {
    json ssd = {
        {"type", ssd_allocator},
        {"capacity_bytes", GetU64(props,
                                   "kvengine.value.ssd_allocator.capacity_bytes",
                                   default_ssd_bytes)},
        {"io",
         {{"type", io_backend},
          {"file_path", value_file},
          {"queue_depth", queue_depth},
          {"base_offset_bytes", ssd_base_offset}}}};
    if (ssd_allocator == "SSD_BUDDY") {
      ssd["min_block_size"] =
          GetU32(props, "kvengine.value.ssd_allocator.min_block_size", 128);
      ssd["max_block_size"] =
          GetU32(props, "kvengine.value.ssd_allocator.max_block_size", 4096);
    }
    cfg.json_config_["value"]["ssd_allocator"] = std::move(ssd);
  }

  if (value_type == "TIERED_VALUE_STORE") {
    cfg.json_config_["value"]["tiering"] =
        {{"cache_policy", props.GetProperty("kvengine.value.tiering.cache_policy", "LRU")},
         {"high_watermark_ratio",
          GetDouble(props, "kvengine.value.tiering.high_watermark_ratio", 0.85)}};
  }

  return base::ResolveEngine(std::move(cfg)).cfg;
}
} // anonymous namespace

class HybridKVDB : public DB {
 public:
  HybridKVDB() = default;
  ~HybridKVDB() override = default;

  void Init() override;
  void Cleanup() override;

  Status Read(const std::string &table, const std::string &key,
              const std::vector<std::string> *fields,
              std::vector<Field> &result) override;

  Status Scan(const std::string &table, const std::string &key, int len,
              const std::vector<std::string> *fields,
              std::vector<std::vector<Field>> &result) override;

  Status Update(const std::string &table, const std::string &key,
                std::vector<Field> &values) override;

  Status Insert(const std::string &table, const std::string &key,
                std::vector<Field> &values) override;

  Status Delete(const std::string &table, const std::string &key) override;

 private:
  enum class Mode { kPerf, kCompat };

  static std::mutex            mu_;
  static std::atomic<uint32_t> ref_cnt_;
  static BaseKV               *engine_;

  static Mode                  mode_;
  static uint32_t              syn_bytes_;        // perf: value size (>0 uses synthetic)
  static uint32_t              read_policy_;      // 0 none, 1 blob, 2 parse

  // Per-thread buffers to avoid allocations
  thread_local static std::string tl_ser_buf_;
  thread_local static std::string tl_blob_;
  thread_local static std::string tl_syn_buf_;
};

// static members
std::mutex            HybridKVDB::mu_;
std::atomic<uint32_t> HybridKVDB::ref_cnt_{0};
BaseKV*              HybridKVDB::engine_ = nullptr;
HybridKVDB::Mode      HybridKVDB::mode_   = HybridKVDB::Mode::kPerf;
uint32_t              HybridKVDB::syn_bytes_  = 0;
uint32_t              HybridKVDB::read_policy_= 0; // none
thread_local std::string HybridKVDB::tl_ser_buf_;
thread_local std::string HybridKVDB::tl_blob_;
thread_local std::string HybridKVDB::tl_syn_buf_;

void HybridKVDB::Init() {
  std::lock_guard<std::mutex> lk(mu_);
  if (ref_cnt_.fetch_add(1, std::memory_order_acq_rel) > 0) return;

  base::PMMmapRegisterCenter::GetConfig().use_dram = true; // keep your previous toggle
  const utils::Properties &props = *props_;

  const std::string db_path  = props.GetProperty(PROP_DB_PATH, DEF_DB_PATH);
  const std::string use_dram = props.GetProperty("hybridkv.use_dram", "true");
  base::PMMmapRegisterCenter::GetConfig().use_dram = IsTrue(use_dram);

  unsigned tc = 1;
  try { tc = std::max(1u, (unsigned)std::stoul(props.GetProperty("hybridkv.threadcount",DEF_THREADCOUNT))); } catch (...) { tc = 1; }
  g_tid_limit.store(tc, std::memory_order_relaxed);
  std::cout << "threadnum" << tc << std::endl;
  const std::string force_tid_zero = props.GetProperty(PROP_FORCE_TID_ZERO, "false");
  const bool ftz = IsTrue(force_tid_zero);
  g_force_tid_zero.store(ftz, std::memory_order_relaxed);

  const std::string m = props.GetProperty(PROP_MODE, "perf");
  mode_ = (m == "compat" ? Mode::kCompat : Mode::kPerf);

  syn_bytes_ = 0;
  try { syn_bytes_ = (uint32_t) std::stoul(props.GetProperty(PROP_SYN_BYTES, "0")); } catch (...) { syn_bytes_ = 0; }

  const std::string rp = props.GetProperty(PROP_READ_RETURN, (mode_==Mode::kPerf?"none":"parse"));
  read_policy_ = (rp=="none"?0 : rp=="blob"?1 : 2);

  BaseKVConfig cfg = BuildKVEngineConfig(props, db_path, tc, syn_bytes_);
  auto resolved = base::ResolveEngine(std::move(cfg));
  engine_ = base::Factory<BaseKV, const BaseKVConfig&>::NewInstance(
      resolved.engine, resolved.cfg);
}

void HybridKVDB::Cleanup() {
  std::lock_guard<std::mutex> lk(mu_);
  if (ref_cnt_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    delete engine_;
    engine_ = nullptr;
    g_next_tid.store(0, std::memory_order_relaxed);
    g_tid_limit.store(0, std::memory_order_relaxed);
    g_force_tid_zero.store(false, std::memory_order_relaxed);
  }
}

DB::Status HybridKVDB::Read(const std::string &, const std::string &key,
                            const std::vector<std::string> *fields,
                            std::vector<Field> &result) {
  if (!engine_) return kError;

  if (read_policy_ == 0) {
    return engine_->Exists(ToKey(key), ThreadTid()) ? kOK : kNotFound;
  }

  engine_->Get(ToKey(key), tl_blob_, ThreadTid());
  if (tl_blob_.empty()) return kNotFound;

  if (read_policy_ == 1) {
    // blob — just confirm length but do not parse
    return kOK;
  }

  // parse
  std::vector<Field> tmp;
  DeserializeRow(&tmp, tl_blob_.data(), tl_blob_.size());
  if (fields && !fields->empty()) {
    result.clear();
    result.reserve(fields->size());
    for (const auto &name : *fields) {
      auto it = std::find_if(tmp.begin(), tmp.end(), [&](const Field &f){ return f.name == name; });
      if (it != tmp.end()) result.push_back(*it);
    }
  } else {
    result.swap(tmp);
  }
  return kOK;
}

DB::Status HybridKVDB::Scan(const std::string &, const std::string & /*key*/, int /*len*/,
                            const std::vector<std::string> * /*fields*/,
                            std::vector<std::vector<Field>> & /*result*/) {
  return kNotFound; // engine isn't ordered
}

DB::Status HybridKVDB::Insert(const std::string &, const std::string &key,
                              std::vector<Field> &values) {
  if (!engine_) return kError;
  const uint64_t k = ToKey(key);
  const unsigned tid = ThreadTid();

  if (mode_ == Mode::kPerf) {
    if (syn_bytes_) {
      // synthesize fixed-size payload once per thread
      if (tl_syn_buf_.size() != syn_bytes_) {
        tl_syn_buf_.assign(syn_bytes_, '\0');
        // deterministic pattern helps profile cacheline behaviour
        for (size_t i = 0; i < tl_syn_buf_.size(); ++i) tl_syn_buf_[i] = (char)(i * 131u);
      }
      engine_->Put(k, std::string_view(tl_syn_buf_.data(), tl_syn_buf_.size()), tid);
      return kOK;
    }
    // perf but no synthetic size — pack only values payload (skip names)
    size_t total = 0;
    for (const auto &f : values) total += f.value.size();
    tl_ser_buf_.clear();
    tl_ser_buf_.reserve(total);
    for (const auto &f : values) tl_ser_buf_.append(f.value);
    engine_->Put(k, std::string_view(tl_ser_buf_.data(), tl_ser_buf_.size()), tid);
    return kOK;
  }

  // compat: serialize name=value pairs
  SerializeRow(values, tl_ser_buf_);
  engine_->Put(k, std::string_view(tl_ser_buf_.data(), tl_ser_buf_.size()), tid);
  return kOK;
}

DB::Status HybridKVDB::Update(const std::string &, const std::string &key,
                              std::vector<Field> &values) {
  if (!engine_) return kError;
  const uint64_t k = ToKey(key);
  const unsigned tid = ThreadTid();

  if (mode_ == Mode::kPerf) {
    // In perf mode, treat Update == Insert (overwrite)
    return Insert("", key, values);
  }

  // compat read-modify-write
  engine_->Get(k, tl_blob_, tid);
  if (tl_blob_.empty()) return kNotFound;
  std::vector<Field> cur;
  DeserializeRow(&cur, tl_blob_.data(), tl_blob_.size());
  for (auto &nf : values) {
    auto it = std::find_if(cur.begin(), cur.end(), [&](const Field &f){ return f.name == nf.name; });
    if (it != cur.end()) it->value = nf.value; else cur.push_back(nf);
  }
  SerializeRow(cur, tl_ser_buf_);
  engine_->Put(k, std::string_view(tl_ser_buf_.data(), tl_ser_buf_.size()), tid);
  return kOK;
}

DB::Status HybridKVDB::Delete(const std::string &, const std::string &key) {
  if (!engine_) return kError;
  engine_->Put(ToKey(key), std::string_view(), ThreadTid()); // tombstone by empty value
  return kOK;
}

static DB *NewHybridKVDB() { return new HybridKVDB; }
const bool registered_hybridkv = DBFactory::RegisterDB("kvdb", NewHybridKVDB);

} // namespace ycsbc
