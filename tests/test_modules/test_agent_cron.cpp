// tests/test_modules/test_agent_cron.cpp
// cron 纯函数用例（agent M2 Task 3）：spec §5.1 R4 语法（5 字段「分 时 日 月 周」，
// 支持 * , - /，周 0-6=0 周日，不支持 Quartz 扩展与英文名称）表驱动覆盖
// 语法域 × 匹配域 × 拒绝域。纯函数零 IO 零时钟，无 fixture。
#include "doctest.h"
#include "module_agent/cron.h"
#include <ctime>

namespace {
std::tm tmOf(int min, int hour, int mday, int mon1to12, int wday) {
    std::tm t{};
    t.tm_min = min; t.tm_hour = hour; t.tm_mday = mday;
    t.tm_mon = mon1to12 - 1; t.tm_wday = wday;   // 0=周日
    return t;
}
bool fires(const std::string& expr, std::tm t) {
    auto c = module::agent::parseCron(expr);
    return c.isOk() && module::agent::cronMatches(c.value(), t);
}
} // namespace

REGISTER_TEST(cron_parse_and_match_table,
              "cron: parse/matches 表驱动（语法域 × 匹配域 × 拒绝域）", {
    // ---- 命中 ----
    DOCTEST_CHECK(fires("*/5 * * * *",  tmOf(0, 9, 5, 10, 3)));
    DOCTEST_CHECK(fires("*/5 * * * *",  tmOf(55, 23, 28, 2, 5)));
    DOCTEST_CHECK(!fires("*/5 * * * *", tmOf(3, 9, 5, 10, 3)));
    DOCTEST_CHECK(fires("0 9 * * 1-5",  tmOf(0, 9, 6, 10, 1)));   // 周一 09:00
    DOCTEST_CHECK(!fires("0 9 * * 1-5", tmOf(0, 9, 6, 10, 6)));   // 周六不中
    DOCTEST_CHECK(fires("30 14 1 * *",  tmOf(30, 14, 1, 7, 0)));  // 每月 1 日
    DOCTEST_CHECK(fires("5,15,45 * * * *", tmOf(15, 0, 2, 1, 2)));
    DOCTEST_CHECK(fires("10-30/10 * * * *", tmOf(20, 0, 2, 1, 2)));
    DOCTEST_CHECK(!fires("10-30/10 * * * *", tmOf(25, 0, 2, 1, 2))); // 步长跳过
    DOCTEST_CHECK(fires("0 12 * * 0", tmOf(0, 12, 7, 10, 0)));    // 周日=0
    // 日/周双受限 → vixie「或」（1 号或周日）
    DOCTEST_CHECK(fires("0 0 1 * 0", tmOf(0, 0, 15, 10, 0)));     // 周日非 1 号
    DOCTEST_CHECK(fires("0 0 1 * 0", tmOf(0, 0, 1, 10, 3)));      // 1 号非周日
    DOCTEST_CHECK(!fires("0 0 1 * 0", tmOf(0, 0, 15, 10, 3)));    // 两者皆非
    // 仅日受限 / 仅周受限 → 交集语义
    DOCTEST_CHECK(!fires("0 0 1 * *", tmOf(0, 0, 15, 10, 0)));
    DOCTEST_CHECK(!fires("0 0 * * 0", tmOf(0, 0, 15, 10, 3)));
    // ---- 拒绝 ----
    for (const char* bad : {"61 * * * *", "* 25 * * *", "* * 32 * *", "* * * 13 *",
                            "* * * * 7", "0 9 * *", "0 9 * * * *", "*/0 * * * *",
                            "9-5 * * * *", "MON * * * *", "L * * * *", "0 9,w * * *",
                            "", "  ", "* * * * *#2"}) {
        DOCTEST_CHECK_MESSAGE(module::agent::parseCron(bad).isFail(),
                              "应拒绝: " + std::string(bad));
    }
    return true;
})
