// tests/test_modules/test_schedule_rule.cpp
// core::schedule 规则引擎纯函数单测：六 kind 解析/校验 + nextFire/missedFires 时间推进。
// 每条用例注明「防什么事故」；纯函数零 IO 零时钟（时间全由测试注入），无 fixture。
// 环境前提：非 DST 用例依赖系统本地时区；DST 用例经 ScopedTz 进程内切到
// America/New_York 真实跑（跳变日钉死 2026-03-08 / 2026-11-01），TZ 切换未生效
// （tzdata 缺失 / Windows 不识别 IANA 名）时显式 SKIP 兜底，不假绿。
// 跨文件前提：nextFire 严格大于 afterUnix；日历推进用 mktime(tm_isdst=-1)+daysFromCivil，
// 禁止 +86400；秋季回拨重复墙钟只取第一次；春季缺口在最早存在时刻触发。
#include "doctest.h"
#include "test_support.h"
#include <ctime>
#include <iostream>

#include "core/schedule_rule.h"
#include "core/time.h"

namespace {
using core::schedule::FireContext;
using core::schedule::RuleKind;
using core::schedule::RuleSpec;
using core::schedule::nextFire;
using core::schedule::missedFires;
using core::schedule::parseRuleSpec;

// 哨兵：所有用例时刻均为 2026 年正值 Unix 秒，-1/-2 不与真实时刻重叠
constexpr int64_t kNoFire = -1;  // nextFire 返回 nullopt（无下次）
constexpr int64_t kErr = -2;     // nextFire/missedFires 返回错误（本层不应发生）

// 本地墙钟（参数为 1 起月份）→ Unix 秒（mktime isdst=-1，与引擎同一换算路径）
std::time_t unixOf(int y, int mon, int day, int hour, int min) {
    std::tm t{};
    t.tm_year = y - 1900;
    t.tm_mon = mon - 1;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min = min;
    t.tm_sec = 0;
    t.tm_isdst = -1;
    return std::mktime(&t);
}

FireContext ctxAt(std::time_t u) {
    FireContext c;
    c.nowUnix = static_cast<int64_t>(u);
    core::time::localTime(u, &c.localNow);
    return c;
}

int64_t nf(const RuleSpec& s, std::time_t after, const FireContext& c) {
    auto r = nextFire(s, static_cast<int64_t>(after), c);
    if (!r.isOk()) return kErr;
    const auto v = r.value();
    return v.has_value() ? *v : kNoFire;
}

std::vector<int64_t> mf(const RuleSpec& s, std::time_t from, std::time_t to, int cap,
                        const FireContext& c) {
    auto r = missedFires(s, static_cast<int64_t>(from), static_cast<int64_t>(to), cap, c);
    if (!r.isOk()) return std::vector<int64_t>();
    return r.value();
}

RuleSpec mustParse(RuleKind k, const std::string& json) {
    auto r = parseRuleSpec(k, json);
    if (!r.isOk()) {
        std::cerr << "FAIL mustParse: " << json << " -> " << r.errorMessage() << std::endl;
        return RuleSpec{};  // kind=Event → 后续 nextFire 断言自然失败，配合 cerr 定位
    }
    return r.value();
}

// NY 钉死前提自检（全走 libc 换算：unixOf=mktime isdst=-1，daysFromCivil 为纯算术，
// 不经引擎——SKIP 只代表环境不具备钉死前提，掩盖不了引擎回归）：
// TZ=America/New_York 切换真实生效 + EST(-5h)/EDT(-4h) 偏移与 2026 年两个跳变日
// 都在期望位置（美国现行规则：3 月第 2 个周日春进 / 11 月第 1 个周日秋回）。
bool nyDstEnvReady() {
    return static_cast<int64_t>(unixOf(2026, 1, 15, 12, 0)) ==
               core::time::daysFromCivil(2026, 1, 15) * 86400 + 17 * 3600 &&   // 12:00 EST
           static_cast<int64_t>(unixOf(2026, 7, 15, 12, 0)) ==
               core::time::daysFromCivil(2026, 7, 15) * 86400 + 16 * 3600 &&   // 12:00 EDT
           static_cast<int64_t>(unixOf(2026, 3, 8, 1, 30)) ==
               core::time::daysFromCivil(2026, 3, 8) * 86400 + 6 * 3600 + 1800 &&   // 春进前 EST
           static_cast<int64_t>(unixOf(2026, 3, 8, 3, 30)) ==
               core::time::daysFromCivil(2026, 3, 8) * 86400 + 7 * 3600 + 1800 &&   // 春进后 EDT
           static_cast<int64_t>(unixOf(2026, 11, 1, 0, 30)) ==
               core::time::daysFromCivil(2026, 11, 1) * 86400 + 4 * 3600 + 1800 &&  // 秋回前 EDT
           static_cast<int64_t>(unixOf(2026, 11, 1, 2, 30)) ==
               core::time::daysFromCivil(2026, 11, 1) * 86400 + 7 * 3600 + 1800;    // 秋回后 EST
}
} // namespace

