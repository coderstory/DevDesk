// tests/test_modules/test_schedule_ui_edit.cpp
// 定时调度编辑弹窗（波 S7b）纯逻辑契约：表单 ↔ spec_json 六 kind 往返、锚定日期
// 周几校验、预览 5 次迭代、渠道多选落 channel_ids、非法 spec 红字不落库。
// 每条用例对应一个业务场景并注明防什么事故；规则解析/时刻计算一律走
// core::schedule（交接文档 §3 冻结契约），本文件不测时刻算法本身。
#include "doctest.h"

#include "core/schedule_rule.h"
#include "core/time.h"
#include "module_schedule/schedule_ui_edit.h"

#include <ctime>
#include <string>
#include <vector>

namespace {

using core::schedule::RuleKind;
using module::schedule::ui::EditFormState;

// 本地墙钟 → Unix 秒（mktime(tm_isdst=-1)，测试只对墙钟时刻断言，跨时区/DST 稳定）
int64_t localUnix(int y, int m, int d, int h, int mi, int s = 0) {
    std::tm t{};
    t.tm_year = y - 1900;
    t.tm_mon = m - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_sec = s;
    t.tm_isdst = -1;
    return static_cast<int64_t>(std::mktime(&t));
}

core::schedule::FireContext makeCtx(int64_t nowUnix) {
    core::schedule::FireContext ctx;
    ctx.nowUnix = nowUnix;
    core::time::localTime(static_cast<std::time_t>(nowUnix), &ctx.localNow);
    ctx.lastEvalAt = nowUnix;
    return ctx;
}

// 工作日 09:00 的日历表单（unit=day + anchor_date；day/week 都要求 anchor_date）
EditFormState makeDailyForm() {
    EditFormState f;
    f.kind = RuleKind::Calendar;
    f.calendarUnit = "day";
    f.calendarInterval = 1;
    f.calendarHour = 9;
    f.calendarMinute = 0;
    f.calendarAnchorDate = "2026-10-27";  // 周二
    return f;
}

}  // namespace

// 防：once 表单字段与 spec_json 字段脱节，闹钟存错时刻（at_local 形态）。
REGISTER_TEST(scheduleEditRoundTripOnceLocal, "schedule 编辑弹窗：once(at_local) 表单→spec_json 往返", {
    EditFormState f;
    f.kind = RuleKind::Once;
    f.onceUseLocal = true;
    f.onceAtLocal = "2026-11-02 08:30";
    const auto spec = module::schedule::ui::validateAndParse(f);
    DOCTEST_REQUIRE(spec.isOk());
    DOCTEST_REQUIRE(spec.value().kind == RuleKind::Once);
    DOCTEST_REQUIRE(spec.value().hasAtLocal);
    DOCTEST_CHECK(spec.value().atLocal.tm_year + 1900 == 2026);
    DOCTEST_CHECK(spec.value().atLocal.tm_mon + 1 == 11);
    DOCTEST_CHECK(spec.value().atLocal.tm_mday == 2);
    DOCTEST_CHECK(spec.value().atLocal.tm_hour == 8);
    DOCTEST_CHECK(spec.value().atLocal.tm_min == 30);
    return true;
});

// 防：once 绝对时刻形态（at）与本地形态（at_local）互串。
REGISTER_TEST(scheduleEditRoundTripOnceAt, "schedule 编辑弹窗：once(at) 表单→spec_json 往返", {
    EditFormState f;
    f.kind = RuleKind::Once;
    f.onceUseLocal = false;
    f.onceAtUnix = 1700000000;
    const auto spec = module::schedule::ui::validateAndParse(f);
    DOCTEST_REQUIRE(spec.isOk());
    DOCTEST_CHECK(spec.value().hasAt);
    DOCTEST_CHECK(spec.value().atUnix == 1700000000);
    DOCTEST_CHECK_FALSE(spec.value().hasAtLocal);
    return true;
});

// 防：interval 网格参数（every_sec/anchor/window）在表单↔JSON 间丢失导致相位漂移。
REGISTER_TEST(scheduleEditRoundTripInterval, "schedule 编辑弹窗：interval 表单→spec_json 往返", {
    EditFormState f;
    f.kind = RuleKind::Interval;
    f.intervalEverySec = 1800;
    f.intervalAnchor = 1000;
    f.intervalWindow = 10;
    const auto spec = module::schedule::ui::validateAndParse(f);
    DOCTEST_REQUIRE(spec.isOk());
    DOCTEST_CHECK(spec.value().kind == RuleKind::Interval);
    DOCTEST_CHECK(spec.value().everySec == 1800);
    DOCTEST_CHECK(spec.value().anchor == 1000);
    DOCTEST_CHECK(spec.value().windowSec == 10);
    return true;
});

