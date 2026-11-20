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

// 定时规则字段 → 人话指引。未知字段返回兜底话术（绝不回显内部名）。
inline std::string scheduleFieldHint(const std::string& fieldName) {
    if (fieldName == "once.at" || fieldName == "once.at_local") {
        return "执行时间格式不对，请按 2026-11-21 09:30 这样的格式填写";
    }
    if (fieldName == "calendar.unit") {
        return "重复周期填错了，请选择 天 / 周 / 月";
    }
    if (fieldName == "calendar.workdays_only") {
        return "工作日设置无效，请至少勾选一天";
    }
    if (fieldName == "interval") {
        return "间隔设置无效，请填写大于 0 的分钟数";
    }
    return "规则配置有误，请检查后重试";
}

} // namespace core::user_message
