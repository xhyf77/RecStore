#include "gtest/gtest.h"
#include "test_io_uring_helper.h"

#include <string>

TEST(IoUringTestHelperTest, ReturnsReasonStringWhenUnavailable) {
  std::string reason;
  const bool available = test_utils::CanUseIoUring(&reason);

  if (available) {
    EXPECT_TRUE(reason.empty());
  } else {
    EXPECT_FALSE(reason.empty());
  }
}