// 防：周规则的周几/相位锚点（anchor_date）在表单↔JSON 间漂移（每两周周二 09:00）。
REGISTER_TEST(scheduleEditRoundTripCalendarWeek, "schedule 编辑弹窗：calendar(week) 表单→spec_json 往返", {
    EditFormState f;
    f.kind = RuleKind::Calendar;
    f.calendarUnit = "week";
    f.calendarInterval = 2;
    f.calendarWeekdays = {2};  // 周二
    f.calendarHour = 9;
    f.calendarMinute = 0;
    f.calendarAnchorDate = "2026-10-27";  // 周二
    const auto spec = module::schedule::ui::validateAndParse(f);
    DOCTEST_REQUIRE(spec.isOk());
    const auto& c = spec.value().calendar;
    DOCTEST_CHECK(c.unit == core::schedule::CalendarSpec::Unit::Week);
    DOCTEST_CHECK(c.interval == 2);
    DOCTEST_CHECK(c.weekdays[2]);
    DOCTEST_CHECK_FALSE(c.weekdays[1]);
    DOCTEST_CHECK(c.hour == 9);
    DOCTEST_CHECK(c.minute == 0);
    DOCTEST_CHECK(c.anchorDayNum == core::time::daysFromCivil(2026, 10, 27));
    return true;
});

// 防：每月三种选择器（第 N 天 / 第 N 个周几 / 最后工作日）互串。
REGISTER_TEST(scheduleEditRoundTripCalendarMonthSelectors, "schedule 编辑弹窗：calendar(month) 三选择器往返", {
    {
        EditFormState f;
        f.kind = RuleKind::Calendar;
        f.calendarUnit = "month";
        f.calendarHasMonthDay = true;
        f.calendarMonthDay = 15;
        f.calendarHour = 10;
        f.calendarMinute = 0;
        const auto spec = module::schedule::ui::validateAndParse(f);
        DOCTEST_REQUIRE(spec.isOk());
        DOCTEST_CHECK(spec.value().calendar.hasMonthDay);
        DOCTEST_CHECK(spec.value().calendar.monthDay == 15);
    }
    {
        EditFormState f;
        f.kind = RuleKind::Calendar;
        f.calendarUnit = "month";
        f.calendarHasMonthDay = false;
        f.calendarHasNthWeekday = true;
        f.calendarNth = 2;
        f.calendarNthWeekday = 1;  // 周一
        f.calendarHour = 9;
        const auto spec = module::schedule::ui::validateAndParse(f);
        DOCTEST_REQUIRE(spec.isOk());
        DOCTEST_CHECK(spec.value().calendar.hasNthWeekday);
        DOCTEST_CHECK(spec.value().calendar.nth == 2);
        DOCTEST_CHECK(spec.value().calendar.nthWeekday == 1);
    }
    {
        EditFormState f;
        f.kind = RuleKind::Calendar;
        f.calendarUnit = "month";
        f.calendarHasMonthDay = false;
        f.calendarLastWorkday = true;
        f.calendarHour = 17;
        const auto spec = module::schedule::ui::validateAndParse(f);
        DOCTEST_REQUIRE(spec.isOk());
        DOCTEST_CHECK(spec.value().calendar.lastWorkday);
    }
    return true;
});

// 防：「工作日 09:00」控件形态的 workdays_only 标志丢失（周末误触发）。
REGISTER_TEST(scheduleEditRoundTripCalendarWorkdays, "schedule 编辑弹窗：calendar(day+工作日) 往返", {
    EditFormState f = makeDailyForm();
    f.calendarWorkdaysOnly = true;
    const auto spec = module::schedule::ui::validateAndParse(f);
    DOCTEST_REQUIRE(spec.isOk());
    DOCTEST_CHECK(spec.value().calendar.unit == core::schedule::CalendarSpec::Unit::Day);
    DOCTEST_CHECK(spec.value().calendar.workdaysOnly);
    DOCTEST_CHECK(spec.value().calendar.hour == 9);
    return true;
});

// 防：cron 表达式经 JSON 转义损坏（空格/星号/逗号）。
REGISTER_TEST(scheduleEditRoundTripCron, "schedule 编辑弹窗：cron 表单→spec_json 往返", {
    EditFormState f;
    f.kind = RuleKind::Cron;
    f.cronExpr = "30 9 * * 1-5";
    const auto spec = module::schedule::ui::validateAndParse(f);
    DOCTEST_REQUIRE(spec.isOk());
    DOCTEST_CHECK(spec.value().kind == RuleKind::Cron);
    return true;
});

