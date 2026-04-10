#pragma once

#include "fmt/core.h"
#include "base/log.h"
#include "base/mmap.h"
#include "base/random.h"
#include "base/string.h"

#if defined(__cpp_lib_hardware_interference_size)
#  include <new>
#endif

namespace base {

#if defined(__cpp_lib_hardware_interference_size)
inline constexpr std::size_t kHardwareDestructiveInterferenceSize =
    std::hardware_destructive_interference_size;
#else
inline constexpr std::size_t kHardwareDestructiveInterferenceSize = 64;
#endif

} // namespace base
