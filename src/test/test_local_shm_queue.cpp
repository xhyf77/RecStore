#include <gtest/gtest.h>

#include "ps/local_shm/local_shm_queue.h"

namespace recstore {
namespace {

TEST(LocalShmQueueTest, RoundTripAndWrapAround) {
  constexpr uint32_t kCapacity = 4;
  LocalShmQueueHeader header{};
  LocalShmQueueCell cells[kCapacity]{};
  LocalShmQueueInitialize(&header, cells, kCapacity);

  for (uint32_t i = 0; i < kCapacity; ++i) {
    EXPECT_TRUE(LocalShmQueueEnqueue(&header, cells, 100 + i));
  }
  EXPECT_FALSE(LocalShmQueueEnqueue(&header, cells, 999));

  for (uint32_t i = 0; i < kCapacity; ++i) {
    uint32_t value = 0;
    ASSERT_TRUE(LocalShmQueueDequeue(&header, cells, &value));
    EXPECT_EQ(value, 100 + i);
  }
  uint32_t value = 0;
  EXPECT_FALSE(LocalShmQueueDequeue(&header, cells, &value));

  for (uint32_t i = 0; i < kCapacity; ++i) {
    EXPECT_TRUE(LocalShmQueueEnqueue(&header, cells, 200 + i));
  }
  for (uint32_t i = 0; i < kCapacity; ++i) {
    ASSERT_TRUE(LocalShmQueueDequeue(&header, cells, &value));
    EXPECT_EQ(value, 200 + i);
  }
}

} // namespace
} // namespace recstore
