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
