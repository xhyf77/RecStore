#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "ps/rdma/rdma_protocol.h"

namespace petps {

TEST(RdmaProtocolTest, EncodesAndDecodesFixedWidthPutPayload) {
  std::vector<uint64_t> keys             = {11, 12};
  std::vector<std::vector<float>> values = {
      {1.0f, 2.0f, 3.0f, 4.0f},
      {5.0f, 6.0f, 7.0f, 8.0f},
  };

  std::string payload = EncodePutPayload(keys, values);

  DecodedPutPayload decoded;
  std::string error;
  ASSERT_TRUE(DecodePutPayload(payload, &decoded, &error)) << error;
  EXPECT_EQ(decoded.embedding_dim, 4u);
  EXPECT_EQ(decoded.keys, keys);
  EXPECT_EQ(decoded.values.size(), 8u);
  EXPECT_FLOAT_EQ(decoded.values[0], 1.0f);
  EXPECT_FLOAT_EQ(decoded.values[7], 8.0f);
}

TEST(RdmaProtocolTest, RejectsTruncatedPayload) {
  std::string payload(sizeof(PutPayloadHeader) - 1, '\0');
  DecodedPutPayload decoded;
  std::string error;
  EXPECT_FALSE(DecodePutPayload(payload, &decoded, &error));
  EXPECT_NE(error.find("header"), std::string::npos);
}

TEST(RdmaProtocolTest, ComputesFixedSlotResponseBytes) {
  EXPECT_EQ(FixedSlotResponseBytes(3, 16), 3u * 16u + sizeof(std::int32_t));
}

TEST(RdmaProtocolTest, EncodesAndDecodesDescriptorDoorbellRequest) {
  RdmaDescriptorRequest req{};
  req.magic            = kRdmaDescriptorMagic;
  req.version          = kRdmaDescriptorVersionV1;
  req.op               = static_cast<std::uint16_t>(RdmaDescriptorOp::kGet);
  req.request_id       = 42;
  req.client_node_id   = 2;
  req.client_thread_id = 3;
  req.lane_id          = 9;
  req.slot_id          = 1;
  req.key_count        = 4;
  req.embedding_dim    = 8;
  req.keys_gaddr       = GlobalAddress{2, 4096};
  req.payload_gaddr    = GlobalAddress{0, 0};
  req.response_gaddr   = GlobalAddress{2, 8192};
  req.status_gaddr     = GlobalAddress{2, 16384};
  req.payload_bytes    = 0;
  req.response_bytes   = FixedSlotResponseBytes(4, 8 * sizeof(float));

  std::string payload;
  std::string error;
  ASSERT_TRUE(EncodeRdmaDescriptorRequest(req, &payload, &error)) << error;

  RdmaDescriptorRequest decoded{};
  ASSERT_TRUE(DecodeRdmaDescriptorRequest(payload, &decoded, &error)) << error;
  EXPECT_EQ(decoded.request_id, req.request_id);
  EXPECT_EQ(decoded.op, req.op);
  EXPECT_EQ(decoded.key_count, req.key_count);
  EXPECT_EQ(decoded.response_bytes, req.response_bytes);
  EXPECT_EQ(decoded.response_gaddr.nodeID, req.response_gaddr.nodeID);
  EXPECT_EQ(decoded.response_gaddr.offset, req.response_gaddr.offset);
}

TEST(RdmaProtocolTest, RejectsInvalidDescriptorRequests) {
  RdmaDescriptorRequest req{};
  req.magic          = kRdmaDescriptorMagic;
  req.version        = kRdmaDescriptorVersionV1;
  req.op             = static_cast<std::uint16_t>(RdmaDescriptorOp::kGet);
  req.key_count      = 1;
  req.embedding_dim  = 4;
  req.keys_gaddr     = GlobalAddress{1, 4096};
  req.response_gaddr = GlobalAddress{1, 8192};
  req.status_gaddr   = GlobalAddress{1, 12288};
  req.response_bytes = FixedSlotResponseBytes(1, 4 * sizeof(float));

  std::string payload;
  std::string error;
  ASSERT_TRUE(EncodeRdmaDescriptorRequest(req, &payload, &error)) << error;

  payload[0] = '\0';
  RdmaDescriptorRequest decoded{};
  EXPECT_FALSE(DecodeRdmaDescriptorRequest(payload, &decoded, &error));
  EXPECT_NE(error.find("magic"), std::string::npos);
}

TEST(RdmaProtocolTest, ValidatesDescriptorLaneBounds) {
  RdmaDescriptorLaneConfig cfg{};
  cfg.region_offset    = 64 * 1024 * 1024;
  cfg.slot_bytes       = 4096;
  cfg.slots_per_client = 8;
  cfg.machine_count    = 4;

  RdmaDescriptorRequest req{};
  req.client_node_id = 2;
  req.slot_id        = 3;
  req.descriptor_gaddr =
      GlobalAddress{0, cfg.region_offset + (2 * 8 + 3) * 4096};

  std::string error;
  EXPECT_TRUE(ValidateDescriptorLane(req, cfg, &error)) << error;

  req.descriptor_gaddr.offset = cfg.region_offset + 4096;
  EXPECT_FALSE(ValidateDescriptorLane(req, cfg, &error));
  EXPECT_NE(error.find("lane"), std::string::npos);
}

} // namespace petps
