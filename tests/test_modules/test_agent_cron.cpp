// tests/test_modules/test_agent_cron.cpp
// cron 纯函数单测：5 字段「分 时 日 月 周」的解析与匹配，表驱动覆盖语法域 × 匹配域 × 拒绝域。
// 跨文件前提：
//   - 语法域只认 * , - /，周取值 0-6 且 0=周日；英文月份名与 Quartz 扩展（L/W/#）一律拒绝。
//   - 日与周同时受限是「或」而非「与」（1 号或周日都命中）；只限其一时按单字段判定。
// 纯函数零 IO 零时钟，无 fixture。
#include "doctest.h"
#include "module_agent/cron.h"
#include <ctime>

#include "core/time.h"

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

namespace {
// 本地墙钟 → time_t（mktime 是 localTime 的逆，测试期望值与被测函数走同一时区口径）
std::time_t localSecOf(int year, int mon1to12, int mday, int hour, int min) {
    std::tm t{};
    t.tm_year = year - 1900;
    t.tm_mon = mon1to12 - 1;
    t.tm_mday = mday;
    t.tm_hour = hour;
    t.tm_min = min;
    t.tm_isdst = -1;   // 让 mktime 按当时夏令时规则归一
    return std::mktime(&t);
}
// 期望的本地 tm（wday 由 mktime 补齐，与生产 localTime 填充路径同源）
std::tm expectLocalOf(int year, int mon1to12, int mday, int hour, int min) {
    std::tm t{};
    t.tm_year = year - 1900;
    t.tm_mon = mon1to12 - 1;
    t.tm_mday = mday;
    t.tm_hour = hour;
    t.tm_min = min;
    t.tm_isdst = -1;
    std::mktime(&t);
    return t;
}
bool sameMinute(const std::tm& a, const std::tm& b) {
    return a.tm_year == b.tm_year && a.tm_mon == b.tm_mon &&
           a.tm_mday == b.tm_mday && a.tm_hour == b.tm_hour &&
           a.tm_min == b.tm_min;
}
} // namespace

