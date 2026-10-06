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
