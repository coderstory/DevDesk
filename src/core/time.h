// src/core/time.h
// 平台无关的本地时间转换接口
// Windows 实现见 time_win.cpp（localtime_s），POSIX（macOS/Linux）见 time_posix.cpp（localtime_r）
#pragma once

#include <ctime>

namespace core::time {

// 线程安全版 localtime：把 time_t 转换为本地时间的 std::tm（写入 *out）
// 替代业务代码里散布的平台分支（localtime_s / localtime_r / localtime）
void localTime(std::time_t t, std::tm* out);

} // namespace core::time
