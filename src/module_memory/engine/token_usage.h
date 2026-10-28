// src/module_memory/engine/token_usage.h
// 消耗页 token 计量表结构（S2 骨架：结构 + 占位数；账面数据源在采集/提炼批次接入，
// 届时只替换取数通道，结构不动）。
#pragma once

namespace module::memory::engine {

// 月度 token 预算（消耗控制第 3 条）：软档 = 黄条提醒；硬档 = 暂停提炼（检索不受影响）。
struct TokenUsageMeter {
    long long todayTokens = 0;       // 今日累计（引擎账面）
    long long monthTokens = 0;       // 本月累计
    long long softBudgetTokens = 2000000;   // 软档阈值（占位默认，配置页可调后续批次接）
    long long hardBudgetTokens = 3000000;   // 硬档阈值

    enum class Phase { Normal, Soft, Hard };

    Phase phase() const {
        if (monthTokens >= hardBudgetTokens) return Phase::Hard;
        if (monthTokens >= softBudgetTokens) return Phase::Soft;
        return Phase::Normal;
    }

    // 仪表条占比 [0,1]：以硬档为满刻度；异常配置（硬<=0）恒 0，不产生 NaN/负宽
    double monthRatio() const {
        if (hardBudgetTokens <= 0) return 0.0;
        if (monthTokens <= 0) return 0.0;
        const double r = static_cast<double>(monthTokens) /
                         static_cast<double>(hardBudgetTokens);
        return r < 1.0 ? r : 1.0;
    }
};

} // namespace module::memory::engine