// 解析：once 的 at / at_local / 互斥 / 缺失 / 非法格式。
// 防：once 双字段同时出现被静默取其一（到点时刻错）；at_local 非法日期（2 月 30 日）被 mktime 归一化成别的日子
REGISTER_TEST(schedule_parse_once, "schedule: once 解析与校验（at / at_local / 互斥 / 非法拒绝）", {
    const RuleSpec a = mustParse(RuleKind::Once, R"({"at": 1770000000})");
    DOCTEST_CHECK(a.hasAt);
    DOCTEST_CHECK(a.atUnix == 1770000000);
    DOCTEST_CHECK(!a.hasAtLocal);

    const RuleSpec b = mustParse(RuleKind::Once, R"({"at_local": "2026-10-28 09:30"})");
    DOCTEST_CHECK(b.hasAtLocal);
    DOCTEST_CHECK(b.atLocal.tm_year == 126 && b.atLocal.tm_mon == 9 && b.atLocal.tm_mday == 28);
    DOCTEST_CHECK(b.atLocal.tm_hour == 9 && b.atLocal.tm_min == 30);

    const RuleSpec c = mustParse(RuleKind::Once, R"({"at_local": "2026-10-28T09:30"})");
    DOCTEST_CHECK(c.hasAtLocal);

    for (const char* bad : {
             R"({})", R"({"at": 1, "at_local": "2026-10-28 09:30"})",
             R"({"at_local": "2026-2-30 09:30"})", R"({"at_local": "2026-10-28 24:00"})",
             R"({"at_local": "2026/10/28 09:30"})", R"({"at": "x"})",
             R"({"at": 1, "extra": 2})", R"({"at_local": "2026-10-28 09:30", "at": 5})"}) {
        DOCTEST_CHECK_MESSAGE(parseRuleSpec(RuleKind::Once, bad).isFail(),
                              "应拒绝: " + std::string(bad));
    }
    return true;
})

// 解析：interval 的 every_sec/anchor/window 校验。
// 防：every_sec=0 造成除零/死循环；window ≥ every_sec 使容差吞掉整个周期
REGISTER_TEST(schedule_parse_interval, "schedule: interval 解析与校验（every_sec / anchor / window）", {
    const RuleSpec a = mustParse(RuleKind::Interval, R"({"every_sec": 3600})");
    DOCTEST_CHECK(a.everySec == 3600 && a.anchor == 0 && a.windowSec == 0);
    const RuleSpec b = mustParse(RuleKind::Interval, R"({"every_sec": 60, "anchor": 100, "window": 10})");
    DOCTEST_CHECK(b.everySec == 60 && b.anchor == 100 && b.windowSec == 10);
    for (const char* bad : {
             R"({})", R"({"every_sec": 0})", R"({"every_sec": -5})",
             R"({"every_sec": 60, "anchor": -1})", R"({"every_sec": 60, "window": 60})",
             R"({"every_sec": 60, "window": -1})", R"({"every_sec": 60, "extra": 1})"}) {
        DOCTEST_CHECK_MESSAGE(parseRuleSpec(RuleKind::Interval, bad).isFail(),
                              "应拒绝: " + std::string(bad));
    }
    return true;
})

