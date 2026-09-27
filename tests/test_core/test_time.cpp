#include "doctest.h"
#include "core/time.h"

#include <ctime>

// core::time::localTime 直接用例。此前 localTime 仅在其他用例里被当作"两侧共用"
// 的辅助函数（构造期望值），若实现整体偏移（如误用 gmtime）会与被测侧同错自抵消。
// 这里用固定 Unix 时间戳直接对照 std::localtime（libc 的另一实现路径）逐字段校验，
// 并做合法性 + mktime 往返检查，构成独立回归防护。
REGISTER_TEST(core_local_time, "core: localTime fixed timestamp vs std::localtime", {
    const std::time_t ts = 1700000000; // 2023-11-14T22:13:20Z（UTC）

    std::tm ours{};
    core::time::localTime(ts, &ours);

    // 1) 与平台 libc 本地时间参考逐字段一致（误用 gmtime 时非 UTC 时区立刻暴露）
    const std::tm* ref = std::localtime(&ts);
    DOCTEST_CHECK(ref != nullptr);
    DOCTEST_CHECK_EQ(ours.tm_year, ref->tm_year);
    DOCTEST_CHECK_EQ(ours.tm_mon, ref->tm_mon);
    DOCTEST_CHECK_EQ(ours.tm_mday, ref->tm_mday);
    DOCTEST_CHECK_EQ(ours.tm_hour, ref->tm_hour);
    DOCTEST_CHECK_EQ(ours.tm_min, ref->tm_min);
    DOCTEST_CHECK_EQ(ours.tm_sec, ref->tm_sec);

    // 2) 字段合法性
    DOCTEST_CHECK(ours.tm_mon >= 0 && ours.tm_mon <= 11);
    DOCTEST_CHECK(ours.tm_mday >= 1 && ours.tm_mday <= 31);
    DOCTEST_CHECK(ours.tm_hour >= 0 && ours.tm_hour <= 23);
    DOCTEST_CHECK(ours.tm_min >= 0 && ours.tm_min <= 59);
    DOCTEST_CHECK(ours.tm_sec >= 0 && ours.tm_sec <= 60);
    DOCTEST_CHECK(ours.tm_wday >= 0 && ours.tm_wday <= 6);
    DOCTEST_CHECK(ours.tm_yday >= 0 && ours.tm_yday <= 365);

    // 3) 往返：把本地 tm 交回 mktime（按本地时区解释）应还原出原时间戳
    std::tm roundtrip = ours;
    DOCTEST_CHECK_EQ(std::mktime(&roundtrip), ts);

    return true;
})

// THS-16（波 1 L6）：本地日历日序号。home 相对时间分桶与 todo 逾期天数此前
// 各自内联 days_from_civil / 用两个本地零点相减；统一为 core::time 提供，
// 本用例锁住其数值契约（纪元原点 / 闰日 / 相邻日差 / 跨年）。
REGISTER_TEST(core_time_days_from_civil,
              "core: time daysFromCivil 日历日序号（1970→0 / 闰日 / 相邻日差）", {
    // 1) 纪元原点：1970-01-01 → 0
    DOCTEST_CHECK_EQ(core::time::daysFromCivil(1970, 1, 1),
                     static_cast<int64_t>(0));

    // 2) 闰日：1970-01-01 → 2024-02-29 共 19782 天
    DOCTEST_CHECK_EQ(core::time::daysFromCivil(2024, 2, 29),
                     static_cast<int64_t>(19782));

    // 3) 相邻日差 1（含闰日次日）
    DOCTEST_CHECK_EQ(core::time::daysFromCivil(2024, 2, 29) -
                         core::time::daysFromCivil(2024, 2, 28),
                     static_cast<int64_t>(1));
    DOCTEST_CHECK_EQ(core::time::daysFromCivil(2024, 3, 1) -
                         core::time::daysFromCivil(2024, 2, 29),
                     static_cast<int64_t>(1));

    // 4) 跨年：2024-12-31 → 2025-01-01 差 1
    DOCTEST_CHECK_EQ(core::time::daysFromCivil(2025, 1, 1) -
                         core::time::daysFromCivil(2024, 12, 31),
                     static_cast<int64_t>(1));

    // 5) localDayNumber：与 libc 本地时间的日期字段一致（时区无关写法，
    //    参考值取自 std::localtime 另一实现路径）
    const std::time_t ts = 1700000000; // 2023-11-14T22:13:20Z（UTC）
    const std::tm* ref = std::localtime(&ts);
    DOCTEST_REQUIRE(ref != nullptr);
    DOCTEST_CHECK_EQ(core::time::localDayNumber(ts),
                     core::time::daysFromCivil(
                         ref->tm_year + 1900,
                         static_cast<unsigned>(ref->tm_mon) + 1,
                         static_cast<unsigned>(ref->tm_mday)));

    return true;
})
