// src/module_memory/proposals/curator.cpp
#include "curator.h"
#include <utility>

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
                const std::string want = std::to_string(m.id);
                for (const std::string& tid : p.targetIds) {
                    if (tid == want) hasPending = true;
                }
            }
        }
        if (hasPending) continue;
        auto card = store.create(
            "forget", {std::to_string(m.id)}, "", "", "devdesk",
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
    if (p.kind == "forget") {
        if (undo) {
            // 撤销遗忘（live 定案 2026-10-28）：引擎侧 delete_memory 不可逆且无
            // 「恢复」缝 → 按原卡证据正文重录入（undo 卡已透传原卡证据）
            if (p.evidence.empty() || p.evidence[0].quoted.empty()) {
                return core::Result<std::string>::fail(
                    core::ErrorCode::ValidationError,
                    "撤销遗忘缺原文证据（无法重录入）");
            }
            return engine.addDirect(p.evidence[0].quoted);
        }
        // 遗忘 = 引擎条目删除（search 无 forgotten 过滤缝，打标记不移出检索——
        // 1.10.1 唯一真实生效通道是 delete_memory）
        const auto r = engine.setMemoryForgotten(p.targetIds[0], true);
        if (r.isFail()) {
            return core::Result<std::string>::fail(r.errorCode(),
                                                   r.errorMessage());
        }
        return core::Result<std::string>::ok(std::string());
    }
    if (p.kind == "invalidate" && undo) {
        // 撤销新增 = 引擎条目删除（B4 回滚表「打失效标记」在 telemem 侧的
        // 真实通道；search 无标记过滤缝，打标记不生效——live 定案）
        const auto r = engine.setMemoryForgotten(p.targetIds[0], true);
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
