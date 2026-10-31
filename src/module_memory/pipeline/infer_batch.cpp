// src/module_memory/pipeline/infer_batch.cpp
#include "infer_batch.h"

#include <ctime>

#include "module_memory/engine/token_usage.h"

namespace module::memory::pipeline {

InferGate inferGateDecision(long long monthTokens, long long hardBudgetTokens,
                            long long hourAttempts, long long hourlyLimit) {
    if (hardBudgetTokens > 0) {
        engine::TokenUsageMeter meter;
        meter.monthTokens = monthTokens;
        meter.hardBudgetTokens = hardBudgetTokens;
        if (meter.phase() == engine::TokenUsageMeter::Phase::Hard) {
            return InferGate::Paused;
        }
    }
    if (hourlyLimit > 0 && hourAttempts >= hourlyLimit) {
        return InferGate::Throttled;
    }
    return InferGate::Proceed;
}

core::Result<InferStepReport> runInferStep(
    InferQueueStore& queue, gateway::IMemoryRetriever& engine,
    proposals::UsageLedger& ledger, const InferLimits& limits,
    const std::function<void(const RoundNote&)>& onNote) {
    InferStepReport report;
    const auto now = static_cast<std::int64_t>(std::time(nullptr));
    const auto num = [](core::Result<long long> r) {
        return r.isOk() ? r.value() : 0;
    };
    const InferGate gate =
        inferGateDecision(num(ledger.monthTokens(now)),
                          limits.hardBudgetTokens,
                          num(ledger.hourAttempts(now)), limits.hourlyLimit);
    if (gate != InferGate::Proceed) {
        report.gateBlocked = true;
        report.paused = gate == InferGate::Paused;
        return core::Result<InferStepReport>::ok(report);
    }
    auto batch = queue.listBatch(1);
    if (batch.isFail()) {
        return core::Result<InferStepReport>::fail(batch.errorCode(),
                                                   batch.errorMessage());
    }
    if (batch.value().empty()) {
        return core::Result<InferStepReport>::ok(report);
    }
    const auto entry = batch.value().front();
    // attempts 口径：出机前记账，成败都算——失败重试受同一小时上限约束
    if (auto a = ledger.recordAttempt(now); a.isFail()) {
        return core::Result<InferStepReport>::fail(a.errorCode(),
                                                   a.errorMessage());
    }
    report.attempted = true;
    auto r = engine.addInferred(entry.text);
    if (r.isFail()) {
        if (onNote) {
            onNote(RoundNote{true, entry.sessionId,
                             "提炼 · 云端失败暂存待补跑（" + r.errorMessage() +
                                 "）"});
        }
        return core::Result<InferStepReport>::ok(report);  // 留队，下轮补跑
    }
    // 估算记账：输入 = 码点数（账本既有口径）；add 响应无 usage/id 回传，输出
    // 按输入等额估算（输出单价高于输入，宁高勿低）
    const long long tokens =
        static_cast<long long>(proposals::countCodePoints(entry.text));
    if (auto in = ledger.record("llm_input", tokens, now); in.isFail()) {
        return core::Result<InferStepReport>::fail(in.errorCode(),
                                                   in.errorMessage());
    }
    if (auto out = ledger.record("llm_output", tokens, now); out.isFail()) {
        return core::Result<InferStepReport>::fail(out.errorCode(),
                                                   out.errorMessage());
    }
    if (auto rm = queue.remove(entry.id); rm.isFail()) {
        return core::Result<InferStepReport>::fail(rm.errorCode(),
                                                   rm.errorMessage());
    }
    report.succeeded = true;
    // 成功不 note：动态流由 llm_input/llm_output 账面行承载（消耗口径可见）
    return core::Result<InferStepReport>::ok(report);
}

}  // namespace module::memory::pipeline