// 防：event 事件名列表与冷却在表单↔JSON 间丢失；resident 空 spec 被解析器拒收。
REGISTER_TEST(scheduleEditRoundTripEventAndResident, "schedule 编辑弹窗：event/resident 往返", {
    {
        EditFormState f;
        f.kind = RuleKind::Event;
        f.eventNames = {"todo:saved", "note:edit"};
        f.eventCooldownSec = 60;
        const auto spec = module::schedule::ui::validateAndParse(f);
        DOCTEST_REQUIRE(spec.isOk());
        DOCTEST_REQUIRE(spec.value().events.size() == 2);
        DOCTEST_CHECK(spec.value().events[0] == "todo:saved");
        DOCTEST_CHECK(spec.value().events[1] == "note:edit");
        DOCTEST_CHECK(spec.value().cooldownSec == 60);
    }
    {
        EditFormState f;
        f.kind = RuleKind::Resident;
        const auto spec = module::schedule::ui::validateAndParse(f);
        DOCTEST_REQUIRE(spec.isOk());
        DOCTEST_CHECK(spec.value().kind == RuleKind::Resident);
    }
    return true;
});

// 防：用户选了周一但锚定日期是周二时静默保存 → 相位错位（每两周规则整体错一周）。
REGISTER_TEST(scheduleEditAnchorWeekdayMismatchRejected, "schedule 编辑弹窗：anchor_date 周几与所选周几不匹配拒绝保存", {
    EditFormState f;
    f.kind = RuleKind::Calendar;
    f.calendarUnit = "week";
    f.calendarWeekdays = {1};       // 只勾周一
    f.calendarAnchorDate = "2026-10-27";  // 周二
    const auto bad = module::schedule::ui::validateAndParse(f);
    DOCTEST_CHECK(bad.isFail());
    DOCTEST_CHECK(bad.errorMessage().find("锚定日期") != std::string::npos);  // 红字文案可展示

    f.calendarWeekdays = {2};  // 改勾周二 → 通过
    const auto good = module::schedule::ui::validateAndParse(f);
    DOCTEST_CHECK(good.isOk());
    return true;
});

// 防：月规则三选择器全不选时落库出「永不触发」的哑规则。
REGISTER_TEST(scheduleEditMonthSelectorRequired, "schedule 编辑弹窗：month 三选择器至少其一", {
    EditFormState f;
    f.kind = RuleKind::Calendar;
    f.calendarUnit = "month";
    f.calendarHasMonthDay = false;
    f.calendarHasNthWeekday = false;
    f.calendarLastWorkday = false;
    DOCTEST_CHECK(module::schedule::ui::validateAndParse(f).isFail());
    return true;
});

// 防：预览自算而非 nextFire 迭代，与引擎实际触发时刻漂移（预览 09:00 实际 10:00）。
REGISTER_TEST(scheduleEditPreviewFiveIterations, "schedule 编辑弹窗：预览 5 次 = nextFire 迭代契约", {
    EditFormState f = makeDailyForm();  // 每天 09:00
    const int64_t after = localUnix(2026, 10, 27, 8, 0);
    const auto preview = module::schedule::ui::previewNextFires(f, after, makeCtx(after));
    DOCTEST_REQUIRE(preview.isOk());
    DOCTEST_REQUIRE(preview.value().size() == 5);
    for (int i = 0; i < 5; ++i) {
        const int64_t expect = localUnix(2026, 10, 27 + i, 9, 0);
        DOCTEST_CHECK(preview.value()[static_cast<size_t>(i)] == expect);
        if (i > 0) {
            DOCTEST_CHECK(preview.value()[static_cast<size_t>(i)] >
                  preview.value()[static_cast<size_t>(i - 1)]);  // 严格递增
        }
    }
    return true;
});

// 防：一次性规则预览越界（过去时刻 → 空预览而非 5 个历史时刻）。
REGISTER_TEST(scheduleEditPreviewOncePastIsEmpty, "schedule 编辑弹窗：一次性已过 → 预览为空", {
    EditFormState f;
    f.kind = RuleKind::Once;
    f.onceUseLocal = true;
    f.onceAtLocal = "2020-01-01 09:00";
    const int64_t now = localUnix(2026, 10, 27, 8, 0);
    const auto preview = module::schedule::ui::previewNextFires(f, now, makeCtx(now));
    DOCTEST_REQUIRE(preview.isOk());
    DOCTEST_CHECK(preview.value().empty());
    return true;
});

