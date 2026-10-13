// POSIX 实现（macOS / Linux 共用）：localtime_r
#include "time.h"

namespace core::time {

void localTime(std::time_t t, std::tm* out) {
    localtime_r(&t, out);
}

} // namespace core::time
