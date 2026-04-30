#include "ps/local_shm/local_shm_futex.h"

#include <cerrno>
#include <ctime>

#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace recstore {

namespace {

timespec ToTimespec(std::chrono::milliseconds timeout) {
  timespec ts{};
  ts.tv_sec  = static_cast<time_t>(timeout.count() / 1000);
  ts.tv_nsec = static_cast<long>((timeout.count() % 1000) * 1000000LL);
  return ts;
}

} // namespace

int FutexWait(std::atomic<uint32_t>* addr,
              uint32_t expected,
              const struct timespec* timeout) {
  return static_cast<int>(syscall(
      SYS_futex,
      reinterpret_cast<uint32_t*>(addr),
      FUTEX_WAIT,
      expected,
      timeout,
      nullptr,
      0));
}

int FutexWake(std::atomic<uint32_t>* addr, int count) {
  return static_cast<int>(syscall(
      SYS_futex,
      reinterpret_cast<uint32_t*>(addr),
      FUTEX_WAKE,
      count,
      nullptr,
      nullptr,
      0));
}

bool FutexWaitUntilValueChange(std::atomic<uint32_t>* addr,
                               uint32_t expected,
                               std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (addr->load(std::memory_order_acquire) == expected) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return false;
    }

    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    const timespec ts = ToTimespec(remaining);
    const int rc      = FutexWait(addr, expected, &ts);
    if (rc == 0) {
      continue;
    }
    if (errno == EAGAIN || errno == EINTR) {
      continue;
    }
    if (errno == ETIMEDOUT) {
      return addr->load(std::memory_order_acquire) != expected;
    }
    return addr->load(std::memory_order_acquire) != expected;
  }
  return true;
}

} // namespace recstore
