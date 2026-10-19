// tests/test_modules/test_agent_time_text.cpp
// agent_time_text 纯函数单测（详情弹窗 P2，spec §12.3 前 2 条）：
//   - 格式化/相对时间/时长/token 千分位/状态中文：期望值用 core::time::localTime
//     从固定字段构造，避免时区/跨零点脆弱断言。
//   - 本地日边界（startOfLocalDay / sevenDayWindowStart）：只断言「差 6 个日历日」
//     与「落在本日 00:00」，不断言具体日期字符串；跨月/跨年由本地日序号差钉住。
// 纯函数零 IO 零时钟，「现在」一律显式注入。
#include "doctest.h"
#include "module_agent/agent_time_text.h"

#include <ctime>

#include "core/time.h"

namespace {

using module::agent::AgentTaskRow;
using module::agent::AgentRunRow;

// 固定本地字段 → epoch（isdst=-1 交 mktime 决断）；测试里构造「今天 HH:MM」类
// 期望的唯一合法姿势
std::time_t epochOf(int year, int mon, int mday, int hour, int min, int sec) {
    std::tm t{};
    t.tm_year = year - 1900;
    t.tm_mon = mon - 1;
    t.tm_mday = mday;
    t.tm_hour = hour;
    t.tm_min = min;
    t.tm_sec = sec;
    t.tm_isdst = -1;
    return std::mktime(&t);
}

// 相对 nowSec 偏移 day 个本地日、钟点取 HH:MM:SS 的 epoch（DST 日不按 86400 硬减）
std::time_t dayAt(int64_t nowSec, int dayDelta, int hour, int min, int sec) {
    std::tm t{};
    core::time::localTime(static_cast<std::time_t>(nowSec), &t);
    t.tm_mday += dayDelta;
    t.tm_hour = hour;
    t.tm_min = min;
    t.tm_sec = sec;
    t.tm_isdst = -1;
    return std::mktime(&t);
}

AgentTaskRow taskOf(const char* type, const char* cfg, bool enabled = true) {
    AgentTaskRow t;
    t.id = 1;
    t.triggerType = type;
    t.triggerConfig = cfg;
    t.enabled = enabled;
    return t;
}

} // namespace

REGISTER_TEST(agent_time_text_formats,
              "agent_time_text: 绝对/短/时钟/日期/今天昨天更早/相对/时长/token/状态", {
    using namespace module::agent;
    // 固定本地时刻 2026-10-18 14:32:05
    const std::time_t t = epochOf(2026, 10, 18, 14, 32, 5);
    DOCTEST_CHECK_EQ(formatLocalDateTime(t), std::string("2026-10-18 14:32:05"));
    DOCTEST_CHECK_EQ(formatLocalShortDateTime(t), std::string("10-18 14:32:05"));
    DOCTEST_CHECK_EQ(formatLocalClock(t), std::string("14:32:05"));
    DOCTEST_CHECK_EQ(formatLocalDate(t), std::string("2026-10-18"));

    // 今天/昨天/更早（nowSec = 同日 20:00）
    const std::time_t now = epochOf(2026, 10, 18, 20, 0, 0);
    DOCTEST_CHECK_EQ(localDayLabel(t, now), std::string("今天"));
    DOCTEST_CHECK_EQ(localDayLabel(dayAt(now, -1, 9, 0, 0), now),
                     std::string("昨天"));
    DOCTEST_CHECK_EQ(localDayLabel(dayAt(now, -8, 9, 0, 0), now),
                     std::string("2026-10-10"));

    // 相对时间：刚刚 / N 分钟前 / N 小时前 / N 天前
    DOCTEST_CHECK_EQ(relativeTimeText(now - 30, now), std::string("刚刚"));
    DOCTEST_CHECK_EQ(relativeTimeText(now - 120, now), std::string("2 分钟前"));
    DOCTEST_CHECK_EQ(relativeTimeText(now - 7200, now), std::string("2 小时前"));
    DOCTEST_CHECK_EQ(relativeTimeText(now - 3 * 86400, now),
                     std::string("3 天前"));

    // 时长（契约与退役前逐字一致，含 0 秒回拨钳）
    DOCTEST_CHECK_EQ(formatRunDurationText(0), std::string("0秒"));
    DOCTEST_CHECK_EQ(formatRunDurationText(-5), std::string("0秒"));
    DOCTEST_CHECK_EQ(formatRunDurationText(133), std::string("2分13秒"));
    DOCTEST_CHECK_EQ(formatRunDurationText(4980), std::string("1时23分"));
    DOCTEST_CHECK_EQ(formatRunDurationText(90000), std::string("1天1时"));

    // token 千分位
    DOCTEST_CHECK_EQ(formatTokenCount(0), std::string("0"));
    DOCTEST_CHECK_EQ(formatTokenCount(999), std::string("999"));
    DOCTEST_CHECK_EQ(formatTokenCount(190000), std::string("190,000"));
    DOCTEST_CHECK_EQ(formatTokenCount(205000), std::string("205,000"));
    DOCTEST_CHECK_EQ(formatTokenCount(1234567), std::string("1,234,567"));

    // 状态中文（未知态「—」）
    DOCTEST_CHECK_EQ(runStatusText("completed"), std::string("成功"));
    DOCTEST_CHECK_EQ(runStatusText("failed"), std::string("失败"));
    DOCTEST_CHECK_EQ(runStatusText("limited"), std::string("受限"));
    DOCTEST_CHECK_EQ(runStatusText("cancelled"), std::string("已取消"));
    DOCTEST_CHECK_EQ(runStatusText("running"), std::string("运行中"));
    DOCTEST_CHECK_EQ(runStatusText("whatever"), std::string("—"));
    return true;
})

