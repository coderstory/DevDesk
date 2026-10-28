// cron 5 字段「分 时 日 月 周」语法：支持 * , - /；周 0-6（0=周日）；
// 不支持 Quartz 扩展（L/W/#）与英文名称。纯函数、无 IO 无时钟：parse 保存时校验，matches 在 tick 时判定。
#pragma once
#include <array>
#include <ctime>      // std::tm（cronMatches 形参）
#include <string>
#include "result.h"

namespace core::cron {
struct CronExpr {
    std::array<bool, 60> minutes{};   // [0..59]
    std::array<bool, 24> hours{};     // [0..23]
    std::array<bool, 32> doms{};      // [1..31]，[0] 恒 false
    std::array<bool, 13> months{};    // [1..12]，[0] 恒 false
    std::array<bool, 8> dows{};       // [0..6]，0=周日
    bool domRestricted = false;       // 日字段非 *
    bool dowRestricted = false;       // 周字段非 *
};

// 非法（字段数≠5 / 越界 / 空字段 / 逆序区间 / 步长 0 / 非数字 / L W # / 英文）→ fail(ValidationError)
core::Result<CronExpr> parseCron(const std::string& expr);

// 纯匹配：只读 tm 的 min/hour/mday/mon/wday（wday 由调用方填，不从日期推导）。
// 日/周同时受限 → vixie「或」；仅一方受限 → 该方必须命中
bool cronMatches(const CronExpr& c, const std::tm& local);

// fromSec 起的下一个命中时刻（本地墙钟 std::tm，wday 已填）：从 fromSec 的下一
// 分钟边界起逐分钟前探（与 Scheduler「分钟翻转即触发」同粒度，fromSec 所在分钟
// 不参与）；硬上限 366*1440 步，到顶 fail(ValidationError, "一年内无触发时间")。
// 纯函数：不读真时钟，fromSec 全由调用方注入；仅时区转换经 core::time::localTime
// （逐分钟调用，语义与 Scheduler 逐 tick 判定一致，DST 缺失的小时同样被跳过）。
core::Result<std::tm> nextCronMatch(const CronExpr& c, std::time_t fromSec);

// fromSec 所在分钟（含）起向前逐分钟回扫的最近一次命中（「计划 vs 实际」对照用，
// spec §8.4）：run 在 09:00:30 启动、cron `0 9 * * *` 应算 09:00 计划——**包含
// fromSec 所在分钟**，与 nextCronMatch 的边界相反。上限 366*1440 分钟，到顶
// fail(ValidationError, "一年内无匹配时间")。纯函数：fromSec 由调用方注入；
// 时区转换只走 core::time::localTime。
core::Result<std::tm> previousCronMatch(const CronExpr& c, std::time_t fromSec);
} // namespace core::cron