// 防：渠道多选/动作/错过策略在保存载荷中丢失 → 投递错渠道、错过策略被重置为默认。
REGISTER_TEST(scheduleEditBuildRecordChannelsAndPolicy, "schedule 编辑弹窗：保存载荷含 channel_ids/动作/策略与重算的 next_fire_at", {
    EditFormState f = makeDailyForm();
    f.name = "工作日提醒";
    f.actionKind = "remind_open";
    f.channelIds = {1, 3};
    f.missedPolicy = "all";
    const int64_t now = localUnix(2026, 10, 27, 8, 0);
    const auto rec = module::schedule::ui::buildRuleRecord(f, makeCtx(now));
    DOCTEST_REQUIRE(rec.isOk());
    DOCTEST_CHECK(rec.value().kind == "calendar");
    DOCTEST_CHECK(rec.value().name == "工作日提醒");
    DOCTEST_CHECK(rec.value().actionKind == "remind_open");
    DOCTEST_REQUIRE(rec.value().channelIds.size() == 2);
    DOCTEST_CHECK(rec.value().channelIds[0] == 1);
    DOCTEST_CHECK(rec.value().channelIds[1] == 3);
    DOCTEST_CHECK(rec.value().missedPolicy == "all");
    DOCTEST_REQUIRE(rec.value().nextFireAt.has_value());  // §4.4：保存时重算 next_fire_at
    DOCTEST_CHECK(*rec.value().nextFireAt == localUnix(2026, 10, 27, 9, 0));
    return true;
});

// 防：非法 spec 静默落库（cron 表达式非法必须红字挡住，不产生记录）。
REGISTER_TEST(scheduleEditInvalidSpecRejectedWithText, "schedule 编辑弹窗：非法 spec 红字不落库", {
    EditFormState f;
    f.kind = RuleKind::Cron;
    f.cronExpr = "not-a-cron";
    const auto spec = module::schedule::ui::validateAndParse(f);
    DOCTEST_CHECK(spec.isFail());
    DOCTEST_CHECK_FALSE(spec.errorMessage().empty());  // 红字文案
    const int64_t now = localUnix(2026, 10, 27, 8, 0);
    const auto rec = module::schedule::ui::buildRuleRecord(f, makeCtx(now));
    DOCTEST_CHECK(rec.isFail());  // 保存载荷不产出 = 不落库
    return true;
});

// 防：既有规则编辑回填时 kind/spec 字段映射错位（改保存后规则语义被静默改写）。
REGISTER_TEST(scheduleEditFormFromRecordRoundTrip, "schedule 编辑弹窗：RuleRecord → 表单回填 → 再保存不丢语义", {
    EditFormState f = makeDailyForm();
    f.name = "回填测试";
    f.channelIds = {2};
    f.missedPolicy = "skip";
    const int64_t now = localUnix(2026, 10, 27, 8, 0);
    const auto rec = module::schedule::ui::buildRuleRecord(f, makeCtx(now));
    DOCTEST_REQUIRE(rec.isOk());
    const auto back = module::schedule::ui::formFromRecord(rec.value());
    DOCTEST_REQUIRE(back.isOk());
    DOCTEST_CHECK(back.value().kind == RuleKind::Calendar);
    DOCTEST_CHECK(back.value().calendarUnit == "day");
    DOCTEST_CHECK(back.value().calendarHour == 9);
    DOCTEST_CHECK(back.value().channelIds.size() == 1);
    DOCTEST_CHECK(back.value().channelIds[0] == 2);
    DOCTEST_CHECK(back.value().missedPolicy == "skip");
    // 回填后的表单再保存 → spec 仍合法（往返闭环）
    const auto again = module::schedule::ui::buildRuleRecord(back.value(), makeCtx(now));
    DOCTEST_CHECK(again.isOk());
    return true;
});

// 防：run_task 动作静默落库空 key——运行时 action_runner extract("key") 拿到空串
// 只能静默失败；保存侧必须拦（R3 A2 的反例面）。
REGISTER_TEST(scheduleEditRunTaskEmptyKeyRejected, "schedule 编辑弹窗：run_task 空 key 拒绝保存", {
    EditFormState f = makeDailyForm();
    f.actionKind = "run_task";
    f.actionJson = "{}";
    const auto empty = module::schedule::ui::validateAndParse(f);
    DOCTEST_CHECK(empty.isFail());
    DOCTEST_CHECK_FALSE(empty.errorMessage().empty());  // 弹窗内嵌红字文案
    // 带注册 key → 合法，且落库键名为 "key"（与 action_runner 消费侧对齐）
    f.actionJson = R"({"key":"md.cleanup"})";
    const auto ok = module::schedule::ui::validateAndParse(f);
    DOCTEST_REQUIRE(ok.isOk());
    const int64_t now = localUnix(2026, 10, 27, 8, 0);
    const auto rec = module::schedule::ui::buildRuleRecord(f, makeCtx(now));
    DOCTEST_REQUIRE(rec.isOk());
    DOCTEST_CHECK(rec.value().actionJson.find("\"key\"") != std::string::npos);
    DOCTEST_CHECK(rec.value().actionJson.find("md.cleanup") != std::string::npos);
    return true;
});
