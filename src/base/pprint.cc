#include "pprint.h"
#include "base/string.h"

namespace base {
std::string PrettyPrintBytes(double bytes) {
  return base::PrettyPrintBytesString(bytes);
}
} // namespace base