// nextCronMatch：fromSec 起下一个命中分钟（本地墙钟），硬上限 366*1440 步、
// 到顶 fail(ValidationError)。语义与 Scheduler 分钟翻转触发对齐：fromSec 所在
// 分钟不参与（已翻过），从下一分钟边界起探。
REGISTER_TEST(cron_next_match_table,
              "cron: nextCronMatch 表驱动（同日命中 / 跨日 / 星期语义跨周末 / 无匹配到顶）", {
    const char* exprs[] = {"*/15 * * * *", "0 9 * * *", "0 9 * * 1-5",
                           "0 9 * * 1-5", "0 9 31 2 *"};
    const std::time_t froms[] = {
        localSecOf(2026, 10, 9, 10, 7), localSecOf(2026, 10, 9, 12, 0),
        localSecOf(2026, 10, 9, 12, 34), localSecOf(2026, 10, 9, 8, 0),
        localSecOf(2026, 10, 9, 0, 0)};
    static_assert(sizeof(exprs) == sizeof(froms));

    // ① 同日基本命中：10:07 注入 */15 → 10:15（不是 10:00，fromSec 所在分钟已翻过）
    {
        auto parsed = module::agent::parseCron(exprs[0]);
        DOCTEST_REQUIRE(parsed.isOk());
        auto r = module::agent::nextCronMatch(parsed.value(), froms[0]);
        DOCTEST_REQUIRE(r.isOk());
        DOCTEST_CHECK(sameMinute(r.value(), expectLocalOf(2026, 10, 9, 10, 15)));
    }
    // ② 跨日：当天 09:00 已过 → 次日 09:00
    {
        auto parsed = module::agent::parseCron(exprs[1]);
        DOCTEST_REQUIRE(parsed.isOk());
        auto r = module::agent::nextCronMatch(parsed.value(), froms[1]);
        DOCTEST_REQUIRE(r.isOk());
        DOCTEST_CHECK(sameMinute(r.value(), expectLocalOf(2026, 10, 10, 9, 0)));
    }
    // ③ 星期语义：2026-10-09 是周五、周五 09:00 已过 → 下周一 2026-10-12 09:00
    {
        std::tm friday = expectLocalOf(2026, 10, 9, 12, 34);
        DOCTEST_CHECK_EQ(friday.tm_wday, 5);
        std::tm expect = expectLocalOf(2026, 10, 12, 9, 0);
        DOCTEST_CHECK_EQ(expect.tm_wday, 1);
        auto parsed = module::agent::parseCron(exprs[2]);
        DOCTEST_REQUIRE(parsed.isOk());
        auto r = module::agent::nextCronMatch(parsed.value(), froms[2]);
        DOCTEST_REQUIRE(r.isOk());
        DOCTEST_CHECK(sameMinute(r.value(), expect));
    }
    // ③b 同周五 08:00（09:00 未过）→ 当天周五 09:00
    {
        auto parsed = module::agent::parseCron(exprs[3]);
        DOCTEST_REQUIRE(parsed.isOk());
        auto r = module::agent::nextCronMatch(parsed.value(), froms[3]);
        DOCTEST_REQUIRE(r.isOk());
        DOCTEST_CHECK(sameMinute(r.value(), expectLocalOf(2026, 10, 9, 9, 0)));
    }
    // ③c 边界：fromSec **恰落在**命中分钟（10:15:00 配 */15）→ 该分钟不算，取下一命中
    //    （10:30）；命中分钟内的任意秒（10:15:30）按同一规则 → 也是 10:30。
    //    语义 = 「严格晚于 fromSec 所在分钟」的下一命中（实现自 fromSec/60 + 1 起扫）。
    //    这条盯的是「秒级抖动不该改变答案」：UI 的「下次运行」与调度 tick 都吃这个口径。
    {
        auto parsed = module::agent::parseCron("*/15 * * * *");
        DOCTEST_REQUIRE(parsed.isOk());
        auto onBoundary = module::agent::nextCronMatch(
            parsed.value(), localSecOf(2026, 10, 9, 10, 15));
        DOCTEST_REQUIRE(onBoundary.isOk());
        DOCTEST_CHECK(
            sameMinute(onBoundary.value(), expectLocalOf(2026, 10, 9, 10, 30)));
        auto midMinute = module::agent::nextCronMatch(
            parsed.value(), localSecOf(2026, 10, 9, 10, 15) + 30);
        DOCTEST_REQUIRE(midMinute.isOk());
        DOCTEST_CHECK(
            sameMinute(midMinute.value(), expectLocalOf(2026, 10, 9, 10, 30)));
    }
    // ④ 无匹配到顶：2 月 31 日永不命中 → fail(ValidationError) 且不死循环
    {
        auto parsed = module::agent::parseCron(exprs[4]);
        DOCTEST_REQUIRE(parsed.isOk());
        auto r = module::agent::nextCronMatch(parsed.value(), froms[4]);
        DOCTEST_REQUIRE(r.isFail());
        DOCTEST_CHECK_EQ(r.errorCode(), core::ErrorCode::ValidationError);
        DOCTEST_CHECK_MESSAGE(r.errorMessage().find("一年内无触发时间") !=
                                  std::string::npos,
                              "错误文案应含「一年内无触发时间」，实得: " +
                                  r.errorMessage());
    }
    return true;
})

