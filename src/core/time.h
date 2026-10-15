// 平台无关的本地时间转换接口（Windows = localtime_s，POSIX = localtime_r）
#pragma once

#include <cstdint>
#include <ctime>

namespace core::time {

// 当前 UNIX 时刻；统一全仓「现在」口径的唯一入口
inline std::time_t nowUnix() { return std::time(nullptr); }

// 线程安全 localtime：time_t → 本地 std::tm（写入 *out），替代散落的平台分支
void localTime(std::time_t t, std::tm* out);

// 线程安全 UTC 转换：time_t → UTC std::tm（写入 *out）。
// 用于备份文件名时间戳——本地时间在 DST 回拨时非单调，滚动修剪按字典序会误删新备份
void utcTime(std::time_t t, std::tm* out);

// 公历日期 → 日历日序号（1970-01-01 → 0，更早为负），纯算术、与时区/DST 无关。
// 用于「两个本地时刻相差几天」：免 tm_yday 跨年/闰年坑，也免本地零点相减在
// DST 切换日 23h/25h 上产生 ±1 天偏差。
inline int64_t daysFromCivil(int y, unsigned m, unsigned d) {
    const long long yy = static_cast<long long>(y) - (m <= 2 ? 1 : 0);
    const long long era = (yy >= 0 ? yy : yy - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(yy - era * 400);
    const unsigned doy =
        (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

// 本地时区的日历日序号；两个本地时刻相差几天一律用其返回值相减
inline int64_t localDayNumber(std::time_t t) {
    std::tm local{};
    localTime(t, &local);
    return daysFromCivil(local.tm_year + 1900,
                         static_cast<unsigned>(local.tm_mon) + 1,
                         static_cast<unsigned>(local.tm_mday));
}

} // namespace core::time
