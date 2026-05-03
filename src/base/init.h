#pragma once

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <mutex>

namespace base {

inline void
InitLoggingAndFlags(int* argc, char*** argv, bool remove_flags = true) {
  static std::once_flag once;
  std::call_once(once, [argc, argv, remove_flags]() {
    google::InitGoogleLogging(
        (argc != nullptr && argv != nullptr && *argc > 0)
            ? (*argv)[0]
            : "recstore");
    if (argc != nullptr && argv != nullptr) {
      gflags::ParseCommandLineFlags(argc, argv, remove_flags);
    }
  });
}

class Init {
public:
  Init(int* argc, char*** argv, bool remove_flags = true) {
    InitLoggingAndFlags(argc, argv, remove_flags);
  }
};

inline void Init(int* argc, char*** argv, bool remove_flags = true) {
  InitLoggingAndFlags(argc, argv, remove_flags);
}

} // namespace base