// 解析：calendar 跨字段约束。
// 防：anchor_date 与 weekdays 不匹配导致「第 0 周」错位（相位漂一周）；month 缺日选择器静默按 1 号触发
REGISTER_TEST(schedule_parse_calendar, "schedule: calendar 解析与跨字段校验", {
    const RuleSpec w = mustParse(RuleKind::Calendar,
        R"({"unit":"week","interval":2,"weekdays":[2],"time":"09:00","anchor_date":"2026-10-27"})");
    DOCTEST_CHECK(w.calendar.unit == core::schedule::CalendarSpec::Unit::Week);
    DOCTEST_CHECK(w.calendar.interval == 2);
    DOCTEST_CHECK(w.calendar.weekdays[2] && !w.calendar.weekdays[1]);
    DOCTEST_CHECK(w.calendar.hour == 9 && w.calendar.minute == 0);

    const RuleSpec m = mustParse(RuleKind::Calendar,
        R"({"unit":"month","interval":1,"month_day":15,"time":"10:00"})");
    DOCTEST_CHECK(m.calendar.hasMonthDay && m.calendar.monthDay == 15);

    const RuleSpec nw = mustParse(RuleKind::Calendar,
        R"({"unit":"month","nth_weekday":{"n":2,"weekday":2},"time":"08:00"})");
    DOCTEST_CHECK(nw.calendar.hasNthWeekday && nw.calendar.nth == 2 && nw.calendar.nthWeekday == 2);

    const RuleSpec lw = mustParse(RuleKind::Calendar,
        R"({"unit":"month","last_workday":true,"time":"17:00","workdays_only":true})");
    DOCTEST_CHECK(lw.calendar.lastWorkday && lw.calendar.workdaysOnly);

    for (const char* bad : {
             // anchor_date 是周三而 weekdays=[2]（周二）→ 必须拒
             R"({"unit":"week","weekdays":[2],"time":"09:00","anchor_date":"2026-10-28"})",
             R"({"unit":"week","weekdays":[2],"time":"09:00"})",   // 缺 anchor_date
             R"({"unit":"week","anchor_date":"2026-10-27","time":"09:00"})",  // 空 weekdays
             R"({"unit":"day","interval":1,"time":"09:00"})",      // day 缺 anchor_date
             R"({"unit":"month","time":"10:00"})",                 // month 缺日选择器
             R"({"unit":"month","month_day":15,"nth_weekday":{"n":1,"weekday":1},"time":"10:00"})",
             R"({"unit":"month","month_day":15,"time":"10:00","anchor_date":"2026-01-01"})",
             R"({"unit":"week","weekdays":[2],"month_day":1,"time":"09:00","anchor_date":"2026-10-27"})",
             R"({"unit":"month","month_day":0,"time":"10:00"})",
             R"({"unit":"month","month_day":32,"time":"10:00"})",
             R"({"unit":"month","nth_weekday":{"n":0,"weekday":1},"time":"10:00"})",
             R"({"unit":"month","nth_weekday":{"n":6,"weekday":1},"time":"10:00"})",
             R"({"unit":"month","nth_weekday":{"n":1,"weekday":7},"time":"10:00"})",
             R"({"unit":"week","weekdays":[2,7],"time":"09:00","anchor_date":"2026-10-27"})",
             R"({"unit":"moon","month_day":1,"time":"00:00"})",
             R"({"unit":"month","month_day":15})",                 // 缺 time
             R"({"unit":"month","month_day":15,"time":"9:00"})",   // time 非 HH:MM
             R"({"unit":"month","month_day":15,"time":"10:00","interval":0})",
             R"({"unit":"month","month_day":15,"time":"10:00","extra":1})"}) {
        DOCTEST_CHECK_MESSAGE(parseRuleSpec(RuleKind::Calendar, bad).isFail(),
                              "应拒绝: " + std::string(bad));
    }
    return true;
})

