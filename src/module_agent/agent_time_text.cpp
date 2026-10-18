// agent_time_text.h 的实现：全部纯函数，「现在」由调用方注入；时区转换只走
// core::time::localTime 与 std::mktime（本地日边界）。禁平台分支（spec §14）。
#include "module_agent/agent_time_text.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "core/time.h"
#include "module_agent/cron.h"
#include "module_agent/trigger_config.h"

namespace module::agent {
namespace {

// 本地 tm 的日序号（供今天/明天判定；daysFromCivil 纯算术，免 tm_yday 跨年坑）
int64_t tmDayNumber(const std::tm& local) {
    return core::time::daysFromCivil(local.tm_year + 1900,
                                     static_cast<unsigned>(local.tm_mon) + 1,
                                     static_cast<unsigned>(local.tm_mday));
}

// 本地 tm → epoch。mktime 依赖 tm_isdst：入参来自 localTime 时已填准；cron/计划
// 的匹配产物同样来自 localTime，原样透传（不强制 -1，避免 DST 歧义时选错边）
std::time_t tmToEpoch(const std::tm& local) {
    std::tm t = local;
    return std::mktime(&t);
}

// HH:MM（schedule 主值的时刻段）
std::string clockHM(const std::tm& local) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", local.tm_hour, local.tm_min);
    return buf;
}

} // namespace

std::string formatLocalDateTime(const std::tm& local) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                  local.tm_hour, local.tm_min, local.tm_sec);
    return buf;
}

std::string formatLocalShortDateTime(const std::tm& local) {
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%02d-%02d %02d:%02d:%02d",
                  local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min,
                  local.tm_sec);
    return buf;
}

std::string formatLocalDateTime(int64_t sec) {
    std::tm local{};
    core::time::localTime(static_cast<std::time_t>(sec), &local);
    return formatLocalDateTime(local);
}

std::string formatLocalShortDateTime(int64_t sec) {
    std::tm local{};
    core::time::localTime(static_cast<std::time_t>(sec), &local);
    return formatLocalShortDateTime(local);
}

std::string formatLocalClock(int64_t sec) {
    std::tm local{};
    core::time::localTime(static_cast<std::time_t>(sec), &local);
    char buf[12];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", local.tm_hour,
                  local.tm_min, local.tm_sec);
    return buf;
}

std::string formatLocalDate(int64_t sec) {
    std::tm local{};
    core::time::localTime(static_cast<std::time_t>(sec), &local);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", local.tm_year + 1900,
                  local.tm_mon + 1, local.tm_mday);
    return buf;
}

std::string formatScheduleTime(const std::tm& local, int64_t nowSec) {
    const int64_t diff = tmDayNumber(local) - core::time::localDayNumber(
                                                     static_cast<std::time_t>(nowSec));
    if (diff == 0) return "今天 " + clockHM(local);
    if (diff == 1) return "明天 " + clockHM(local);
    char buf[12];
    std::snprintf(buf, sizeof(buf), "%02d-%02d %02d:%02d", local.tm_mon + 1,
                  local.tm_mday, local.tm_hour, local.tm_min);
    return buf;
}

std::string formatScheduleTime(int64_t sec, int64_t nowSec) {
    std::tm local{};
    core::time::localTime(static_cast<std::time_t>(sec), &local);
    return formatScheduleTime(local, nowSec);
}

std::string localDayLabel(int64_t sec, int64_t nowSec) {
    const int64_t diff = core::time::localDayNumber(static_cast<std::time_t>(sec)) -
                         core::time::localDayNumber(static_cast<std::time_t>(nowSec));
    if (diff == 0) return "今天";
    if (diff == -1) return "昨天";
    return formatLocalDate(sec);
}

std::string relativeTimeText(int64_t sec, int64_t nowSec) {
    int64_t delta = nowSec - sec;
    if (delta < 0) delta = 0;   // 未来时刻不出现负相对时间（防御）
    if (delta < 60) return "刚刚";
    if (delta < 3600) return std::to_string(delta / 60) + " 分钟前";
    if (delta < 86400) return std::to_string(delta / 3600) + " 小时前";
    return std::to_string(delta / 86400) + " 天前";
}

std::string formatRunDurationText(int64_t seconds) {
    if (seconds < 0) seconds = 0;   // 时钟回拨防御：不显示负时长
    const int64_t days = seconds / 86400;
    const int64_t hours = (seconds % 86400) / 3600;
    const int64_t mins = (seconds % 3600) / 60;
    const int64_t secs = seconds % 60;
    if (days > 0) return std::to_string(days) + "天" + std::to_string(hours) + "时";
    if (hours > 0) {
        return std::to_string(hours) + "时" +
               (mins > 0 ? std::to_string(mins) + "分" : "");
    }
    if (mins > 0) {
        return std::to_string(mins) + "分" +
               (secs > 0 ? std::to_string(secs) + "秒" : "");
    }
    return std::to_string(secs) + "秒";
}

