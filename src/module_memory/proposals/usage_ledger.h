// src/module_memory/proposals/usage_ledger.h
// 消耗计量账本（S3-D）：collector/hook 调用后 token 累计 + 金额估算，累计落库。
// 单价常量来源：docs/superpowers/research/2026-10-27-memcpp-zhipu-endpoint-research.md
//（2026-10-27 抓取的智谱计费页：glm-5.3-flash 输入 0.8 元/百万 token、输出 2.8；
//  embedding-3 0.5）。金额粒度 0.001 元（整型毫厘），避免浮点累计漂移。
// 口径：token 为估算值（码点数 ≈ CJK 1 字 1 token）；账面供消耗页与预算硬档判定，
// 真 token 数以引擎侧返回为准（live 对账后接入，结构不动）。
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include "core/result.h"
#include "core/sqlite_db.h"

namespace module::memory::proposals {

// 单价常量（元 / 百万 token；调研文档定案，改动即契约变更）
inline constexpr double kLlmInputYuanPerMTok = 0.8;
inline constexpr double kLlmOutputYuanPerMTok = 2.8;
inline constexpr double kEmbedYuanPerMTok = 0.5;

// 码点计数（估算 token 的输入口径；与 hook 预算"字"同一定义）
std::size_t countCodePoints(const std::string& utf8);

// 金额估算（毫厘 = 0.001 元）。kind: llm_input / llm_output / embed
long long estimateCostMilliYuan(const std::string& kind, long long tokens);

// 计价 kind（与下面三个单价常量一一对应）。新增计价档 = 加常量 + 补 here 上方
// if 分支 + 补 test_memory_proposals 的价目断言，三处缺一即静默计 0。
inline constexpr const char* kKindLlmInput = "llm_input";
inline constexpr const char* kKindLlmOutput = "llm_output";
inline constexpr const char* kKindEmbed = "embed";

// 注入记账专用 kind 前缀：inject_* 行是 hook 通道预算账（面板注入预算环消费），
// 不是模型 token——恒被 todayTokens/monthTokens 与金额聚合排除（成本口径隔离）。
inline constexpr const char* kKindInjectItems = "inject_items";  // tokens 列存条数
inline constexpr const char* kKindInjectChars = "inject_chars";  // tokens 列存字数

// 云端提炼尝试 kind：每次 infer 出机尝试一行（成败都记），tokens 恒 0——
// 每小时节流计数器（spec 控耗 2）兼失败审计行；token/金额聚合恒不受影响。
inline constexpr const char* kKindLlmAttempt = "llm_attempt";

// 账面行（消耗页最近调用表 / 动态流的取数形态）
struct LedgerRow {
    std::int64_t ts = 0;
    std::string kind;
    long long tokens = 0;
    long long costMilliYuan = 0;
};

class UsageLedger {
public:
    static core::Result<UsageLedger> open(const std::filesystem::path& dbFile);

    // 记一笔（tokens<0 视为 0）；返回该笔估算毫厘
    core::Result<long long> record(const std::string& kind, long long tokens,
                                   std::int64_t nowSec);

    // 注入预算记账：一次注入 = 两行（条数 / 字数），金额恒 0。hook 子命令出
    // 口调用；预算环「今日已注入 N/5 条 · M/500 字」的唯一数据源。
    core::Result<void> recordInjection(int items, int chars,
                                       std::int64_t nowSec);
    core::Result<long long> todayInjectItems(std::int64_t nowSec) const;
    core::Result<long long> todayInjectChars(std::int64_t nowSec) const;

    // 今日 / 本月累计 token（按本地时区自然日/自然月；账面口径；不含 inject_* 行）
    core::Result<long long> todayTokens(std::int64_t nowSec) const;
    core::Result<long long> monthTokens(std::int64_t nowSec) const;

    // 今日 / 本月估算金额（毫厘；不含 inject_* 行——注入不调云端，无金额）
    core::Result<long long> todayCostMilliYuan(std::int64_t nowSec) const;
    core::Result<long long> monthCostMilliYuan(std::int64_t nowSec) const;

    // 提炼节流记账与计数（本地自然小时窗口，同日/月起点惯例）：attempt 失败也
    // 计数（attempts 口径），云端失败的重试天然受同一小时预算约束
    core::Result<void> recordAttempt(std::int64_t nowSec);
    core::Result<long long> hourAttempts(std::int64_t nowSec) const;

    // 最近账面行（ts 降序；消耗页最近调用表 + 主控台动态流取数）
    core::Result<std::vector<LedgerRow>> recentRows(int limit) const;

private:
    UsageLedger() = default;
    std::shared_ptr<class core::data::SqliteDb> db_;
};

}  // namespace module::memory::proposals
