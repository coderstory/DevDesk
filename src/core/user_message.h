// 用户可见错误文案层（bug361）：把内部字段名翻译成人话。
// 底层 errorMessage() 保留技术细节供日志；UI 展示前过本层。
#pragma once

#include <string>

namespace core::user_message {

// Claude Profile 列表错误态（bug369）：内部 errorMessage() 可能带类名
//（如「ClaudeService 未注册」）或 sqlite 技术串，UI 展示前一律过本层换人话。
// 两类错误恢复路径不同，故拆两个函数，由调用方按 svc 是否注册选择。
// 只依赖 std：core 层不得依赖 platform / EUI 运行时（AGENTS.md §六）。
inline bool claudeErrorNeedsRestart(const std::string& rawError) {
    return rawError.find("ClaudeService 未注册") != std::string::npos ||
           rawError.find("服务未就绪") != std::string::npos;
}

inline std::string claudeProfileErrorText(const std::string& rawError) {
    if (claudeErrorNeedsRestart(rawError)) {
        return "Claude 代理服务未启动，需要重启应用才能恢复";
    }
    return "Profile 列表加载失败，请点「重试」重新加载";
}

// 引导句：重启类给关闭重开指引；可重试类给失败排查指引。
inline std::string claudeProfileErrorGuide(const std::string& rawError) {
    if (claudeErrorNeedsRestart(rawError)) {
        return "请关闭 DevDesk 后重新打开，再进入本页";
    }
    return "若反复失败，请查看日志确认数据库是否可读写";
}

// 定时规则字段 → 人话指引。键 = spec_json 里的字段路径（嵌套用点号），组级键
//（once / interval / calendar / cron / event / resident）收组内跨字段错误。
// 契约：本层只产人话；技术细节由调用方 LOG_WARN 留在日志，不进返回值。
// 未知键返回兜底话术（绝不回显内部名）。
inline std::string scheduleFieldHint(const std::string& fieldName) {
    if (fieldName == "once") {
        return "执行时间设置有误，请重新选择执行时间";
    }
    if (fieldName == "once.at" || fieldName == "once.at_local") {
        return "执行时间格式不对，请按 2026-11-21 09:30 这样的格式填写";
    }
    if (fieldName == "interval") {
        return "间隔设置无效，请填写大于 0 的秒数";
    }
    if (fieldName == "interval.every_sec") {
        return "间隔秒数必须填大于 0 的整数";
    }
    if (fieldName == "interval.anchor") {
        return "间隔的起始时间无效，请重新设置";
    }
    if (fieldName == "interval.window") {
        return "执行时间窗口无效，请把窗口时长设为小于间隔秒数";
    }
    if (fieldName == "calendar") {
        return "重复规则设置有误，请检查周期与日期条件是否搭配";
    }
    if (fieldName == "calendar.unit") {
        return "重复周期填错了，请选择 天 / 周 / 月 / 年";
    }
    if (fieldName == "calendar.interval") {
        return "重复间隔必须填大于 0 的整数";
    }
    if (fieldName == "calendar.workdays_only") {
        return "工作日设置无效，请至少勾选一天";
    }
    if (fieldName == "calendar.weekdays") {
        return "星期选择无效，请至少选择一个星期";
    }
    if (fieldName == "calendar.month_day") {
        return "每月执行日期填错了，请填写 1 到 31 之间的日期";
    }
    if (fieldName == "calendar.nth_weekday") {
        return "「第几个星期几」的设置无效，请重新选择";
    }
    if (fieldName == "calendar.nth_weekday.n") {
        return "「第几个」必须填 1 到 5，请重新选择";
    }
    if (fieldName == "calendar.nth_weekday.weekday") {
        return "星期几的选择无效，请重新选择";
    }
    if (fieldName == "calendar.last_workday") {
        return "「每月最后一个工作日」的设置无效，请重新勾选";
    }
    if (fieldName == "calendar.time") {
        return "执行时刻格式不对，请按 09:30 这样的格式填写";
    }
    if (fieldName == "calendar.anchor_date") {
        return "基准日期无效，请填写真实存在的日期（如 2026-11-21），并让它落在已选的星期里";
    }
    if (fieldName == "cron") {
        return "定时规则设置有误，请重新填写表达式";
    }
    if (fieldName == "cron.expr") {
        return "定时表达式无效，请检查后重新填写";
    }
    if (fieldName == "event") {
        return "事件规则设置有误，请重新选择触发事件";
    }
    if (fieldName == "event.events") {
        return "触发事件无效，请重新选择事件";
    }
    if (fieldName == "event.cooldown_sec") {
        return "冷却时间必须填大于等于 0 的秒数";
    }
    if (fieldName == "resident") {
        return "常驻规则不接受额外设置，请检查后重试";
    }
    return "规则配置有误，请检查后重试";
}

} // namespace core::user_message
