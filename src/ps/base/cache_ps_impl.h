#pragma once

#include <algorithm>
#include <atomic>
#include <boost/coroutine2/all.hpp>
#include <cstdint>
#include <experimental/filesystem>
#include <random>
#include <vector>

#include "base/array.h"
#include "base/factory.h"
#include "base/log.h" // NOLINT
#include "base/timer.h"
#include "parameters.h"
#include "storage/kv_engine/base_kv.h"
#include "storage/kv_engine/engine_factory.h"
#include "storage/kv_engine/engine_selector.h"
#include "optimizer/optimizer.h"

#ifdef ENABLE_PERF_REPORT
#  include <chrono>
#  include "base/report/report_client.h"
#endif

using boost::coroutines2::coroutine;

static const int KEY_CNT = 12543670;

template <typename key_t>
struct TaskElement {
  TaskElement(const base::ConstArray<key_t>& keys,
              const base::MutableArray<ParameterPack>& packs,
              std::atomic_bool* promise)
      : keys(keys), packs(packs), promise(promise) {}

  TaskElement() {}

  base::ConstArray<key_t> keys;
  base::MutableArray<ParameterPack> packs;
  std::atomic_bool* promise;
};

class CachePS {
public:
  using key_t = uint64_t;

  CachePS(json config) {
    LOG(INFO) << "cache ps config: " << config.dump(2);
    BaseKVConfig kv_config;
    kv_config.num_threads_ = config["num_threads"].get<int>();
    kv_config.json_config_ = config["base_kv_config"];
    auto r                 = base::ResolveEngine(kv_config);
    base_kv_.reset(base::Factory<BaseKV, const BaseKVConfig&>::NewInstance(
        r.engine, r.cfg));
  }

  ~CachePS() {}

  bool Initialize(const std::vector<std::string>& model_config_path,
                  const std::vector<std::string>& emb_file_path) {
    LOG(INFO) << "Before Load CKPT";
    LoadCkpt(model_config_path, emb_file_path);
    LOG(INFO) << "After Load CKPT";
    return true;
  }

  void Clear() { base_kv_->clear(); }

  void LoadFakeData(int64_t key_capacity, int value_size) {
    base_kv_->LoadFakeData(key_capacity, value_size);
  }

  bool LoadCkpt(const std::vector<std::string>& model_config_path,
                const std::vector<std::string>& emb_file_path) {
    // base_kv_->loadCkpt();
    // LoadFakeData(KEY_CNT);
    return true;
  }

  void PutSingleParameter(
      const uint64_t key, const void* data, const int dim, const int tid) {
    base_kv_->Put(key, std::string_view((char*)data, dim * sizeof(float)), tid);
  }

  void PutSingleParameter(const ParameterCompressItem* item, int tid) {
    auto key = item->key;
    auto dim = item->dim;
    base_kv_->Put(
        key, std::string_view((char*)item->data(), dim * sizeof(float)), tid);
  }

  void PutParameter(coroutine<void>::push_type& sink,
                    const ParameterCompressReader* reader,
                    int tid) {
    std::vector<uint64_t> keys_vec;
    std::vector<base::ConstArray<float>> values;
    for (int i = 0; i < reader->item_size(); i++) {
      keys_vec.emplace_back(reader->item(i)->key);
      values.emplace_back(
          (float*)reader->item(i)->data(), reader->item(i)->dim);
    }
    base::ConstArray<uint64_t> keys(keys_vec);

    base_kv_->BatchPut(sink, keys, &values, tid);
  }

  void PutParameter(const ParameterCompressReader* reader, int tid) {
    std::vector<uint64_t> keys_vec;
    std::vector<base::ConstArray<float>> values;
    for (int i = 0; i < reader->item_size(); i++) {
      keys_vec.emplace_back(reader->item(i)->key);
      values.emplace_back(
          (float*)reader->item(i)->data(), reader->item(i)->dim);
    }
    base::ConstArray<uint64_t> keys(keys_vec);

    base_kv_->BatchPut(keys, &values, tid);
  }