REGISTER_TEST(agent_time_text_seven_day_window,
              "agent_time_text: startOfLocalDay / sevenDayWindowStart 跨月跨年不漂", {
    using namespace module::agent;
    const std::time_t now = epochOf(2026, 10, 18, 15, 0, 0);
    const std::time_t dayStart = startOfLocalDay(now);
    // 本日 00:00：日序号相同、钟点为 0
    {
        std::tm t{};
        core::time::localTime(dayStart, &t);
        DOCTEST_CHECK_EQ(t.tm_hour, 0);
        DOCTEST_CHECK_EQ(t.tm_min, 0);
        DOCTEST_CHECK_EQ(t.tm_sec, 0);
        DOCTEST_CHECK_EQ(core::time::localDayNumber(dayStart),
                         core::time::localDayNumber(now));
    }
    // 窗口起点 = 今天 00:00 往前 6 个日历日（跨月/跨年都按日序号差判定）
    const std::time_t win = sevenDayWindowStart(now);
    DOCTEST_CHECK_EQ(core::time::localDayNumber(win),
                     core::time::localDayNumber(now) - 6);
    // 跨年：1 月 3 日的窗口起点落在上一年 12 月 28 日（日序号差仍 = 6）
    const std::time_t jan = epochOf(2027, 1, 3, 12, 0, 0);
    DOCTEST_CHECK_EQ(core::time::localDayNumber(sevenDayWindowStart(jan)),
                     core::time::localDayNumber(jan) - 6);
    // 幂等：窗口起点自己的 startOfLocalDay = 自身（已是 00:00）
    DOCTEST_CHECK_EQ(startOfLocalDay(win), win);
    return true;
})

