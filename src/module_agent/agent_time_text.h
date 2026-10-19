// 详情弹窗 P2 的时间/文案纯 helper（spec 2026-10-18 §11）：UI 与测试共用，
// 无 IO 无时钟——「现在」一律由调用方注入 nowSec。时间转换只走 core::time。
#pragma once

#include <cstdint>
#include <ctime>
#include <string>

#include "module_agent/agent_store.h"

namespace module::agent {

// ---- 绝对时间格式化（本地时区）----
std::string formatLocalDateTime(int64_t sec);        // YYYY-MM-DD HH:MM:SS
std::string formatLocalShortDateTime(int64_t sec);   // MM-DD HH:MM:SS
std::string formatLocalClock(int64_t sec);           // HH:MM:SS
std::string formatLocalDate(int64_t sec);            // YYYY-MM-DD

// cron/计划匹配返回的是本地 tm（无 epoch）：直接格式化，不经 mktime 二次换算
//（避开 DST 缺失小时的往返陷阱）
std::string formatLocalDateTime(const std::tm& local);
std::string formatLocalShortDateTime(const std::tm& local);

// 今天 09:00 / 明天 09:00 / MM-DD HH:MM（跨天判断用 localDayNumber，不用 tm_yday）
std::string formatScheduleTime(int64_t sec, int64_t nowSec);
std::string formatScheduleTime(const std::tm& local, int64_t nowSec);

// 时间线日期分组头：今天 / 昨天 / YYYY-MM-DD
std::string localDayLabel(int64_t sec, int64_t nowSec);

// 刚刚 / N 分钟前 / N 小时前 / N 天前（只做辅助展示，不替代绝对时间）
std::string relativeTimeText(int64_t sec, int64_t nowSec);

// 消息头时间（spec §8.3）：createdAt<=0 → 「—」（旧库异常行不崩）；
// 与参照时刻（选中 run 的 startedAt）同一本地日 → HH:MM:SS，跨日 → MM-DD HH:MM:SS
std::string messageTimeText(int64_t createdAtSec, int64_t refSec);

// 时长秒 → 紧凑中文（0秒 / 45秒 / 2分13秒 / 1时23分 / 2天3时；负值按 0）
std::string formatRunDurationText(int64_t seconds);

// token 千分位：190000 → 「190,000」；0 → 「0」；负值原样（不出现，防御）
std::string formatTokenCount(int64_t tokens);

// run 状态 → 中文展示（completed=成功 / failed=失败 / limited=受限 /
// cancelled=已取消 / running=运行中；未知态「—」）
std::string runStatusText(const std::string& status);

// 本地日 00:00 的 epoch；sevenDayWindowStart = 本地今天 00:00 往前推 6 个日历日
//（tm_mday -= 6 后 mktime，DST 日按 86400 硬减会漂 ±1 小时）
std::time_t startOfLocalDay(std::time_t sec);
std::time_t sevenDayWindowStart(std::time_t nowSec);

// 文案色调（UI 侧映射主题 token；结构体不携带颜色）
enum class TextTone { Normal, Muted, Warning, Error };

// 「下一次」卡片的真话（spec §8.5）：没有可预测下一次时不造假时间
struct NextRunInfo {
    std::string value;    // 主值（具体时间 / 语义文案）
    std::string detail;   // 副值（来源说明）
    TextTone tone = TextTone::Normal;   // UI: Normal→textPrimary / Muted→textTertiary /
                                        // Warning→warning / Error→error
};

// 「计划 vs 实际」对照（spec §8.4/§11.2）：只对选中 run 计算（启发式，
// 手动启动 schedule 任务可能被判「延迟/计划外」——§15 R5 已登记）
struct RunPlanInfo {
    bool hasPlan = false;
    std::string plan;      // MM-DD HH:MM 或「—」
    std::string actual;    // MM-DD HH:MM:SS（= run 开始时刻）
    std::string verdict;   // 按时 / 延迟 N 分 / 计划外启动 / 提前启动 /
                           // 无计划对照 / 无法计算计划时间 / 缺少 once_at
    TextTone tone = TextTone::Muted;   // UI: Normal→success（按时）/ Warning→warning /
                                       // Error→error / Muted→textTertiary
};

// 下一次触发语义。latest = 该任务最近一次 run（可为 nullptr = 从未运行）。
// 判定顺序：disabled（once 已有 run 例外，显示运行/结束语义）→ 配置异常 →
// 按触发类型（schedule/daily/once/manual/event/resident/未知）。
NextRunInfo describeNextRun(const AgentTaskRow& task, const AgentRunRow* latest,
                            int64_t nowSec);

// 选中 run 的计划对照。schedule 走 previousCronMatch（含 startedAt 所在分钟）；
// once 用 once_at 直接比较；daily/manual/event/resident 无计划对照。
RunPlanInfo describeRunPlan(const AgentTaskRow& task, const AgentRunRow& run,
                            int64_t nowSec);

} // namespace module::agent