  bool GetParameterRun2Completion(key_t key, ParameterPack& pack, int tid) {
    std::vector<uint64_t> keys = {key};
    base::ConstArray<uint64_t> keys_array(keys);
    std::vector<base::ConstArray<float>> values;

    base_kv_->BatchGet(keys_array, &values, tid);
    base::ConstArray<float> value = values[0];

    if (value.Size() == 0) {
      pack.key      = key;
      pack.dim      = 0;
      pack.emb_data = nullptr;
      FB_LOG_EVERY_MS(ERROR, 1000) << "key " << key << " not existing";
      return false;
    }
    pack.key      = key;
    pack.dim      = value.Size();
    pack.emb_data = value.Data();
    // LOG(ERROR) << "Get key " << key << " dim " << pack.dim;
    return true;
  }

  bool GetParameterRun2Completion(base::ConstArray<uint64_t> keys,
                                  std::vector<ParameterPack>& packs,
                                  int tid) {
#ifdef ENABLE_PERF_REPORT
    auto start_time = std::chrono::high_resolution_clock::now();
#endif
    std::vector<base::ConstArray<float>> values;
    base_kv_->BatchGet(keys, &values, tid);

    for (int i = 0; i < keys.Size(); i++) {
      packs.emplace_back(keys[i], values[i].Size(), values[i].Data());
    }

#ifdef ENABLE_PERF_REPORT
    auto end_time = std::chrono::high_resolution_clock::now();
    double start_us =
        std::chrono::duration_cast<std::chrono::microseconds>(
            start_time.time_since_epoch())
            .count();
    auto duration =
        std::chrono::duration_cast<std::chrono::microseconds>(
            end_time - start_time)
            .count();

    std::string report_id = "cache_ps::GetParameterRun2Completion|" +
                            std::to_string(static_cast<uint64_t>(start_us));

    report("embread_stages",
           report_id.c_str(),
           "duration_us",
           static_cast<double>(duration));

    report("embread_stages",
           report_id.c_str(),
           "request_size",
           static_cast<double>(keys.Size()));

    std::string unique_id =
        "embread_debug|" + std::to_string(static_cast<uint64_t>(start_us));
    FlameGraphData fg_data = {
        "cache_ps::GetParameterRun2Completion",
        start_us,
        3, // level
        static_cast<double>(duration),
        static_cast<double>(duration)};
    report_flame_graph("emb_read_flame_map", unique_id.c_str(), fg_data);
#endif
    return true;
  }

  bool GetParameterRun2Completion(
      coroutine<void>::push_type& sink,
      base::ConstArray<uint64_t> keys,
      std::vector<ParameterPack>& pack,
      int tid) {
    std::vector<base::ConstArray<float>> values;

    base_kv_->BatchGet(sink, keys, &values, tid);

    for (int i = 0; i < keys.Size(); i++) {
      pack.emplace_back(keys[i], values[i].Size(), values[i].Data());
    }
    return true;
  }

  /// optimizer interface

  bool InitTable(const std::string& table_name,
                 uint64_t num_embeddings,
                 uint64_t embedding_dim) {
    if (!optimizer_) {
      // TODO: optimizer type from config
      optimizer_ = std::make_unique<SGD>(0.01);
    }

    EmbeddingTableConfig config{num_embeddings, embedding_dim};
    optimizer_->Init({table_name}, config, base_kv_.get());
    return true;
  }

  bool UpdateParameter(const std::string& table_name,
                       const ParameterCompressReader* reader,
                       unsigned tid) {
    if (!optimizer_) {
      LOG(ERROR) << "Optimizer not initialized. Please call InitTable first.";
      return false;
    }

    optimizer_->Update(table_name, reader, tid);
    return true;
  }

private:
  std::unique_ptr<BaseKV> base_kv_;
  std::unique_ptr<Optimizer> optimizer_;
  std::atomic<bool> stopFlag_{false};
};