REGISTER_TEST(agent_time_text_message_time,
              "agent_time_text: 消息头时间——同日 HH:MM:SS / 跨日 MM-DD HH:MM:SS / 0 显「—」", {
    using namespace module::agent;
    // 参照 = 选中 run 的 startedAt（固定本地时刻）
    const std::time_t ref = epochOf(2026, 10, 18, 9, 30, 0);
    // 同一本地日（凌晨早于参照）：只给时钟
    const std::time_t sameDay = epochOf(2026, 10, 18, 0, 5, 7);
    DOCTEST_CHECK_EQ(messageTimeText(sameDay, ref), std::string("00:05:07"));
    // 同日午後：同上口径
    const std::time_t laterSameDay = epochOf(2026, 10, 18, 23, 59, 59);
    DOCTEST_CHECK_EQ(messageTimeText(laterSameDay, ref),
                     std::string("23:59:59"));
    // 跨日：带月日
    const std::time_t prevDay = epochOf(2026, 10, 17, 22, 1, 2);
    DOCTEST_CHECK_EQ(messageTimeText(prevDay, ref),
                     std::string("10-17 22:01:02"));
    // 参照为 0（无 run 上下文）：退全格式，不崩
    DOCTEST_CHECK_EQ(messageTimeText(prevDay, 0),
                     std::string("10-17 22:01:02"));
    // 旧库/异常行 createdAt=0：显「—」
    DOCTEST_CHECK_EQ(messageTimeText(0, ref), std::string("—"));
    return true;
})

REGISTER_TEST(agent_ui_next_run_semantics_pure,
              "agent_time_text: describeNextRun 手动/事件/常驻不造假时间 + 停用/异常显形", {
    using namespace module::agent;
    const std::time_t now = epochOf(2026, 10, 18, 15, 0, 0);

    // 手动：真话 = 不会自动运行
    {
        NextRunInfo info = describeNextRun(taskOf("manual", ""), nullptr, now);
        DOCTEST_CHECK_EQ(info.value, std::string("手动触发"));
        DOCTEST_CHECK_EQ(info.detail, std::string("不会自动运行"));
        DOCTEST_CHECK(info.tone == TextTone::Muted);
    }
    // 事件（带事件名）
    {
        NextRunInfo info = describeNextRun(
            taskOf("event", R"({"events":["todo.created"]})"), nullptr, now);
        DOCTEST_CHECK_EQ(info.value, std::string("等待事件"));
        DOCTEST_CHECK(info.detail.find("todo.created") != std::string::npos);
    }
    // 常驻
    {
        NextRunInfo info = describeNextRun(taskOf("resident", ""), nullptr, now);
        DOCTEST_CHECK_EQ(info.value, std::string("等待入队"));
    }
    // 停用优先（once 无 run 也停用）
    {
        NextRunInfo info =
            describeNextRun(taskOf("manual", "", false), nullptr, now);
        DOCTEST_CHECK_EQ(info.value, std::string("已停用"));
        DOCTEST_CHECK_EQ(info.detail, std::string("启用后才会自动运行"));
    }
    // 坏 cron：triggerConfigProblem（Scheduler 同源）先行显形「配置异常」，
    // 具体问题原文给副值——「cron 无效」分支只是 decode/parse 双保险
    {
        NextRunInfo info =
            describeNextRun(taskOf("schedule", R"({"cron":"0 9 * *"})"), nullptr,
                            now);
        DOCTEST_CHECK_EQ(info.value, std::string("配置异常"));
        DOCTEST_CHECK(info.tone == TextTone::Error);
    }
    // schedule 正常：主值含 09:00（今天或明天，不钉死哪天）
    {
        AgentTaskRow task = taskOf("schedule", R"({"cron":"0 9 * * *"})", true);
        NextRunInfo info = describeNextRun(task, nullptr, now);
        DOCTEST_CHECK(info.value.find("09:00") != std::string::npos);
        DOCTEST_CHECK(info.detail.find("定时") != std::string::npos);
        DOCTEST_CHECK(info.tone == TextTone::Normal);
    }
    // daily：今天没跑过 → 今天首次调度
    {
        NextRunInfo info = describeNextRun(taskOf("daily", ""), nullptr, now);
        DOCTEST_CHECK_EQ(info.value, std::string("今天首次调度"));
        DOCTEST_CHECK(info.detail.find("需应用保持运行") != std::string::npos);
    }
    // daily：今天已跑 → 明天首次调度
    {
        AgentRunRow latest;
        latest.id = 7;
        latest.status = "completed";
        latest.startedAt = dayAt(now, 0, 9, 0, 0);
        NextRunInfo info = describeNextRun(taskOf("daily", ""), &latest, now);
        DOCTEST_CHECK_EQ(info.value, std::string("明天首次调度"));
        DOCTEST_CHECK(info.detail.find("今天已运行") != std::string::npos);
    }
    // once：已错过不补跑（once_at 在过去 1 小时外）；已有 run 显示运行/结束语义
    {
        AgentTaskRow task =
            taskOf("once",
                   (R"({"once_at":)" + std::to_string(now - 86400) + "}").c_str(),
                   true);
        NextRunInfo info = describeNextRun(task, nullptr, now);
        DOCTEST_CHECK_MESSAGE(info.value == "已错过",
                              "once 已错过应显「已错过」，实测: " + info.value +
                                  " / " + info.detail);
        DOCTEST_CHECK(info.detail.find("应用当时未运行") != std::string::npos);
    }
    {
        // 未来 once_at：epochOf 造一个「明天 09:00」
        const std::time_t future = dayAt(now, 1, 9, 0, 0);
        AgentTaskRow task =
            taskOf("once", (R"({"once_at":)" + std::to_string(future) + "}").c_str(),
                   true);
        NextRunInfo info = describeNextRun(task, nullptr, now);
        DOCTEST_CHECK(info.value.find("09:00") != std::string::npos);
        DOCTEST_CHECK_EQ(info.detail, std::string("一次性任务"));
    }
    // once 已有 run：显示运行/结束语义（例外于停用优先；配置须合法——
    // 空配置在 triggerConfigProblem 就被拦成「配置异常」，到不了 run 语义）
    {
        const std::time_t past = now - 7200;
        AgentTaskRow task =
            taskOf("once", (R"({"once_at":)" + std::to_string(past) + "}").c_str(),
                   true);
        AgentRunRow latest;
        latest.id = 9;
        latest.status = "running";
        latest.startedAt = now - 60;
        NextRunInfo info = describeNextRun(task, &latest, now);
        DOCTEST_CHECK_MESSAGE(info.value == "运行中",
                              "实测: " + info.value + " / " + info.detail);
        DOCTEST_CHECK(info.detail.find("一次性任务 · 已触发") != std::string::npos);
    }
    {
        const std::time_t past = now - 7200;
        AgentTaskRow task =
            taskOf("once", (R"({"once_at":)" + std::to_string(past) + "}").c_str(),
                   true);
        AgentRunRow latest;
        latest.id = 9;
        latest.status = "completed";
        latest.startedAt = now - 3600;
        latest.endedAt = now - 3500;
        NextRunInfo info = describeNextRun(task, &latest, now);
        DOCTEST_CHECK_EQ(info.value, std::string("已结束"));
        DOCTEST_CHECK(info.detail.find("一次性任务 · 最近 ") == 0);
    }
    // 未知触发类型原样回显
    {
        NextRunInfo info = describeNextRun(taskOf("wat", ""), nullptr, now);
        DOCTEST_CHECK_EQ(info.value, std::string("未知触发"));
        DOCTEST_CHECK_EQ(info.detail, std::string("wat"));
    }
    return true;
})