// 解析：cron / event / resident / 非对象根。
// 防：cron 非法表达式漏过（下一层数组越界）；event/resident 误收字段静默生效
REGISTER_TEST(schedule_parse_cron_event_resident, "schedule: cron / event / resident 解析与拒绝域", {
    const RuleSpec cr = mustParse(RuleKind::Cron, R"({"expr": "30 14 * * 1"})");
    DOCTEST_CHECK(cr.kind == RuleKind::Cron);
    const RuleSpec ev = mustParse(RuleKind::Event, R"({"events":["todo.saved"],"cooldown_sec":30})");
    DOCTEST_CHECK(ev.kind == RuleKind::Event && ev.events.size() == 1 && ev.cooldownSec == 30);
    const RuleSpec rs = mustParse(RuleKind::Resident, R"({})");
    DOCTEST_CHECK(rs.kind == RuleKind::Resident);
    for (const char* bad : {
             R"({"expr": "bad expr"})", R"({"expr": "61 * * * *"})", R"({})",
             R"({"expr": "30 14 * * 1", "x": 1})", R"("not an object")",
             "not json at all", ""}) {
        DOCTEST_CHECK_MESSAGE(parseRuleSpec(RuleKind::Cron, bad).isFail(),
                              "应拒绝(cron): " + std::string(bad));
    }
    for (const char* bad : {R"({"events": "x"})", R"({"events": [1]})", R"({"cooldown_sec": -1})",
                            R"({"x": 1})", R"({"events": [], "extra": 1})"}) {
        DOCTEST_CHECK_MESSAGE(parseRuleSpec(RuleKind::Event, bad).isFail(),
                              "应拒绝(event): " + std::string(bad));
    }
    DOCTEST_CHECK(parseRuleSpec(RuleKind::Resident, R"({"x": 1})").isFail());
    DOCTEST_CHECK(parseRuleSpec(RuleKind::Once, R"("not an object")").isFail());
    DOCTEST_CHECK(parseRuleSpec(RuleKind::Once, "not json at all").isFail());
    DOCTEST_CHECK(parseRuleSpec(RuleKind::Once, "").isFail());
    return true;
})

// §9.1：「每两周周二 09:00」，anchor_date=2026-10-27（周二），连续推进 8 周触发集合
// 恰好为 10-27 / 11-10 / 11-24 / 12-08 的 09:00。
// 防：周相位错位（从 anchor 当周起算还是次周起算）、间隔漏翻倍、推进出现非周二
REGISTER_TEST(schedule_biweekly_tuesday, "schedule: 每两周周二 09:00 连续推进 8 周（§9.1）", {
    const RuleSpec s = mustParse(RuleKind::Calendar,
        R"({"unit":"week","interval":2,"weekdays":[2],"time":"09:00","anchor_date":"2026-10-27"})");
    const FireContext c = ctxAt(unixOf(2026, 10, 20, 0, 0));
    const std::time_t expect[4] = {unixOf(2026, 10, 27, 9, 0), unixOf(2026, 11, 10, 9, 0),
                                   unixOf(2026, 11, 24, 9, 0), unixOf(2026, 12, 8, 9, 0)};
    std::time_t cursor = 0;
    for (int i = 0; i < 4; ++i) {
        const int64_t n = nf(s, cursor, c);
        DOCTEST_CHECK_MESSAGE(n == static_cast<int64_t>(expect[i]),
                              "第 " + std::to_string(i) + " 次应等于 " + std::to_string(expect[i]));
        cursor = static_cast<std::time_t>(n);
    }
    // 8 周窗口内的全部触发（闭区间）也应恰好是这 4 个
    const auto all = mf(s, unixOf(2026, 10, 20, 0, 0), unixOf(2026, 12, 15, 0, 0), 10, c);
    DOCTEST_CHECK(all.size() == 4);
    for (size_t i = 0; i < all.size(); ++i) {
        DOCTEST_CHECK(all[i] == static_cast<int64_t>(expect[i]));
    }
    return true;
})

