#pragma once

#include <folly/executors/CPUThreadPoolExecutor.h>

#include <cstddef>
#include <memory>

namespace base {

class CPUThreadPoolExecutor {
public:
  using Options  = folly::CPUThreadPoolExecutor::Options;
  using Blocking = folly::CPUThreadPoolExecutor::Options::Blocking;

  explicit CPUThreadPoolExecutor(size_t num_threads,
                                 Options options = Options())
      : impl_(std::make_unique<folly::CPUThreadPoolExecutor>(
            num_threads, std::move(options))) {}
  ~CPUThreadPoolExecutor() = default;

  template <typename Func>
  void add(Func&& func) {
    impl_->add(std::forward<Func>(func));
  }

  void join() { impl_->join(); }

private:
  std::unique_ptr<folly::CPUThreadPoolExecutor> impl_;
};

} // namespace base
