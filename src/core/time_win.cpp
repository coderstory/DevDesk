// src/core/time_win.cpp
// Windows 实现：localtime_s
#include "time.h"

namespace core::time {

void localTime(std::time_t t, std::tm* out) {
    localtime_s(out, &t);
}

} // namespace core::time