REGISTER_TEST(agent_ui_run_plan_pure,
              "agent_time_text: describeRunPlan 按时/延迟/计划外/无对照/cron坏/提前", {
    using namespace module::agent;
    const std::time_t now = epochOf(2026, 10, 18, 15, 0, 0);
    // 计划 = 昨天 09:00 本地；实际相对它偏移
    const std::time_t plan = dayAt(now, -1, 9, 0, 0);
    auto scheduleTask = [] {
        return taskOf("schedule", R"({"cron":"0 9 * * *"})", true);
    };

    // 按时（+30s，含 startedAt 所在分钟的 09:00 计划）
    {
        AgentRunRow run;
        run.id = 1;
        run.status = "completed";
        run.startedAt = plan + 30;
        RunPlanInfo info = describeRunPlan(scheduleTask(), run, now);
        DOCTEST_CHECK(info.hasPlan);
        DOCTEST_CHECK_EQ(info.verdict, std::string("按时"));
        DOCTEST_CHECK_EQ(info.actual, formatLocalShortDateTime(run.startedAt));
        DOCTEST_CHECK(info.tone == TextTone::Normal);
    }
    // 延迟（+20min → ceil=20 分）
    {
        AgentRunRow run;
        run.id = 1;
        run.status = "completed";
        run.startedAt = plan + 20 * 60;
        RunPlanInfo info = describeRunPlan(scheduleTask(), run, now);
        DOCTEST_CHECK_EQ(info.verdict, std::string("延迟 20 分"));
        DOCTEST_CHECK(info.tone == TextTone::Warning);
    }
    // 计划外（+14h）
    {
        AgentRunRow run;
        run.id = 1;
        run.status = "completed";
        run.startedAt = plan + 14 * 3600;
        RunPlanInfo info = describeRunPlan(scheduleTask(), run, now);
        DOCTEST_CHECK_EQ(info.verdict, std::string("计划外启动"));
        DOCTEST_CHECK(info.tone == TextTone::Warning);
    }
    // 提前（schedule 的 previousCronMatch 只会回扫出 ≤ startedAt 的计划 →
    // delta 恒 ≥0，抢跑 10 分钟被判「计划外启动」；「提前启动」仅 once 可达）
    {
        AgentRunRow run;
        run.id = 1;
        run.status = "completed";
        run.startedAt = plan - 10 * 60;
        RunPlanInfo info = describeRunPlan(scheduleTask(), run, now);
        DOCTEST_CHECK_EQ(info.verdict, std::string("计划外启动"));
        DOCTEST_CHECK(info.tone == TextTone::Warning);
    }
    // manual：无计划对照
    {
        AgentRunRow run;
        run.id = 1;
        run.status = "completed";
        run.startedAt = now;
        RunPlanInfo info = describeRunPlan(taskOf("manual", ""), run, now);
        DOCTEST_CHECK(!info.hasPlan);
        DOCTEST_CHECK_EQ(info.plan, std::string("—"));
        DOCTEST_CHECK_EQ(info.verdict, std::string("无计划对照"));
    }
    // cron 坏：无法计算
    {
        AgentRunRow run;
        run.id = 1;
        run.status = "completed";
        run.startedAt = now;
        RunPlanInfo info = describeRunPlan(
            taskOf("schedule", R"({"cron":"61 * * * *"})", true), run, now);
        DOCTEST_CHECK(!info.hasPlan);
        DOCTEST_CHECK_EQ(info.verdict, std::string("无法计算计划时间"));
        DOCTEST_CHECK(info.tone == TextTone::Error);
    }
    // once：once_at 对照（按时）+ 缺 once_at
    {
        AgentRunRow run;
        run.id = 1;
        run.status = "completed";
        run.startedAt = plan + 45;
        AgentTaskRow task =
            taskOf("once", (R"({"once_at":)" + std::to_string(plan) + "}").c_str(),
                   true);
        RunPlanInfo info = describeRunPlan(task, run, now);
        DOCTEST_CHECK(info.hasPlan);
        DOCTEST_CHECK_EQ(info.verdict, std::string("按时"));
    }
    // once 提前启动（早于 once_at 10 分钟）：仅 once 路径可达的负差判定
    {
        AgentRunRow run;
        run.id = 1;
        run.status = "completed";
        run.startedAt = plan - 10 * 60;
        AgentTaskRow task =
            taskOf("once", (R"({"once_at":)" + std::to_string(plan) + "}").c_str(),
                   true);
        RunPlanInfo info = describeRunPlan(task, run, now);
        DOCTEST_CHECK(info.hasPlan);
        DOCTEST_CHECK_EQ(info.verdict, std::string("提前启动"));
        DOCTEST_CHECK(info.tone == TextTone::Warning);
    }
    {
        AgentRunRow run;
        run.id = 1;
        run.status = "completed";
        run.startedAt = now;
        RunPlanInfo info = describeRunPlan(taskOf("once", ""), run, now);
        DOCTEST_CHECK_EQ(info.verdict, std::string("缺少 once_at"));
        DOCTEST_CHECK(info.tone == TextTone::Error);
    }
    return true;
})
