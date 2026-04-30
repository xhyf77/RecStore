#include "brpc_ps_server.h"

#include <gtest/gtest.h>

namespace recstore {
namespace {

TEST(BRPCParameterServerUnitTest, SelectsAllShardsWhenNoLocalShardProvided) {
  nlohmann::json cache_ps = {
      {"num_shards", 2},
      {"servers",
       {{{"host", "127.0.0.1"}, {"port", 15123}, {"shard", 0}},
        {{"host", "127.0.0.1"}, {"port", 15124}, {"shard", 1}}}}};

  auto selected = SelectBRPCShardConfigs(cache_ps, std::nullopt);

  ASSERT_EQ(selected.size(), 2);
  EXPECT_EQ(selected[0]["shard"].get<int>(), 0);
  EXPECT_EQ(selected[1]["shard"].get<int>(), 1);
}

TEST(BRPCParameterServerUnitTest, SelectsOnlyMatchingLocalShard) {
  nlohmann::json cache_ps = {
      {"num_shards", 2},
      {"servers",
       {{{"host", "10.0.0.1"}, {"port", 15123}, {"shard", 0}},
        {{"host", "10.0.0.2"}, {"port", 15124}, {"shard", 1}}}}};

  auto selected = SelectBRPCShardConfigs(cache_ps, 1);

  ASSERT_EQ(selected.size(), 1);
  EXPECT_EQ(selected[0]["host"].get<std::string>(), "10.0.0.2");
  EXPECT_EQ(selected[0]["port"].get<int>(), 15124);
  EXPECT_EQ(selected[0]["shard"].get<int>(), 1);
}

TEST(BRPCParameterServerUnitTest, ReturnsEmptyWhenLocalShardMissing) {
  nlohmann::json cache_ps = {
      {"num_shards", 2},
      {"servers",
       {{{"host", "10.0.0.1"}, {"port", 15123}, {"shard", 0}},
        {{"host", "10.0.0.2"}, {"port", 15124}, {"shard", 1}}}}};

  auto selected = SelectBRPCShardConfigs(cache_ps, 3);

  EXPECT_TRUE(selected.empty());
}

} // namespace
} // namespace recstore
