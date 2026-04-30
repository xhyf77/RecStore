#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>

#include "framework/common/op_runtime_support.h"

namespace {

TEST(OpRuntimeSupportTest, BackendNameFromConfigNormalizesKnownBackends) {
  EXPECT_EQ(recstore::BackendNameFromConfig(
                json{{"cache_ps", {{"ps_type", "grpc"}}}}),
            "grpc");
  EXPECT_EQ(recstore::BackendNameFromConfig(
                json{{"cache_ps", {{"ps_type", "BRPC"}}}}),
            "brpc");
  EXPECT_EQ(recstore::BackendNameFromConfig(
                json{{"cache_ps", {{"ps_type", "RDMA"}}}}),
            "rdma");
  EXPECT_EQ(recstore::BackendNameFromConfig(
                json{{"cache_ps", {{"ps_type", "LOCAL_SHM"}}}}),
            "local_shm");
}

TEST(OpRuntimeSupportTest, LoadConfigFromFileParsesJsonDocument) {
  const std::string path = "/tmp/recstore_op_runtime_support_" +
                           std::to_string(::getpid()) + ".json";
  {
    std::ofstream out(path);
    ASSERT_TRUE(out.is_open());
    out << R"({"cache_ps":{"ps_type":"BRPC"},"client":{"port":15123}})";
  }

  const json config = recstore::load_config_from_file(path);
  EXPECT_EQ(config["cache_ps"]["ps_type"].get<std::string>(), "BRPC");
  EXPECT_EQ(config["client"]["port"].get<int>(), 15123);

  std::remove(path.c_str());
}

} // namespace