// §9.2：每月 15 日从 1 月 31 日起下一次是 2 月 15 日；每月 31 日在 2 月跳过、3 月 31 日触发；
// 每月最后一个工作日在 2026-10 为 10-30（2026-10-31 是周六）。
// 防：+86400 式推进在 31 日/短月翻转；最后工作日把周六日算进去
REGISTER_TEST(schedule_monthly_selectors, "schedule: 每月 15 日 / 31 日跳 2 月 / 最后工作日（§9.2）", {
    const FireContext c = ctxAt(unixOf(2026, 1, 31, 12, 0));
    const RuleSpec d15 = mustParse(RuleKind::Calendar, R"({"unit":"month","month_day":15,"time":"10:00"})");
    DOCTEST_CHECK(nf(d15, unixOf(2026, 1, 31, 12, 0), c) ==
                  static_cast<int64_t>(unixOf(2026, 2, 15, 10, 0)));

    const RuleSpec d31 = mustParse(RuleKind::Calendar, R"({"unit":"month","month_day":31,"time":"10:00"})");
    DOCTEST_CHECK_MESSAGE(nf(d31, unixOf(2026, 1, 31, 12, 0), c) ==
                              static_cast<int64_t>(unixOf(2026, 3, 31, 10, 0)),
                          "2 月无 31 日应跳过并在 3 月 31 日触发");

    const RuleSpec lw = mustParse(RuleKind::Calendar, R"({"unit":"month","last_workday":true,"time":"17:00"})");
    DOCTEST_CHECK(nf(lw, unixOf(2026, 10, 1, 0, 0), c) ==
                  static_cast<int64_t>(unixOf(2026, 10, 30, 17, 0)));
    return true;
})

// §9.3：工作日 09:00 周六/周日不触发、周一触发；跨法定节假日 v1 不处理（文档口径）。
// 防：wday 0=周日 与 1=周一 的映射颠倒（0/1 基准错位是常见事故）
REGISTER_TEST(schedule_workdays, "schedule: 工作日 09:00 周六日不触发（§9.3）", {
    const FireContext c = ctxAt(unixOf(2026, 10, 20, 0, 0));
    const RuleSpec s = mustParse(RuleKind::Calendar,
        R"({"unit":"day","interval":1,"time":"09:00","anchor_date":"2026-10-19","workdays_only":true})");
    // 2026-10-24 周六、10-25 周日 → 下一次必须是 10-26 周一
    DOCTEST_CHECK(nf(s, unixOf(2026, 10, 23, 12, 0), c) ==
                  static_cast<int64_t>(unixOf(2026, 10, 26, 9, 0)));
    DOCTEST_CHECK(nf(s, unixOf(2026, 10, 24, 12, 0), c) ==
                  static_cast<int64_t>(unixOf(2026, 10, 26, 9, 0)));
    // 区间内周六日不出现
    const auto all = mf(s, unixOf(2026, 10, 19, 0, 0), unixOf(2026, 10, 25, 23, 59), 20, c);
    for (int64_t u : all) {
        std::tm w{};
        core::time::localTime(static_cast<std::time_t>(u), &w);
        DOCTEST_CHECK_MESSAGE(w.tm_wday != 0 && w.tm_wday != 6, "周六日不得触发");
    }
    DOCTEST_CHECK(all.size() == 5);  // 10-19 至 10-23 五个工作日
    return true;
})

// 契约统一（P2-11）：calendar 400 年窗口无匹配 → ok(nullopt)，不是 fail。
// 防：两种合法形态被 UI 保存误拒——①合法稀疏 interval（首触发在 400 年外）；
// ②病态组合（unit=year + 第 5 个周日 + workdays_only：周日非工作日，恒不匹配）。
// 契约口径 = 无下次不是错误（与 cron 8 窗口无匹配一致）；回退成 fail(ValidationError) 此用例即红。
REGISTER_TEST(schedule_calendar_no_match_nullopt, "schedule: calendar 400 年无匹配返回 ok(nullopt) 而非 fail", {
    const FireContext c = ctxAt(unixOf(2026, 10, 29, 12, 0));
    // 合法稀疏：day interval=1000000（约 2739 年一周），下一次远超 400 年窗口
    const RuleSpec sparse = mustParse(RuleKind::Calendar,
        R"({"unit":"day","interval":1000000,"time":"09:00","anchor_date":"2026-10-29"})");
    {
        auto r = nextFire(sparse, static_cast<int64_t>(unixOf(2026, 10, 29, 12, 0)), c);
        DOCTEST_REQUIRE_MESSAGE(r.isOk(), "稀疏 interval 必须是 ok，不是 fail");
        DOCTEST_CHECK(!r.value().has_value());
    }
    // 病态组合：第 5 个周日 + 仅工作日 → 恒不匹配
    const RuleSpec impossible = mustParse(RuleKind::Calendar,
        R"({"unit":"year","nth_weekday":{"n":5,"weekday":0},"workdays_only":true,"time":"09:00"})");
    {
        auto r = nextFire(impossible, static_cast<int64_t>(unixOf(2026, 10, 29, 12, 0)), c);
        DOCTEST_REQUIRE_MESSAGE(r.isOk(), "病态组合也必须是 ok(nullopt)，不是 fail");
        DOCTEST_CHECK(!r.value().has_value());
    }
    // 同款病态组合的 missedFires 也必须 ok 且空，不是 fail
    {
        auto r = missedFires(impossible, static_cast<int64_t>(unixOf(2026, 1, 1, 0, 0)),
                             static_cast<int64_t>(unixOf(2026, 12, 31, 23, 59)), 10, c);
        DOCTEST_REQUIRE_MESSAGE(r.isOk(), "missedFires 同契约：无触发返回 ok 空集");
        DOCTEST_CHECK(r.value().empty());
    }
    return true;
})