std::string formatTokenCount(int64_t tokens) {
    if (tokens < 0) return std::to_string(tokens);   // 不出现；防御不崩
    std::string digits = std::to_string(tokens);
    std::string out;
    out.reserve(digits.size() + digits.size() / 3);
    for (size_t i = 0; i < digits.size(); ++i) {
        out.push_back(digits[i]);
        const size_t remaining = digits.size() - 1 - i;
        if (remaining > 0 && remaining % 3 == 0) out.push_back(',');
    }
    return out;
}

std::string runStatusText(const std::string& status) {
    if (status == "completed") return "成功";
    if (status == "failed") return "失败";
    if (status == "limited") return "受限";
    if (status == "cancelled") return "已取消";
    if (status == "running") return "运行中";
    return "—";
}

std::time_t startOfLocalDay(std::time_t sec) {
    std::tm local{};
    core::time::localTime(sec, &local);
    local.tm_hour = 0;
    local.tm_min = 0;
    local.tm_sec = 0;
    local.tm_isdst = -1;   // 零点落在 DST 切换边界时让 mktime 决断
    return std::mktime(&local);
}

std::time_t sevenDayWindowStart(std::time_t nowSec) {
    std::tm local{};
    core::time::localTime(startOfLocalDay(nowSec), &local);
    local.tm_mday -= 6;    // 本地日历今天及前 6 天；mktime 归一化跨月/跨年
    local.tm_isdst = -1;
    return std::mktime(&local);
}

// ---- 下一次触发语义（spec §8.5 表逐行对齐；没有可预测下一次时不造假时间）----

NextRunInfo describeNextRun(const AgentTaskRow& task, const AgentRunRow* latest,
                            int64_t nowSec) {
    // disabled 优先；once 已有 run 是例外——它的「下一次」就是运行/结束语义本身
    if (!task.enabled &&
        !(task.triggerType == "once" && latest != nullptr)) {
        return {"已停用", "启用后才会自动运行", TextTone::Muted};
    }
    // 配置异常显形（具体问题原文给副值，error 色）——比任何语义都优先于派发侧
    const std::string problem =
        triggerConfigProblem(task.triggerType, task.triggerConfig);
    if (!problem.empty()) {
        return {"配置异常", problem, TextTone::Error};
    }

    if (task.triggerType == "schedule") {
        auto cfg = decodeTriggerConfig(task.triggerConfig);
        const char* cronExpr = cfg.isOk() ? cfg.value().cron.c_str() : "";
        auto expr = parseCron(cronExpr);
        if (expr.isFail()) {
            return {"cron 无效", expr.errorMessage(), TextTone::Error};
        }
        auto next = nextCronMatch(expr.value(), static_cast<std::time_t>(nowSec));
        if (next.isFail()) {
            return {"一年内无触发时间", next.errorMessage(), TextTone::Error};
        }
        const std::time_t nextEpoch = tmToEpoch(next.value());
        int64_t diff = static_cast<int64_t>(nextEpoch) - nowSec;
        if (diff < 0) diff = 0;
        std::string detail;
        if (diff < 3600) {
            detail = "定时 · 1 小时内";
        } else {
            detail = "定时 · " + std::to_string(diff / 3600) + " 小时后";
        }
        return {formatScheduleTime(next.value(), nowSec), detail, TextTone::Normal};
    }

    if (task.triggerType == "daily") {
        // 「今天跑过没」与 Scheduler::ranOnLocalDay 同口径（本地日序号比较）
        const bool ranToday =
            latest != nullptr &&
            core::time::localDayNumber(static_cast<std::time_t>(latest->startedAt)) ==
                core::time::localDayNumber(static_cast<std::time_t>(nowSec));
        if (ranToday) {
            return {"明天首次调度", "每日任务 · 今天已运行", TextTone::Normal};
        }
        return {"今天首次调度", "每日任务 · 需应用保持运行", TextTone::Normal};
    }

    if (task.triggerType == "once") {
        auto cfg = decodeTriggerConfig(task.triggerConfig);
        const std::optional<int64_t> onceAt =
            cfg.isOk() ? cfg.value().onceAt : std::nullopt;
        if (latest != nullptr) {
            if (latest->status == "running") {
                return {"运行中", "一次性任务 · 已触发", TextTone::Normal};
            }
            return {"已结束", "一次性任务 · 最近 " +
                                  formatLocalShortDateTime(latest->startedAt)
                                      .substr(0, 11),
                    TextTone::Muted};
        }
        if (!onceAt) {
            return {"配置异常", "一次性触发缺少 once_at", TextTone::Error};
        }
        if (nowSec >= *onceAt + 60) {
            return {"已错过",
                    formatLocalDateTime(*onceAt).substr(0, 16) + " · 应用当时未运行",
                    TextTone::Warning};
        }
        return {formatScheduleTime(*onceAt, nowSec), "一次性任务", TextTone::Normal};
    }

    if (task.triggerType == "manual") {
        return {"手动触发", "不会自动运行", TextTone::Muted};
    }
    if (task.triggerType == "event") {
        auto cfg = decodeTriggerConfig(task.triggerConfig);
        std::string detail = "事件触发";
        if (cfg.isOk() && !cfg.value().events.empty()) {
            detail += " · " + cfg.value().events.front();
            if (cfg.value().events.size() > 1) {
                detail += " 等 " + std::to_string(cfg.value().events.size()) + " 个事件";
            }
        }
        return {"等待事件", detail, TextTone::Normal};
    }
    if (task.triggerType == "resident") {
        return {"等待入队", "常驻任务 · 由队列触发", TextTone::Normal};
    }
    return {"未知触发", task.triggerType, TextTone::Error};
}

