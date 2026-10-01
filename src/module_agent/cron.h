// src/module_agent/cron.h —— spec §5.1 R4 语法：5 字段「分 时 日 月 周」，
// 支持 * , - /；周 0-6（0=周日）；不支持 Quartz 扩展（L/W/#）与英文名称。
// 纯函数、无 IO 无时钟：parse 在任务保存时校验，matches 在 tick 时判定。
#pragma once
#include <array>
#include <ctime>      // std::tm（cronMatches 形参；brief 片段漏 include，卫生补齐）
#include <string>
#include "core/result.h"

namespace module::agent {
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

// 纯匹配：只读 tm 的 tm_min/tm_hour/tm_mday/tm_mon/tm_wday（tm_wday 由调用方
// 经 core::time::localTime 填充，不从日期推导）。日/周同时受限时按 vixie-cron
// 标准取「或」：命中任一即当日命中；仅一方受限则该方必须命中。
bool cronMatches(const CronExpr& c, const std::tm& local);
} // namespace module::agent