// interval 锚点式推进：anchor + ceil((after-anchor)/N)*N；after < anchor 时返回 anchor 本身。
// 防：网格相位漂移（每次从 now 重起算而非锚点）、恰在边界时刻被重复返回（不严格大于）
REGISTER_TEST(schedule_interval_anchor, "schedule: interval 锚点式 nextFire / missedFires", {
    const FireContext c = ctxAt(0);
    const RuleSpec s = mustParse(RuleKind::Interval, R"({"every_sec": 3600, "anchor": 1000})");
    DOCTEST_CHECK(nf(s, 500, c) == 1000);   // after < anchor → anchor
    DOCTEST_CHECK(nf(s, 1000, c) == 4600);  // 严格大于：1000 自身不算下次
    DOCTEST_CHECK(nf(s, 4599, c) == 4600);  // 锚点网格内
    DOCTEST_CHECK(nf(s, 4600, c) == 8200);
    const auto all = mf(s, 1000, 8200, 10, c);  // 闭区间含两端
    DOCTEST_CHECK(all.size() == 3);
    DOCTEST_CHECK(all[0] == 1000 && all[1] == 4600 && all[2] == 8200);
    const auto capped = mf(s, 1000, 8200, 2, c);
    DOCTEST_CHECK(capped.size() == 2);
    // 空窗：网格 …19000, 22600…（锚点 1000 + 3600k），[20000, 22599] 无落点
    DOCTEST_CHECK(mf(s, 20000, 22599, 10, c).empty());
    return true;
})

// cron kind 复用 core::cron：nextFire 严格大于、missedFires 闭区间。
// 防：nextCronMatch 边界（fromSec 所在分钟不参与）与 missedFires 含端点口径不一致
REGISTER_TEST(schedule_cron_kind, "schedule: cron kind nextFire / missedFires（复用 core::cron）", {
    const FireContext c = ctxAt(unixOf(2026, 10, 26, 0, 0));  // 2026-10-26 是周一
    const RuleSpec s = mustParse(RuleKind::Cron, R"({"expr": "30 14 * * 1"})");
    DOCTEST_CHECK(nf(s, unixOf(2026, 10, 26, 0, 0), c) ==
                  static_cast<int64_t>(unixOf(2026, 10, 26, 14, 30)));
    DOCTEST_CHECK(nf(s, unixOf(2026, 10, 26, 14, 30), c) ==  // 已到点 → 下周一
                  static_cast<int64_t>(unixOf(2026, 11, 2, 14, 30)));
    const auto all = mf(s, unixOf(2026, 10, 26, 0, 0), unixOf(2026, 11, 2, 14, 30), 10, c);
    DOCTEST_CHECK(all.size() == 2);
    DOCTEST_CHECK(all[0] == static_cast<int64_t>(unixOf(2026, 10, 26, 14, 30)));
    DOCTEST_CHECK(all[1] == static_cast<int64_t>(unixOf(2026, 11, 2, 14, 30)));
    return true;
})

