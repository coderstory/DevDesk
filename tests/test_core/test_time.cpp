#include "doctest.h"
#include "core/time.h"

#include <ctime>

// 期望值必须取自 std::localtime（libc 的独立实现路径），不能拿另一个 core::time helper
// 当参考：实现整体偏移（如误用 gmtime）时会与被测侧同错自抵消
REGISTER_TEST(core_local_time, "core: localTime fixed timestamp vs std::localtime", {
    const std::time_t ts = 1700000000;

    std::tm ours{};
    core::time::localTime(ts, &ours);

    // 与平台 libc 本地时间参考逐字段一致：误用 gmtime 时非 UTC 时区立刻暴露
    const std::tm* ref = std::localtime(&ts);
    DOCTEST_CHECK(ref != nullptr);
    DOCTEST_CHECK_EQ(ours.tm_year, ref->tm_year);
    DOCTEST_CHECK_EQ(ours.tm_mon, ref->tm_mon);
    DOCTEST_CHECK_EQ(ours.tm_mday, ref->tm_mday);
    DOCTEST_CHECK_EQ(ours.tm_hour, ref->tm_hour);
    DOCTEST_CHECK_EQ(ours.tm_min, ref->tm_min);
    DOCTEST_CHECK_EQ(ours.tm_sec, ref->tm_sec);

    DOCTEST_CHECK(ours.tm_mon >= 0 && ours.tm_mon <= 11);
    DOCTEST_CHECK(ours.tm_mday >= 1 && ours.tm_mday <= 31);
    DOCTEST_CHECK(ours.tm_hour >= 0 && ours.tm_hour <= 23);
    DOCTEST_CHECK(ours.tm_min >= 0 && ours.tm_min <= 59);
    DOCTEST_CHECK(ours.tm_sec >= 0 && ours.tm_sec <= 60);
    DOCTEST_CHECK(ours.tm_wday >= 0 && ours.tm_wday <= 6);
    DOCTEST_CHECK(ours.tm_yday >= 0 && ours.tm_yday <= 365);

    std::tm roundtrip = ours;
    DOCTEST_CHECK_EQ(std::mktime(&roundtrip), ts);

    return true;
})

// daysFromCivil 是全项目唯一的日历日序号实现：home 相对时间分桶、todo 逾期天数都调它，
// 不要在别处再内联一份或改用「两个本地零点相减」
REGISTER_TEST(core_time_days_from_civil,
              "core: time daysFromCivil 日历日序号（1970→0 / 闰日 / 相邻日差）", {
    DOCTEST_CHECK_EQ(core::time::daysFromCivil(1970, 1, 1),
                     static_cast<int64_t>(0));

    DOCTEST_CHECK_EQ(core::time::daysFromCivil(2024, 2, 29),
                     static_cast<int64_t>(19782));

    DOCTEST_CHECK_EQ(core::time::daysFromCivil(2024, 2, 29) -
                         core::time::daysFromCivil(2024, 2, 28),
                     static_cast<int64_t>(1));
    DOCTEST_CHECK_EQ(core::time::daysFromCivil(2024, 3, 1) -
                         core::time::daysFromCivil(2024, 2, 29),
                     static_cast<int64_t>(1));

    DOCTEST_CHECK_EQ(core::time::daysFromCivil(2025, 1, 1) -
                         core::time::daysFromCivil(2024, 12, 31),
                     static_cast<int64_t>(1));

    // localDayNumber 的参考值取自 std::localtime 的日期字段，不可用 daysFromCivil 自身作参考，否则恒等
    const std::time_t ts = 1700000000;
    const std::tm* ref = std::localtime(&ts);
    DOCTEST_REQUIRE(ref != nullptr);
    DOCTEST_CHECK_EQ(core::time::localDayNumber(ts),
                     core::time::daysFromCivil(
                         ref->tm_year + 1900,
                         static_cast<unsigned>(ref->tm_mon) + 1,
                         static_cast<unsigned>(ref->tm_mday)));

    return true;
})

// 防：localMidnightUnix 用「t − 墙钟已流逝秒数」减法实现——DST 切换日墙钟 ≠ 24h，
//   秋回日（25h）返回今日 01:00（00:00-01:00 完成的卡被冷启动误归档）、春进日
//   （23h）返回昨日 23:00（完成卡多滞留 done）。三条不变式在任何本地时区都必须
//   成立：① 零点 ≤ t；② 与 t 同一本地日历日；③ 映射回本地时刻恰为 00:00:00。
//   探测时刻取美东真实切换日（秋回 2024-11-03 / 春进 2024-03-10）——在 DST 时区
//   旧实现会在 ② 或 ③ 上红；在无 DST 时区（如 UTC CI）两者皆绿，此用例是防回归
//   契约钉。
REGISTER_TEST(core_time_local_midnight_dst_invariant,
              "core.time: localMidnightUnix 返回当日真实零点（DST 切换日不漂移）", {
    const std::time_t probes[] = {
        1730615400,  // 2024-11-03 06:30 UTC = 美东秋回日 01:30 EST（25h 日第二遍 01 时）
        1710127800,  // 2024-03-11 03:30 UTC = 美东春进日 23:30 EDT（23h 日末尾）
    };
    for (const std::time_t t : probes) {
        const std::time_t midnight = core::time::localMidnightUnix(t);
        DOCTEST_CHECK(midnight <= t);   // ① 不晚于 t

        std::tm m{};
        core::time::localTime(midnight, &m);
        DOCTEST_CHECK_EQ(core::time::localDayNumber(midnight),
                         core::time::localDayNumber(t));  // ② 同一本地日历日
        DOCTEST_CHECK(m.tm_hour == 0 && m.tm_min == 0 &&
                      m.tm_sec == 0);                      // ③ 恰为当日 00:00:00
    }

    // 当下时刻同三条不变式（普通日兜底）
    const std::time_t now = std::time(nullptr);
    const std::time_t midnightNow = core::time::localMidnightUnix(now);
    DOCTEST_CHECK(midnightNow <= now);
    DOCTEST_CHECK_EQ(core::time::localDayNumber(midnightNow),
                     core::time::localDayNumber(now));
    return true;
})