// ---- 计划 vs 实际（spec §8.4/§11.2；按时 ≤90s / 延迟 ≤30min / 计划外 >30min）----

RunPlanInfo describeRunPlan(const AgentTaskRow& task, const AgentRunRow& run,
                            int64_t nowSec) {
    (void)nowSec;
    RunPlanInfo info;
    info.plan = "—";       // 无计划对照的占位（spec §11.2 表）；有计划分支会覆写
    info.actual = formatLocalShortDateTime(run.startedAt);
    auto verdictForDelta = [](int64_t delta) -> RunPlanInfo {
        // delta = 实际 − 计划（秒）；≤90s 都算按时（含提前 90s 内的抢跑）
        if (delta <= 90) {
            return {true, {}, {}, "按时", TextTone::Normal};
        }
        if (delta <= 30 * 60) {
            const int64_t mins = std::max<int64_t>(1, (delta + 59) / 60);
            return {true, {}, {}, "延迟 " + std::to_string(mins) + " 分",
                    TextTone::Warning};
        }
        return {true, {}, {}, "计划外启动", TextTone::Warning};
    };

    if (task.triggerType == "schedule") {
        auto cfg = decodeTriggerConfig(task.triggerConfig);
        auto expr = parseCron(cfg.isOk() ? cfg.value().cron.c_str() : "");
        if (expr.isFail()) {
            info.verdict = "无法计算计划时间";
            info.tone = TextTone::Error;
            return info;
        }
        // 含 startedAt 所在分钟：run 在 09:00:30 启动，0 9 * * * 应算 09:00 计划
        auto plan = previousCronMatch(expr.value(),
                                      static_cast<std::time_t>(run.startedAt));
        if (plan.isFail()) {
            info.verdict = "无计划对照";
            info.tone = TextTone::Muted;
            return info;
        }
        info.hasPlan = true;
        info.plan = formatScheduleTime(plan.value(), run.startedAt);
        // previousCronMatch 只回扫出 ≤ startedAt 的计划 → delta ≥ 0：
        // 抢跑（早于当日计划）天然落入「计划外启动」，无需负差分支
        const int64_t delta =
            run.startedAt - static_cast<int64_t>(tmToEpoch(plan.value()));
        RunPlanInfo v = verdictForDelta(delta);
        v.hasPlan = info.hasPlan;
        v.plan = info.plan;
        v.actual = info.actual;
        return v;
    }

    if (task.triggerType == "once") {
        auto cfg = decodeTriggerConfig(task.triggerConfig);
        if (!cfg.isOk() || !cfg.value().onceAt) {
            info.verdict = "缺少 once_at";
            info.tone = TextTone::Error;
            return info;
        }
        info.hasPlan = true;
        info.plan = formatScheduleTime(*cfg.value().onceAt, run.startedAt);
        const int64_t delta = run.startedAt - *cfg.value().onceAt;
        RunPlanInfo v = verdictForDelta(delta);
        if (delta < -90) {
            v.verdict = "提前启动";
            v.tone = TextTone::Warning;
        }
        v.hasPlan = info.hasPlan;
        v.plan = info.plan;
        v.actual = info.actual;
        return v;
    }

    info.verdict = "无计划对照";   // daily / manual / event / resident
    info.tone = TextTone::Muted;
    return info;
}

} // namespace module::agent
