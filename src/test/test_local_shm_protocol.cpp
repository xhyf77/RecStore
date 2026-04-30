#include <gtest/gtest.h>

#include "ps/local_shm/local_shm_layout.h"

namespace recstore {
namespace {

TEST(LocalShmProtocolTest, ControlBlockAndSlotHeaderAreDefined) {
  EXPECT_GT(sizeof(LocalShmControlBlock), 0U);
  EXPECT_GT(sizeof(LocalShmQueueHeader), 0U);
  EXPECT_GT(sizeof(LocalShmQueueCell), 0U);
  EXPECT_GT(sizeof(LocalShmSlotHeader), 0U);
  EXPECT_EQ(alignof(LocalShmControlBlock), 64U);
  EXPECT_EQ(alignof(LocalShmQueueHeader), 64U);
  EXPECT_EQ(alignof(LocalShmSlotHeader), 64U);
}

TEST(LocalShmProtocolTest, LayoutOffsetsAreMonotonic) {
  constexpr uint32_t kSlotCount       = 8;
  constexpr uint32_t kSlotBytes       = 4096;
  constexpr uint32_t kReadyQueueCount = 4;

  EXPECT_EQ(ControlBlockOffset(), 0U);
  EXPECT_GE(QueueHeadersOffset(), sizeof(LocalShmControlBlock));
  EXPECT_GT(QueueCellsOffset(kReadyQueueCount), QueueHeadersOffset());
  EXPECT_GT(SlotHeadersOffset(kSlotCount, kReadyQueueCount),
            QueueCellsOffset(kReadyQueueCount));
  EXPECT_GT(SlotPayloadsOffset(kSlotCount, kReadyQueueCount),
            SlotHeadersOffset(kSlotCount, kReadyQueueCount));
  EXPECT_GT(TotalRegionBytes(kSlotCount, kSlotBytes, kReadyQueueCount),
            SlotPayloadsOffset(kSlotCount, kReadyQueueCount));
  EXPECT_EQ(SlotPayloadOffset(kSlotCount, kSlotBytes, 0, kReadyQueueCount),
            SlotPayloadsOffset(kSlotCount, kReadyQueueCount));
  EXPECT_GT(SlotPayloadOffset(kSlotCount, kSlotBytes, 1),
            SlotPayloadOffset(kSlotCount, kSlotBytes, 0));
  EXPECT_LT(ReadyQueueHeaderOffset(0), ReadyQueueHeaderOffset(1));
  EXPECT_LT(ReadyQueueCellsOffset(kSlotCount, 0),
            ReadyQueueCellsOffset(kSlotCount, 1));
}

} // namespace
} // namespace recstore
