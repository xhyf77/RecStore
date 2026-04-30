#include <gtest/gtest.h>

#include "ps/rdma/raw_verbs_transport.h"

TEST(RawVerbsRegionAllocatorTest, SkipsReservedRegionWhenAllocationWouldOverlap) {
  petps::RawVerbsRegionAllocator allocator(1024);
  allocator.SetReservedRegion({256, 128});

  EXPECT_EQ(allocator.Allocate(64), 0);
  EXPECT_EQ(allocator.Allocate(192), 64);
  EXPECT_EQ(allocator.Allocate(64), 384);
}

TEST(RawVerbsRegionAllocatorTest, ScopeRestoresAllocationOffset) {
  petps::RawVerbsRegionAllocator allocator(1024);
  allocator.SetReservedRegion({256, 128});

  EXPECT_EQ(allocator.current_offset(), 0);
  {
    petps::RawVerbsRegionAllocatorScope scope(&allocator);
    EXPECT_EQ(allocator.Allocate(64), 0);
    EXPECT_EQ(allocator.Allocate(64), 64);
    EXPECT_EQ(allocator.current_offset(), 128);
  }
  EXPECT_EQ(allocator.current_offset(), 0);
  EXPECT_EQ(allocator.Allocate(64), 0);
}

TEST(RawVerbsRegionAllocatorTest, RejectsReservedRegionOutsideLocalMemory) {
  petps::RawVerbsRegionAllocator allocator(512);
  EXPECT_THROW(allocator.SetReservedRegion({448, 128}), std::runtime_error);
}
