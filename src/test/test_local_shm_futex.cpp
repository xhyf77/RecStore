#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "ps/local_shm/local_shm_futex.h"

namespace recstore {
namespace {

TEST(LocalShmFutexTest, WaitUntilValueChangeWakesAfterUpdate) {
  std::atomic<uint32_t> word{0};
  bool woke = false;

  std::thread waiter([&]() {
    woke = FutexWaitUntilValueChange(&word, 0, std::chrono::milliseconds(200));
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  word.store(1, std::memory_order_release);
  EXPECT_GE(FutexWakeAll(&word), 0);

  waiter.join();
  EXPECT_TRUE(woke);
}

TEST(LocalShmFutexTest, WaitUntilValueChangeTimesOut) {
  std::atomic<uint32_t> word{7};
  const auto start = std::chrono::steady_clock::now();
  const bool woke =
      FutexWaitUntilValueChange(&word, 7, std::chrono::milliseconds(20));
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);

  EXPECT_FALSE(woke);
  EXPECT_GE(elapsed.count(), 15);
}

} // namespace
} // namespace recstore
