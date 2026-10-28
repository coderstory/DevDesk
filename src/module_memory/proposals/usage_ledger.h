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

class UsageLedger {
public:
    static core::Result<UsageLedger> open(const std::filesystem::path& dbFile);

    // 记一笔（tokens<0 视为 0）；返回该笔估算毫厘
    core::Result<long long> record(const std::string& kind, long long tokens,
                                   std::int64_t nowSec);

    // 今日 / 本月累计 token（按本地时区自然日/自然月；账面口径）
    core::Result<long long> todayTokens(std::int64_t nowSec) const;
    core::Result<long long> monthTokens(std::int64_t nowSec) const;

private:
    UsageLedger() = default;
    std::shared_ptr<class core::data::SqliteDb> db_;
};

}  // namespace module::memory::proposals
