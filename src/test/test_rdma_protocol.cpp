#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "ps/rdma/raw_verbs_transport.h"
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

TEST(RdmaProtocolTest, ComputesDescriptorClientPermanentSlotBytes) {
  EXPECT_EQ(DescriptorClientPermanentSlotBytes(8, 4096),
            8u * (4096u + sizeof(std::atomic<std::int32_t>)));
  EXPECT_EQ(DescriptorClientPermanentSlotBytes(0, 4096), 0u);
}

TEST(RdmaProtocolTest, EncodesAndDecodesDescriptorDoorbellRequest) {
  RdmaDescriptorRequest req{};
  req.magic = kRdmaDescriptorMagic;
  req.version = kRdmaDescriptorVersionV1;
  req.op = static_cast<std::uint16_t>(RdmaDescriptorOp::kGet);
  req.request_id = 42;
  req.client_node_id = 2;
  req.client_thread_id = 3;
  req.lane_id = 9;
  req.slot_id = 1;
  req.key_count = 4;
  req.embedding_dim = 8;
  req.response_gaddr = GlobalAddress{2, 8192};
  req.status_gaddr = GlobalAddress{2, 16384};
  req.payload_bytes = 4 * sizeof(std::uint64_t);
  req.response_bytes = FixedSlotResponseBytes(4, 8 * sizeof(float));

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

TEST(RdmaProtocolTest, WritesDescriptorRequestIntoExistingBuffer) {
  RdmaDescriptorRequest req{};
  req.magic = kRdmaDescriptorMagic;
  req.version = kRdmaDescriptorVersionV1;
  req.op = static_cast<std::uint16_t>(RdmaDescriptorOp::kGet);
  req.request_id = 7;
  req.client_node_id = 1;
  req.client_thread_id = 2;
  req.slot_id = 3;
  req.key_count = 4;
  req.embedding_dim = 8;
  req.payload_bytes = 4 * sizeof(std::uint64_t);
  req.response_bytes = FixedSlotResponseBytes(4, 8 * sizeof(float));

  char buffer[sizeof(RdmaDescriptorRequest)]{};
  std::string error;
  ASSERT_TRUE(WriteRdmaDescriptorRequest(req, buffer, sizeof(buffer), &error))
      << error;

  RdmaDescriptorRequest decoded{};
  ASSERT_TRUE(DecodeRdmaDescriptorRequest(
      std::string_view(buffer, sizeof(buffer)), &decoded, &error))
      << error;
  EXPECT_EQ(decoded.request_id, req.request_id);
  EXPECT_EQ(decoded.client_thread_id, req.client_thread_id);
}

TEST(RdmaProtocolTest, RejectsDescriptorWriteToSmallBuffer) {
  RdmaDescriptorRequest req{};
  req.magic = kRdmaDescriptorMagic;
  req.version = kRdmaDescriptorVersionV1;
  req.op = static_cast<std::uint16_t>(RdmaDescriptorOp::kGet);
  req.key_count = 1;
  req.embedding_dim = 4;

  char buffer[sizeof(RdmaDescriptorRequest) - 1]{};
  std::string error;
  EXPECT_FALSE(WriteRdmaDescriptorRequest(req, buffer, sizeof(buffer), &error));
  EXPECT_NE(error.find("buffer"), std::string::npos);
}

TEST(RdmaProtocolTest, RejectsInvalidDescriptorRequests) {
  RdmaDescriptorRequest req{};
  req.magic = kRdmaDescriptorMagic;
  req.version = kRdmaDescriptorVersionV1;
  req.op = static_cast<std::uint16_t>(RdmaDescriptorOp::kGet);
  req.key_count = 1;
  req.embedding_dim = 4;
  req.response_gaddr = GlobalAddress{1, 8192};
  req.status_gaddr = GlobalAddress{1, 12288};
  req.payload_bytes = sizeof(std::uint64_t);
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
  cfg.region_offset = 64 * 1024 * 1024;
  cfg.slot_bytes = 4096;
  cfg.slots_per_client = 8;
  cfg.machine_count = 4;

  RdmaDescriptorRequest req{};
  req.client_node_id = 2;
  req.slot_id = 3;
  req.descriptor_gaddr =
      GlobalAddress{0, cfg.region_offset + (2 * 8 + 3) * 4096};

  std::string error;
  EXPECT_TRUE(ValidateDescriptorLane(req, cfg, &error)) << error;

  req.descriptor_gaddr.offset = cfg.region_offset + 4096;
  EXPECT_FALSE(ValidateDescriptorLane(req, cfg, &error));
  EXPECT_NE(error.find("lane"), std::string::npos);
}

TEST(RdmaProtocolTest, DescriptorDoorbellPostPlanUsesWriteWithImm) {
  const RdmaDescriptorDoorbellPostPlan plan =
      GetRdmaDescriptorDoorbellPostPlan();

  EXPECT_TRUE(plan.use_write_with_imm);
  EXPECT_FALSE(plan.wait_write_with_imm_completion);
  EXPECT_FALSE(plan.signal_descriptor_write);
  EXPECT_FALSE(plan.wait_descriptor_write_completion);
  EXPECT_FALSE(plan.signal_doorbell);
  EXPECT_FALSE(plan.wait_doorbell_completion);
}

TEST(RdmaProtocolTest, DescriptorPushPayloadPostPlanUsesRawWriteWithoutWait) {
  const RdmaDescriptorPushPayloadPostPlan plan =
      GetRdmaDescriptorPushPayloadPostPlan();

  EXPECT_TRUE(plan.use_raw_write);
  EXPECT_FALSE(plan.signal_payload_write);
  EXPECT_FALSE(plan.wait_payload_write_completion);
}

TEST(RdmaProtocolTest, DescriptorGetAsyncReturnsAfterPost) {
  EXPECT_EQ(GetRdmaDescriptorGetCompletionMode(false),
            RdmaDescriptorClientCompletionMode::kWaitForCompletion);
  EXPECT_EQ(GetRdmaDescriptorGetCompletionMode(true),
            RdmaDescriptorClientCompletionMode::kReturnAfterPost);
}

TEST(RdmaProtocolTest, DescriptorDoorbellPostDecisionMatchesMayflyBatching) {
  EXPECT_EQ(kRdmaDescriptorSignalBatchSize, 32u);

  const RdmaDescriptorDoorbellPostDecision first =
      GetRdmaDescriptorDoorbellPostDecision(0);
  EXPECT_FALSE(first.poll_before_post);
  EXPECT_TRUE(first.signal_write_with_imm);

  for (std::uint64_t counter = 1; counter < kRdmaDescriptorSignalBatchSize;
       ++counter) {
    const RdmaDescriptorDoorbellPostDecision decision =
        GetRdmaDescriptorDoorbellPostDecision(counter);
    EXPECT_FALSE(decision.poll_before_post) << counter;
    EXPECT_FALSE(decision.signal_write_with_imm) << counter;
  }

  const RdmaDescriptorDoorbellPostDecision next_batch =
      GetRdmaDescriptorDoorbellPostDecision(kRdmaDescriptorSignalBatchSize);
  EXPECT_TRUE(next_batch.poll_before_post);
  EXPECT_TRUE(next_batch.signal_write_with_imm);
}

TEST(RdmaProtocolTest, DescriptorDoorbellPostStateTracksEachLaneSeparately) {
  RdmaDescriptorDoorbellPostState state;

  for (std::uint64_t i = 0; i < kRdmaDescriptorSignalBatchSize; ++i) {
    const int lane = 1 + static_cast<int>(i % 3);
    const RdmaDescriptorDoorbellPostDecision decision = state.Next(lane);
    EXPECT_FALSE(decision.poll_before_post) << i;
  }

  const RdmaDescriptorDoorbellPostDecision lane1_next = state.Next(1);
  EXPECT_FALSE(lane1_next.poll_before_post);
  EXPECT_FALSE(lane1_next.signal_write_with_imm);

  for (std::uint64_t i = 0; i < kRdmaDescriptorSignalBatchSize - 12; ++i) {
    state.Next(1);
  }
  const RdmaDescriptorDoorbellPostDecision lane1_batch = state.Next(1);
  EXPECT_TRUE(lane1_batch.poll_before_post);
  EXPECT_TRUE(lane1_batch.signal_write_with_imm);
}

TEST(RdmaProtocolTest, DescriptorWorkerSelectionKeepsPollerFree) {
  EXPECT_EQ(SelectRdmaDescriptorWorkerThread(1, 0), 0);
  EXPECT_EQ(SelectRdmaDescriptorWorkerThread(2, 0), 1);
  EXPECT_EQ(SelectRdmaDescriptorWorkerThread(4, 0), 1);
  EXPECT_EQ(SelectRdmaDescriptorWorkerThread(4, 1), 2);
  EXPECT_EQ(SelectRdmaDescriptorWorkerThread(4, 2), 3);
  EXPECT_EQ(SelectRdmaDescriptorWorkerThread(4, 3), 1);
}

TEST(RdmaProtocolTest, DescriptorServingThreadsExposeWorkersOnly) {
  EXPECT_EQ(GetRdmaDescriptorServingThreadIDs(1), std::vector<int>({0}));
  EXPECT_EQ(GetRdmaDescriptorServingThreadIDs(4),
            std::vector<int>({1, 2, 3}));
}

TEST(RdmaProtocolTest, RotatesDescriptorServingThreadsBySeed) {
  const std::vector<int> threads = {1, 2, 3, 4};
  EXPECT_EQ(RotateRdmaDescriptorServingThreadIDs(threads, 0), threads);
  EXPECT_EQ(RotateRdmaDescriptorServingThreadIDs(threads, 1),
            std::vector<int>({2, 3, 4, 1}));
  EXPECT_EQ(RotateRdmaDescriptorServingThreadIDs(threads, 5),
            std::vector<int>({2, 3, 4, 1}));
  EXPECT_TRUE(RotateRdmaDescriptorServingThreadIDs({}, 3).empty());
}

TEST(RdmaProtocolTest, DescriptorWorkerSelectionHonorsRequestedLane) {
  int worker_thread = -1;
  std::string error;
  ASSERT_TRUE(TrySelectRdmaDescriptorWorkerThread(
      4, 3, &worker_thread, &error)) << error;
  EXPECT_EQ(worker_thread, 3);

  EXPECT_FALSE(TrySelectRdmaDescriptorWorkerThread(
      4, 0, &worker_thread, &error));
  EXPECT_NE(error.find("lane"), std::string::npos);

  ASSERT_TRUE(TrySelectRdmaDescriptorWorkerThread(
      1, 0, &worker_thread, &error)) << error;
  EXPECT_EQ(worker_thread, 0);
}

TEST(RdmaProtocolTest, EncodesAndDecodesDescriptorWorkerThreads) {
  const std::vector<int> threads = {1, 2, 3};
  const std::string payload = EncodeRdmaDescriptorWorkerThreads(threads);

  std::vector<int> decoded;
  std::string error;
  ASSERT_TRUE(DecodeRdmaDescriptorWorkerThreads(payload, &decoded, &error))
      << error;
  EXPECT_EQ(decoded, threads);
}

TEST(RdmaProtocolTest, RejectsInvalidDescriptorWorkerThreadPayload) {
  const std::string payload(3, '\0');
  std::vector<int> decoded;
  std::string error;
  EXPECT_FALSE(DecodeRdmaDescriptorWorkerThreads(payload, &decoded, &error));
  EXPECT_NE(error.find("size"), std::string::npos);
}

TEST(RdmaProtocolTest, RawVerbsCompletionBatchCursorCachesPolledEntries) {
  EXPECT_EQ(kRawVerbsPollBatchSize, 16);

  RawVerbsCompletionBatchCursor cursor;
  EXPECT_FALSE(cursor.HasCachedCompletion());

  ibv_wc entries[kRawVerbsPollBatchSize]{};
  entries[0].wr_id = 11;
  entries[1].wr_id = 12;
  cursor.Reset(entries, 2);

  ASSERT_TRUE(cursor.HasCachedCompletion());
  EXPECT_EQ(cursor.TakeCachedCompletion()->wr_id, 11u);
  ASSERT_TRUE(cursor.HasCachedCompletion());
  EXPECT_EQ(cursor.TakeCachedCompletion()->wr_id, 12u);
  EXPECT_FALSE(cursor.HasCachedCompletion());
}

TEST(RdmaProtocolTest, DescriptorDsmWriteDecisionMatchesMayflyBatching) {
  const RdmaDescriptorDsmWriteDecision first =
      GetRdmaDescriptorDsmWriteDecision(0);
  EXPECT_FALSE(first.poll_before_write);
  EXPECT_TRUE(first.signal_write);

  for (std::uint64_t counter = 1; counter < kRdmaDescriptorSignalBatchSize;
       ++counter) {
    const RdmaDescriptorDsmWriteDecision decision =
        GetRdmaDescriptorDsmWriteDecision(counter);
    EXPECT_FALSE(decision.poll_before_write) << counter;
    EXPECT_FALSE(decision.signal_write) << counter;
  }

  const RdmaDescriptorDsmWriteDecision next_batch =
      GetRdmaDescriptorDsmWriteDecision(kRdmaDescriptorSignalBatchSize);
  EXPECT_TRUE(next_batch.poll_before_write);
  EXPECT_TRUE(next_batch.signal_write);
}

TEST(RdmaProtocolTest, DescriptorReadyRequiresAllThreadsAndRawConnect) {
  EXPECT_FALSE(CanPublishRdmaDescriptorReady(0, 4, false));
  EXPECT_FALSE(CanPublishRdmaDescriptorReady(4, 4, false));
  EXPECT_FALSE(CanPublishRdmaDescriptorReady(3, 4, true));
  EXPECT_TRUE(CanPublishRdmaDescriptorReady(4, 4, true));
  EXPECT_FALSE(CanPublishRdmaDescriptorReady(1, 0, true));
}

} // namespace petps