// once：到点前有下次、到点后无下次；missedFires 闭区间只含该点一次。
// 防：一次性规则被 nextFire 反复返回（重启后再触发）
REGISTER_TEST(schedule_once_fire, "schedule: once 到点后无下次（重启不重复触发）", {
    const FireContext c = ctxAt(unixOf(2026, 10, 28, 0, 0));
    const RuleSpec s = mustParse(RuleKind::Once, R"({"at_local": "2026-10-28 09:30"})");
    const int64_t t0 = static_cast<int64_t>(unixOf(2026, 10, 28, 9, 30));
    DOCTEST_CHECK(nf(s, unixOf(2026, 10, 28, 0, 0), c) == t0);
    DOCTEST_CHECK(nf(s, static_cast<std::time_t>(t0), c) == kNoFire);
    const auto all = mf(s, unixOf(2026, 10, 28, 9, 0), unixOf(2026, 10, 28, 9, 30), 10, c);
    DOCTEST_CHECK(all.size() == 1 && all[0] == t0);
    DOCTEST_CHECK(mf(s, unixOf(2026, 10, 28, 9, 31), unixOf(2026, 10, 28, 10, 0), 10, c).empty());
    return true;
})

// event / resident：无下次时刻、无错过清单。
// 防：event/resident 被误套时间线（到点引擎空转或误触发）
REGISTER_TEST(schedule_event_resident_no_fire, "schedule: event / resident 无 nextFire 无 missedFires", {
    const FireContext c = ctxAt(unixOf(2026, 10, 28, 0, 0));
    const RuleSpec ev = mustParse(RuleKind::Event, R"({"events":["todo.saved"],"cooldown_sec":30})");
    const RuleSpec rs = mustParse(RuleKind::Resident, R"({})");
    DOCTEST_CHECK(nf(ev, 0, c) == kNoFire);
    DOCTEST_CHECK(nf(rs, 0, c) == kNoFire);
    DOCTEST_CHECK(mf(ev, 0, 1048576, 10, c).empty());
    DOCTEST_CHECK(mf(rs, 0, 1048576, 10, c).empty());
    return true;
})

// §9.11：时钟回拨不重放——nextFire 永不返回 ≤ afterUnix 的时刻；已过去的触发点再问也是明天。
// lastEvalAt 的比较在引擎层（本层无状态），本用例锁住纯函数侧的前提：严格大于 + 闭区间恰好一次。
// 防：回拨后同一触发点被第二次返回（动作重复执行）
REGISTER_TEST(schedule_rollback_no_replay, "schedule: 时钟回拨不重放（§9.11 纯函数侧）", {
    const FireContext c = ctxAt(unixOf(2026, 10, 27, 9, 0));
    const RuleSpec s = mustParse(RuleKind::Calendar,
        R"({"unit":"day","interval":1,"time":"09:00","anchor_date":"2026-10-27"})");
    const int64_t t1 = static_cast<int64_t>(unixOf(2026, 10, 27, 9, 0));
    const int64_t t2 = static_cast<int64_t>(unixOf(2026, 10, 28, 9, 0));
    // 09:00 已触发过（回拨后 afterUnix 回到 09:00）：必须返回明天，而不是再次 09:00
    DOCTEST_CHECK(nf(s, static_cast<std::time_t>(t1), c) == t2);
    // 回拨前的 10:00 视角：missedFires [08:00, 10:00] 中该触发点只出现一次
    const auto all = mf(s, unixOf(2026, 10, 27, 8, 0), unixOf(2026, 10, 27, 10, 0), 10, c);
    DOCTEST_CHECK(all.size() == 1 && all[0] == t1);
    return true;
})

