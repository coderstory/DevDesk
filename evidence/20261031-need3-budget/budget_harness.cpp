// 池3实测：TokenUsageMeter 预算档位判定实证（纯函数，header-only）
// 编译: c++ -std=c++20 -I /Users/coderstory/dev/DevDesk/src budget_harness.cpp -o budget_harness
#include "/Users/coderstory/dev/DevDesk/src/module_memory/engine/token_usage.h"
#include <cstdio>

using module::memory::engine::TokenUsageMeter;

static int fails = 0;
#define CHECK(cond, msg)                                  \
    do {                                                  \
        if (cond) {                                       \
            std::printf("  PASS %s\n", msg);              \
        } else {                                          \
            std::printf("  FAIL %s\n", msg);              \
            ++fails;                                      \
        }                                                 \
    } while (0)

int main() {
    std::printf("[F-05] TokenUsageMeter 档位判定（默认软 2000000 / 硬 3000000）\n");
    {
        TokenUsageMeter m;
        CHECK(m.monthTokens == 0 && m.phase() == TokenUsageMeter::Phase::Normal,
              "月账面 0 -> Normal");
    }
    {
        TokenUsageMeter m;
        m.monthTokens = 1999999;
        CHECK(m.phase() == TokenUsageMeter::Phase::Normal, "1999999 -> Normal");
        m.monthTokens = 2000000;
        CHECK(m.phase() == TokenUsageMeter::Phase::Soft, "2000000(恰等软阈值) -> Soft");
        m.monthTokens = 2999999;
        CHECK(m.phase() == TokenUsageMeter::Phase::Soft, "2999999 -> Soft");
        m.monthTokens = 3000000;
        CHECK(m.phase() == TokenUsageMeter::Phase::Hard, "3000000(恰等硬阈值) -> Hard");
        m.monthTokens = 99999999;
        CHECK(m.phase() == TokenUsageMeter::Phase::Hard, "远超硬阈值 -> Hard");
    }
    {
        TokenUsageMeter m;
        m.monthTokens = 1500000;
        CHECK(m.monthRatio() > 0.4999 && m.monthRatio() < 0.5001, "ratio 1.5M/3M = 0.5");
        m.monthTokens = 4000000;
        CHECK(m.monthRatio() == 1.0, "超硬档 ratio 截到 1.0");
        m.monthTokens = 0;
        CHECK(m.monthRatio() == 0.0, "账面 0 ratio = 0");
        m.hardBudgetTokens = 0;
        CHECK(m.monthRatio() == 0.0, "硬阈值<=0 异常配置 ratio 恒 0（不产生 NaN）");
    }
    std::printf("[F-05] 结果: %s（fails=%d）\n", fails == 0 ? "全绿" : "有红", fails);
    return fails == 0 ? 0 : 1;
}
