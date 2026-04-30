#pragma once

#include <atomic>
#include <chrono>

#include <gflags/gflags.h>
#include <glog/logging.h>

#define BASE_LOG_CONCAT_INNER(x, y) x##y
#define BASE_LOG_CONCAT(x, y) BASE_LOG_CONCAT_INNER(x, y)

namespace base::detail {

class LogEveryMsState {
public:
  bool ShouldLog(int64_t interval_ms) {
    const int64_t now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    int64_t last_ms = last_ms_.load(std::memory_order_relaxed);
    while (now_ms - last_ms >= interval_ms) {
      if (last_ms_.compare_exchange_weak(
              last_ms, now_ms, std::memory_order_relaxed)) {
        return true;
      }
    }
    return false;
  }

private:
  std::atomic<int64_t> last_ms_{0};
};

} // namespace base::detail

#define FB_LOG_EVERY_MS(level, interval_ms)                                    \
  static ::base::detail::LogEveryMsState BASE_LOG_CONCAT(                      \
      _fb_log_every_ms_state_, __LINE__);                                      \
  for (bool BASE_LOG_CONCAT(_fb_log_every_ms_once_, __LINE__) =                \
           BASE_LOG_CONCAT(_fb_log_every_ms_state_, __LINE__)                  \
               .ShouldLog(interval_ms);                                        \
       BASE_LOG_CONCAT(_fb_log_every_ms_once_, __LINE__);                      \
       BASE_LOG_CONCAT(_fb_log_every_ms_once_, __LINE__) = false)              \
  LOG(level)