// §9.10 春季：跳变缺口内墙钟在最早存在时刻触发。ScopedTz 切到纽约真实跑：
// 2026-03-08（3 月第 2 个周日）02:00→03:00，墙钟 02:30 在该日不存在。
// 期望钉死真实日期（daysFromCivil 纯算术，无时区歧义）：缺口日触发 = 03:00 EDT = 07:00 UTC。
// 防：春季在缺口日静默丢一次触发、或偏到缺口后的下一个整点之外（如 03:30）
REGISTER_TEST(schedule_dst_spring_gap, "schedule: DST 春季缺口在最早存在时刻触发（§9.10 春季）", {
    const test_support::ScopedTz tz("America/New_York");
    if (!nyDstEnvReady()) {
        DOCTEST_SKIP("TZ=America/New_York 未生效（tzdata 缺失 / 受限环境），春季缺口用例跳过");
    }
    const RuleSpec s = mustParse(RuleKind::Calendar,
        R"({"unit":"day","interval":1,"time":"02:30","anchor_date":"2026-01-01"})");
    const FireContext c = ctxAt(unixOf(2026, 3, 7, 12, 0));
    // 缺口前一日（24h 日）同墙钟正常触发：02:30 EST = 07:30 UTC，不走前探
    const int64_t prevFire = static_cast<int64_t>(unixOf(2026, 3, 7, 2, 30));
    DOCTEST_CHECK(prevFire == core::time::daysFromCivil(2026, 3, 7) * 86400 + 7 * 3600 + 1800);
    DOCTEST_CHECK(nf(s, unixOf(2026, 3, 7, 0, 0), c) == prevFire);
    // 缺口日：请求 02:30 → 逐分钟前探命中最早存在墙钟 03:00 EDT = 07:00 UTC（Unix 秒钉死）
    const int64_t n = nf(s, unixOf(2026, 3, 7, 12, 0), c);
    DOCTEST_CHECK_MESSAGE(n == core::time::daysFromCivil(2026, 3, 8) * 86400 + 7 * 3600,
                          "缺口日应触发在 03:00 EDT = 07:00 UTC，实际 " + std::to_string(n));
    std::tm got{};
    core::time::localTime(static_cast<std::time_t>(n), &got);
    DOCTEST_CHECK(got.tm_mday == 8 && got.tm_hour == 3 && got.tm_min == 0);
    // 23h 日的物理签名：比前一日同墙钟触发点只晚 23.5h（偏到 03:30 或次日都会破坏它）
    DOCTEST_CHECK(n - prevFire == 23 * 3600 + 1800);
    return true;
})

// §9.10 秋季：回拨日重复墙钟只触发第一次出现。ScopedTz 切到纽约真实跑：
// 2026-11-01（11 月第 1 个周日）02:00 EDT→01:00 EST，墙钟 01:30 出现两次
// （第一次 EDT = 05:30 UTC、第二次 EST = 06:30 UTC，相差恰好 1h）。
// 期望钉死真实日期：第一次出现 = daysFromCivil(2026,11,1)·86400 + 5h30m。
// 防：同一本地日期+时分被返回两次 → 引擎重复执行同一计划时刻
REGISTER_TEST(schedule_dst_fall_dedupe, "schedule: DST 秋季回拨重复墙钟只第一次（§9.10 秋季）", {
    const test_support::ScopedTz tz("America/New_York");
    if (!nyDstEnvReady()) {
        DOCTEST_SKIP("TZ=America/New_York 未生效（tzdata 缺失 / 受限环境），秋季回拨用例跳过");
    }
    const int64_t firstOcc = core::time::daysFromCivil(2026, 11, 1) * 86400 + 5 * 3600 + 1800;
    const RuleSpec s = mustParse(RuleKind::Calendar,
        R"({"unit":"day","interval":1,"time":"01:30","anchor_date":"2026-01-01"})");
    const FireContext c = ctxAt(unixOf(2026, 11, 1, 0, 0));
    // 回拨前一日 12:00 起：nextFire 命中第一次出现（EDT），不是第二次（EST）
    const int64_t n = nf(s, unixOf(2026, 10, 31, 12, 0), c);
    DOCTEST_CHECK_MESSAGE(n == firstOcc,
                          "回拨日 01:30 应命中第一次出现 05:30 UTC，实际 " + std::to_string(n));
    std::tm got{};
    core::time::localTime(static_cast<std::time_t>(n), &got);
    DOCTEST_CHECK(got.tm_mday == 1 && got.tm_hour == 1 && got.tm_min == 30);
    // 已到第一次出现后再问：不得重放第二次出现（重放），而是次日 01:30 EST = 06:30 UTC
    DOCTEST_CHECK(nf(s, static_cast<std::time_t>(firstOcc), c) ==
                  core::time::daysFromCivil(2026, 11, 2) * 86400 + 6 * 3600 + 1800);
    // 全天 missedFires 闭区间：该（本地日期, 时分）只出现一次，且为第一次出现
    const auto all = mf(s, unixOf(2026, 11, 1, 0, 0), unixOf(2026, 11, 1, 23, 59), 10, c);
    DOCTEST_CHECK(all.size() == 1);
    DOCTEST_CHECK(all[0] == firstOcc);
    return true;
})
