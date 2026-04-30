#pragma once

#include <cstring>
#include <liburing.h>
#include <string>

namespace test_utils {

inline bool CanUseIoUring(std::string* reason) {
  io_uring ring{};
  const int ret = io_uring_queue_init(2, &ring, 0);
  if (ret == 0) {
    io_uring_queue_exit(&ring);
    return true;
  }
  if (reason != nullptr) {
    const int err = -ret;
    *reason       = "io_uring unavailable: " + std::string(std::strerror(err)) +
              " (errno=" + std::to_string(err) + ")";
  }
  return false;
}

} // namespace test_utils