// previousCronMatch（详情弹窗「计划 vs 实际」对照，spec §12.3）：与 nextCronMatch
// 的边界差是**包含 fromSec 所在分钟**——run 在 09:00:30 启动，0 9 * * * 应算 09:00 计划。
// 期望值用 expectLocalOf 构造（与被测函数同一时区口径），不写死日期字符串。
REGISTER_TEST(agent_cron_previous_match,
              "cron: previousCronMatch 含当前分钟/回扫跨天/周过滤/无匹配到顶 fail", {
    using module::agent::parseCron;
    using module::agent::previousCronMatch;
    // ① 含 fromSec 所在分钟：09:00:30 → 计划 09:00；秒级抖动不改变答案
    {
        auto parsed = parseCron("0 9 * * *");
        DOCTEST_REQUIRE(parsed.isOk());
        auto inMinute = previousCronMatch(parsed.value(),
                                          localSecOf(2026, 10, 9, 9, 0) + 30);
        DOCTEST_REQUIRE(inMinute.isOk());
        DOCTEST_CHECK(
            sameMinute(inMinute.value(), expectLocalOf(2026, 10, 9, 9, 0)));
        auto midMinute = previousCronMatch(parsed.value(),
                                           localSecOf(2026, 10, 9, 9, 0) + 59);
        DOCTEST_REQUIRE(midMinute.isOk());
        DOCTEST_CHECK(
            sameMinute(midMinute.value(), expectLocalOf(2026, 10, 9, 9, 0)));
    }
    // ② 跨天回扫：10-09 08:00 → 最近命中是 10-08 09:00
    {
        auto parsed = parseCron("0 9 * * *");
        DOCTEST_REQUIRE(parsed.isOk());
        auto r =
            previousCronMatch(parsed.value(), localSecOf(2026, 10, 9, 8, 0));
        DOCTEST_REQUIRE(r.isOk());
        DOCTEST_CHECK(sameMinute(r.value(), expectLocalOf(2026, 10, 8, 9, 0)));
    }
    // ③ DOW 过滤：0 9 * * 1-5，from = 周六 10:00 → 最近命中是周五 09:00
    //（2026-10-10 是周六；期望用同一 mktime 本地口径构造）
    {
        auto parsed = parseCron("0 9 * * 1-5");
        DOCTEST_REQUIRE(parsed.isOk());
        auto r =
            previousCronMatch(parsed.value(), localSecOf(2026, 10, 10, 10, 0));
        DOCTEST_REQUIRE(r.isOk());
        const std::time_t friday = [] {
            std::tm t{};
            core::time::localTime(localSecOf(2026, 10, 10, 10, 0), &t);
            t.tm_mday -= 1;   // 周五
            t.tm_hour = 9;
            t.tm_min = 0;
            t.tm_sec = 0;
            t.tm_isdst = -1;
            return std::mktime(&t);
        }();
        std::tm fridayLocal{};
        core::time::localTime(friday, &fridayLocal);
        DOCTEST_CHECK(sameMinute(r.value(), fridayLocal));
        DOCTEST_CHECK(r.value().tm_wday >= 1 && r.value().tm_wday <= 5);
    }
    // ④ 全通配：返回 fromSec 所在分钟本身
    {
        auto parsed = parseCron("* * * * *");
        DOCTEST_REQUIRE(parsed.isOk());
        const std::time_t from = localSecOf(2026, 10, 9, 10, 7) + 20;
        auto r = previousCronMatch(parsed.value(), from);
        DOCTEST_REQUIRE(r.isOk());
        std::tm fromLocal{};
        core::time::localTime(from, &fromLocal);
        DOCTEST_CHECK(sameMinute(r.value(), fromLocal));
    }
    // ⑤ 无匹配到顶：2 月 31 日 → fail(ValidationError, "一年内无匹配时间")
    {
        auto parsed = parseCron("0 0 31 2 *");
        DOCTEST_REQUIRE(parsed.isOk());
        auto r = previousCronMatch(parsed.value(), localSecOf(2026, 10, 9, 10, 0));
        DOCTEST_REQUIRE(r.isFail());
        DOCTEST_CHECK_EQ(r.errorCode(), core::ErrorCode::ValidationError);
        DOCTEST_CHECK_MESSAGE(r.errorMessage().find("一年内无匹配时间") !=
                                  std::string::npos,
                              "错误文案应含「一年内无匹配时间」，实得: " +
                                  r.errorMessage());
    }
    return true;
})
