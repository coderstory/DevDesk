// src/module_memory/proposals/curator.cpp
#include "curator.h"

#include "module_memory/gateway/telemem_client.h"

namespace module::memory::proposals {

namespace gateway = ::module::memory::gateway;

core::Result<Proposal> Curator::proposeAdd(
    ProposalStore& store, const std::string& text,
    const std::vector<ProposalEvidence>& evidence, double confidence,
    std::int64_t nowSec) {
    if (text.empty()) {
        return core::Result<Proposal>::fail(core::ErrorCode::ValidationError,
                                            "新增提案正文为空");
    }
    // add 类无目标 → baseVersion 恒 -1（无前置校验对象）
    return store.create("add", {}, text, "fact", "devdesk", evidence,
                        confidence, "collect", "assistant_derived", nowSec,
                        -1);
}

core::Result<std::vector<std::string>> Curator::proposeForget(
    ProposalStore& store, const MemoryListFn& memories, int idleThresholdDays,
    std::int64_t nowSec) {
    auto all = memories();
    if (all.isFail()) {
        return core::Result<std::vector<std::string>>::fail(all.errorCode(),
                                                            all.errorMessage());
    }
    const std::int64_t thresholdSec =
        static_cast<std::int64_t>(idleThresholdDays) * 86400;
    std::vector<std::string> created;
    for (const MemorySummary& m : all.value()) {
        if (m.lastUsedAt <= 0 || nowSec - m.lastUsedAt <= thresholdSec) {
            continue;  // 未闲置到阈值（无使用记录的不动，保守优先）
        }
        // 目标已有未决卡 → 不重复建（forget 目标唯一性由调用侧去重兜底）
        bool hasPending = false;
        auto pending = store.listPending();
        if (pending.isOk()) {
            for (const auto& p : pending.value()) {
                if (p.kind != "forget") continue;
                for (std::int64_t tid : p.targetIds) {
                    if (tid == m.id) hasPending = true;
                }
            }
        }
        if (hasPending) continue;
        auto card = store.create(
            "forget", {m.id}, "", "", "devdesk",
            {ProposalEvidence{"", 0, m.text, nowSec}},
            kHighConfidence, "decay",
            "assistant_derived", nowSec, m.version);
        if (card.isOk()) {
            created.push_back(card.value().id);
        }
    }
    return core::Result<std::vector<std::string>>::ok(std::move(created));
}

core::Result<std::string> executeProposal(
    const Proposal& p, gateway::IMemoryRetriever& engine) {
    if (p.kind == "add" && p.undoOf.empty()) {
        return engine.addDirect(p.resultText);
    }
    // 标记翻转类：需要恰好一个目标
    if (p.targetIds.size() != 1) {
        return core::Result<std::string>::fail(
            core::ErrorCode::ValidationError,
            "标记类提案需要恰好一个目标，当前 " +
                std::to_string(p.targetIds.size()));
    }
    const bool undo = !p.undoOf.empty();
    const bool forgotten = !undo;  // forget 建=打标；forget 撤销=清标
    if (p.kind == "forget") {
        const auto r = engine.setMemoryForgotten(
            std::to_string(p.targetIds[0]), forgotten);
        if (r.isFail()) return core::Result<std::string>::fail(r.errorCode(),
                                                               r.errorMessage());
        return core::Result<std::string>::ok(std::string());
    }
    if (p.kind == "invalidate" && undo) {
        // 撤销新增 = 给新增条目打失效标记（不删行；B4 §1.3 回滚表）
        const auto r = engine.setMemoryForgotten(
            std::to_string(p.targetIds[0]), true);
        if (r.isFail()) {
            return core::Result<std::string>::fail(r.errorCode(),
                                                   r.errorMessage());
        }
        return core::Result<std::string>::ok(std::string());
    }
    return core::Result<std::string>::fail(
        core::ErrorCode::NotImplemented,
        "首版执行器只支持 add/forget/撤销类: " + p.kind);
}

}  // namespace module::memory::proposals
