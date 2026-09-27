// src/core/time.h
// 平台无关的本地时间转换接口
// Windows 实现见 time_win.cpp（localtime_s），POSIX（macOS/Linux）见 time_posix.cpp（localtime_r）
#pragma once

#include <cstdint>
#include <ctime>

namespace core::time {

// 线程安全版 localtime：把 time_t 转换为本地时间的 std::tm（写入 *out）
// 替代业务代码里散布的平台分支（localtime_s / localtime_r / localtime）
void localTime(std::time_t t, std::tm* out);

// 公历日期 → 日历日序号（Howard Hinnant 的 days_from_civil；1970-01-01 → 0，
// 更早为负）。纯算术，与时区 / DST 无关；供"两个本地时刻相差几天"类判断使用
// （免 tm_yday 的跨年 / 闰年坑，也免"两个本地零点相减"在 DST 切换日 23h / 25h
// 的一天上产生 ±1 天偏差）。
inline int64_t daysFromCivil(int y, unsigned m, unsigned d) {
    const long long yy = static_cast<long long>(y) - (m <= 2 ? 1 : 0);
    const long long era = (yy >= 0 ? yy : yy - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(yy - era * 400);
    const unsigned doy =
        (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

// 本地时区的日历日序号：把 t 转成本地 tm 后取 daysFromCivil。
// 两个本地时刻"相差几天"一律用其返回值相减（跨月 / 跨年 / 闰年安全）。
inline int64_t localDayNumber(std::time_t t) {
    std::tm local{};
    localTime(t, &local);
    return daysFromCivil(local.tm_year + 1900,
                         static_cast<unsigned>(local.tm_mon) + 1,
                         static_cast<unsigned>(local.tm_mday));
}

} // namespace core::time
