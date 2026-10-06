// tests/test_modules/test_agent_cron.cpp
// cron 纯函数单测：5 字段「分 时 日 月 周」的解析与匹配，表驱动覆盖语法域 × 匹配域 × 拒绝域。
// 跨文件前提：
//   - 语法域只认 * , - /，周取值 0-6 且 0=周日；英文月份名与 Quartz 扩展（L/W/#）一律拒绝。
//   - 日与周同时受限是「或」而非「与」（1 号或周日都命中）；只限其一时按单字段判定。
// 纯函数零 IO 零时钟，无 fixture。
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
    DOCTEST_CHECK(fires("*/5 * * * *",  tmOf(0, 9, 5, 10, 3)));
    DOCTEST_CHECK(fires("*/5 * * * *",  tmOf(55, 23, 28, 2, 5)));
    DOCTEST_CHECK(!fires("*/5 * * * *", tmOf(3, 9, 5, 10, 3)));
    DOCTEST_CHECK(fires("0 9 * * 1-5",  tmOf(0, 9, 6, 10, 1)));
    DOCTEST_CHECK(!fires("0 9 * * 1-5", tmOf(0, 9, 6, 10, 6)));
    DOCTEST_CHECK(fires("30 14 1 * *",  tmOf(30, 14, 1, 7, 0)));
    DOCTEST_CHECK(fires("5,15,45 * * * *", tmOf(15, 0, 2, 1, 2)));
    DOCTEST_CHECK(fires("10-30/10 * * * *", tmOf(20, 0, 2, 1, 2)));
    DOCTEST_CHECK(!fires("10-30/10 * * * *", tmOf(25, 0, 2, 1, 2)));  // 10-30/10 只展开 10/20/30
    DOCTEST_CHECK(fires("0 12 * * 0", tmOf(0, 12, 7, 10, 0)));
    DOCTEST_CHECK(fires("0 0 1 * 0", tmOf(0, 0, 15, 10, 0)));     // 周日非 1 号仍命中
    DOCTEST_CHECK(fires("0 0 1 * 0", tmOf(0, 0, 1, 10, 3)));      // 1 号非周日仍命中
    DOCTEST_CHECK(!fires("0 0 1 * 0", tmOf(0, 0, 15, 10, 3)));    // 两者皆非才不中
    DOCTEST_CHECK(!fires("0 0 1 * *", tmOf(0, 0, 15, 10, 0)));
    DOCTEST_CHECK(!fires("0 0 * * 0", tmOf(0, 0, 15, 10, 3)));
    for (const char* bad : {"61 * * * *", "* 25 * * *", "* * 32 * *", "* * * 13 *",
                            "* * * * 7", "0 9 * *", "0 9 * * * *", "*/0 * * * *",
                            "9-5 * * * *", "MON * * * *", "L * * * *", "0 9,w * * *",
                            "", "  ", "* * * * *#2"}) {
        DOCTEST_CHECK_MESSAGE(module::agent::parseCron(bad).isFail(),
                              "应拒绝: " + std::string(bad));
    }
    return true;
})

// parseCron 拒绝 step > 字段跨度：这是防越界写而非风格约束——步进 v += step 在
// 起点(1) + INT_MAX 处有符号回绕成负下标。步长 > 跨度只可能命中起点一次，等价写法是单值。
REGISTER_TEST(cron_rejects_oversized_step,
              "cron: 巨大 step（INT_MAX / 超跨度）→ 拒绝且不崩", {
    for (const char* bad : {"5/2147483647 * * * *", "*/2147483647 * * * *",
                            "0 0 */2147483647 * *", "*/999999999 * * * *",
                            "* * 1-31/32 * *", "* * * * 0-6/8"}) {
        DOCTEST_CHECK_MESSAGE(module::agent::parseCron(bad).isFail(),
                              "应拒绝: " + std::string(bad));
    }
    // 上界不可收窄到「步长必须整除跨度」：*/60 = 每小时第 0 分钟，与 */5 同源合法
    DOCTEST_CHECK(fires("*/60 * * * *", tmOf(0, 9, 5, 10, 3)));
    DOCTEST_CHECK(!fires("*/60 * * * *", tmOf(1, 9, 5, 10, 3)));
    return true;
})
