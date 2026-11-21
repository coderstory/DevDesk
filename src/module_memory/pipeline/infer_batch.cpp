// src/module_memory/pipeline/infer_batch.cpp
#include "infer_batch.h"

#include <ctime>
#include <string>
#include <utility>

#include "core/logging.h"
#include "module_memory/engine/token_usage.h"
#include "module_memory/hook/inject.h"

namespace module::memory::pipeline {

namespace {

// 需-3 路②清单差分窗口（细则 §3：get_memories limit 200，现有缝）；
// 超时同 gateway 既有清单口径
constexpr int kInferDiffListLimit = 200;
constexpr int kInferListTimeoutMs = 10000;

// OQ-2（老板 2026-11-18 裁决）：修饰重试耗尽 → 出队落兜底人审，不再无限
// 留队——池 #333「只进不出」根因的队列侧修复。5 = 保守缺省（OQ-5 口径：
// 无实测依据前不拍大，后续按真数据集校准）；节流档下一轮 30s，耗尽最快
// 5 个节拍窗口
constexpr int kInferMaxAttempts = 5;

// OQ-5：低置信兜底阈值，沿用 collect 侧未评估置信 0.3 既有值
//（collect_round.cpp:36 / infer_queue.h kQueueDefaultConfidence 同源镜像）
constexpr double kLowConfidenceFallback = 0.3;

// 需-3 路②补标：add 前快照 → 差分新条目 → 无合法前缀的补卡侧前缀。
// 尽力而为：快照/补标失败只 LOG，不影响提炼主流程（未分类是细则 §1.3 合法态）。
// 腿 A 扩展：差分产物 id/text 全量出参（终稿卡 targetIds/resultText 来源——
// 撤销链锚点）；返回差分新条目数；清单不可得返回 -1（≠零产物，不计
// productMissing）。
int tagInferredProducts(
    gateway::IMemoryRetriever& engine, const hook::MemoryPrefix& cardPrefix,
    const std::vector<std::string>& beforeIds,
    std::vector<std::string>* outIds, std::vector<std::string>* outTexts) {
    auto after =
        engine.listAll(kInferDiffListLimit, kInferListTimeoutMs);
    if (after.isFail()) {
        LOG_WARN("提炼补标跳过（清单不可得）: " + after.errorMessage());
        return -1;
    }
    int freshItems = 0;
    for (const gateway::MemoryListItem& item : after.value()) {
        if (item.id.empty()) continue;
        bool known = false;
        for (const std::string& id : beforeIds) {
            if (id == item.id) {
                known = true;
                break;
            }
        }
        if (known) continue;  // 差分：只看本次提炼新增
        ++freshItems;
        if (outIds != nullptr) outIds->push_back(item.id);
        if (outTexts != nullptr) outTexts->push_back(item.text);
        if (hook::parseMemoryPrefix(item.text).valid) continue;  // 已带合法前缀
        if (!cardPrefix.valid) continue;  // 卡未定类型 → 不补（细则 §1.3 合法态）
        auto upd = engine.updateMemoryText(
            item.id, cardPrefix.tag + " " + item.text);
        if (upd.isFail()) {
            LOG_WARN("提炼补标失败（条目 " + item.id + "）: " +
                     upd.errorMessage());
        } else if (outTexts != nullptr) {
            // 补标后以补标文为准（卡面与引擎侧一致）；补标失败带差分原文
            outTexts->back() = cardPrefix.tag + " " + item.text;
        }
    }
    return freshItems;
}

}  // namespace

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
    proposals::ProposalStore& store, proposals::UsageLedger& ledger,
    const InferLimits& limits,
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
        return core::propagate<InferStepReport>(batch);
    }
    if (batch.value().empty()) {
        return core::Result<InferStepReport>::ok(report);
    }
    const auto entry = batch.value().front();
    // OQ-2 低置信前置分流：入队置信 < 0.3（OQ-5 既有值）不出机（零云端
    // 成本），直接出队落兜底 pending 卡待人审；collect 缺省 0.3 恰不触发
    //（严格小于）。出队序：先落卡再删队——卡失败时条目留队可重试（代价：
    // remove 失败的极小窗口可能重复落卡，LOG 兜底）
    if (entry.confidence < kLowConfidenceFallback) {
        report.fallbackPending = true;
        report.fallbackReason =
            "入队置信 " + std::to_string(entry.confidence) + " < 0.3";
        const proposals::ProposalEvidence ev{entry.sessionId, entry.startSeq,
                                             entry.text, now};
        auto card = store.createFinalDraft({}, entry.text, "", {ev},
                                           entry.confidence, false, now);
        if (card.isFail()) {
            return core::propagate<InferStepReport>(card);
        }
        if (auto rm = queue.remove(entry.id); rm.isFail()) {
            return core::propagate<InferStepReport>(rm);
        }
        if (onNote) {
            onNote(RoundNote{false, entry.sessionId,
                             "终稿 · 置信过低未修饰，转人审兜底"});
        }
        return core::Result<InferStepReport>::ok(report);
    }
    // attempts 口径：出机前记账，成败都算——失败重试受同一小时上限约束
    if (auto a = ledger.recordAttempt(now); a.isFail()) {
        return core::propagate<InferStepReport>(a);
    }
    report.attempted = true;
    // 差分快照无条件取（腿 A：终稿卡 targetIds 的差分来源——撤销链锚点，
    // 与卡侧前缀无关）；失败只降级撤销链（targetIds 空），不影响主流程
    std::vector<std::string> beforeIds;
    bool beforeSnapshotted = false;
    {
        auto before = engine.listAll(kInferDiffListLimit, kInferListTimeoutMs);
        if ((beforeSnapshotted = before.isOk())) {
            beforeIds.reserve(before.value().size());
            for (const gateway::MemoryListItem& it : before.value()) {
                beforeIds.push_back(it.id);
            }
        } else {
            LOG_WARN("提炼差分快照失败（终稿卡 targetIds 将缺失，撤销链降级）: " +
                     before.errorMessage());
        }
    }
    const hook::MemoryPrefix cardPrefix = hook::parseMemoryPrefix(entry.text);
    // 修饰（OQ-4 裁决：云端 addInferred 过桥，infer=true 修饰+落库一体；
    // E-1 拆分后换 refine 接口位——telemem_client.h 已预留）
    auto r = engine.addInferred(entry.text);
    if (r.isFail()) {
        auto attempts = queue.recordFailure(entry.id, r.errorMessage());
        if (attempts.isFail()) {
            return core::propagate<InferStepReport>(attempts);
        }
        if (attempts.value() >= kInferMaxAttempts) {
            // OQ-2：重试耗尽 → 出队落兜底（不再无限滞留——「只进不出」根因
            // 修复的出队侧）；失败原因入 fallbackReason + note + LOG
            report.fallbackPending = true;
            report.fallbackReason = "修饰重试耗尽（" + r.errorMessage() + "）";
            const proposals::ProposalEvidence ev{entry.sessionId,
                                                 entry.startSeq, entry.text,
                                                 now};
            auto card = store.createFinalDraft({}, entry.text, "", {ev},
                                               entry.confidence, false, now);
            if (card.isFail()) {
                return core::propagate<InferStepReport>(card);
            }
            if (auto rm = queue.remove(entry.id); rm.isFail()) {
                return core::propagate<InferStepReport>(rm);
            }
            LOG_WARN("提炼重试耗尽转人审兜底: " + report.fallbackReason);
            if (onNote) {
                onNote(RoundNote{true, entry.sessionId,
                                 "终稿 · 修饰重试耗尽，转人审兜底（" +
                                     r.errorMessage() + "）"});
            }
        } else if (onNote) {
            onNote(RoundNote{true, entry.sessionId,
                             "提炼 · 云端失败暂存待补跑（" + r.errorMessage() +
                                 "）"});
        }
        return core::Result<InferStepReport>::ok(report);  // 未耗尽：留队下轮补跑
    }
    // 成功路：差分 + 补标（freshIds/freshTexts → 终稿卡）
    std::vector<std::string> freshIds;
    std::vector<std::string> freshTexts;
    int freshItems = -1;  // 快照失败 = 差分不可得（≠零产物）
    if (beforeSnapshotted) {
        freshItems = tagInferredProducts(engine, cardPrefix, beforeIds,
                                         &freshIds, &freshTexts);
    }
    // 估算记账：输入 = 码点数（账本既有口径）；add 响应无 usage/id 回传，输出
    // 按输入等额估算（输出单价高于输入，宁高勿低）
    const long long tokens =
        static_cast<long long>(proposals::countCodePoints(entry.text));
    if (auto in = ledger.record("llm_input", tokens, now); in.isFail()) {
        return core::propagate<InferStepReport>(in);
    }
    if (auto out = ledger.record("llm_output", tokens, now); out.isFail()) {
        return core::propagate<InferStepReport>(out);
    }
    if (auto rm = queue.remove(entry.id); rm.isFail()) {
        return core::propagate<InferStepReport>(rm);
    }
    report.succeeded = true;  // 云端 add 成功且已出队（含下列零产物形态）
    if (freshItems == 0) {
        // bug239 + OQ-2：add 回包 ok 但引擎清单零新增 = add 失败形态——照常
        // 出队不重试（重试同文本无意义），落兜底 pending 卡人审
        report.productMissing = true;
        report.fallbackPending = true;
        report.fallbackReason =
            "云端回包成功但引擎侧零新增条目（可能异常被吞或去重合并）";
        const proposals::ProposalEvidence ev{entry.sessionId, entry.startSeq,
                                             entry.text, now};
        auto card = store.createFinalDraft({}, entry.text, "", {ev},
                                           entry.confidence, false, now);
        if (card.isFail()) {
            return core::propagate<InferStepReport>(card);
        }
        LOG_WARN("提炼零产物：add 成功但引擎清单无新增（云端异常被吞或去重合并）");
        if (onNote) {
            onNote(RoundNote{true, entry.sessionId,
                             "终稿 · 云端回包成功但引擎侧零新增条目"
                             "（可能异常被吞或被去重合并），转人审兜底"});
        }
        return core::Result<InferStepReport>::ok(report);
    }
    // OQ-2 自动入库：终稿卡 auto_accepted（引擎已由 addInferred 落库——此卡
    // 是台账 + 撤销锚点，不走 executor）。单产物带提炼正文；多产物/差分不可
    // 得带源文（正文以引擎侧为准）。多产物 targetIds 全量回填（执行器首版
    // invalidate 仅支持单目标——多产物撤销暂不成立，wave 3 F-02 聚合再收口）
    const std::string finalText =
        freshTexts.size() == 1 ? freshTexts.front() : entry.text;
    const proposals::ProposalEvidence ev{entry.sessionId, entry.startSeq,
                                         entry.text, now};
    auto card = store.createFinalDraft(freshIds, finalText, "", {ev},
                                       entry.confidence, true, now);
    if (card.isFail()) {
        // 引擎已有产物但卡落库失败（台账/撤销锚点丢失）——如实红不静默；
        // 队列条目已删，下轮不重放（避免引擎重复入库）
        return core::propagate<InferStepReport>(card);
    }
    report.autoAccepted = true;
    if (onNote) {
        onNote(RoundNote{false, entry.sessionId,
                         "终稿 · 修饰完成已自动入库（可撤销）"});
    }
    return core::Result<InferStepReport>::ok(report);
}

}  // namespace module::memory::pipeline
