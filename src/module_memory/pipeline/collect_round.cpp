// src/module_memory/pipeline/collect_round.cpp
#include "collect_round.h"

#include <ctime>
#include <utility>

#include "core/logging.h"

namespace module::memory::pipeline {

namespace {

std::int64_t nowSec() { return static_cast<std::int64_t>(std::time(nullptr)); }

// 拦截段 note：只含会话名与类别（脱敏契约：行内零原文零正文片段）
RoundNote blockedNote(const collector::Segment& seg, const std::string& text) {
    RoundNote n;
    n.blocked = true;
    n.sessionId = seg.sessionId;
    n.text = text;
    return n;
}

}  // namespace

core::Result<CollectRoundReport> runCollectRound(
    collector::Collector& collector, const sanitize::SanitizeGate& gate,
    proposals::ProposalStore& store,
    const std::function<void(const RoundNote&)>& onNote,
    InferQueueStore* inferQueue) {
    auto segments = collector.pollOnce();
    if (segments.isFail()) {
        return core::Result<CollectRoundReport>::fail(segments.errorCode(),
                                                      segments.errorMessage());
    }
    CollectRoundReport report;
    report.segments = static_cast<int>(segments.value().size());
    const auto now = nowSec();
    for (const collector::Segment& seg : segments.value()) {
        // 单一管线入口：出机（入库提案）前每段必过闸
        auto gated = gate.process(seg.text);
        if (gated.isFail()) {
            // fail-closed：非法输入视为检测器无法保证 → 中断外发，不入提案
            ++report.blocked;
            RoundNote n = blockedNote(
                seg, "拦截 · 会话 " + seg.sessionId +
                         " 输入非法（fail-closed，已隔离不入提案）");
            if (onNote) onNote(n);
            report.notes.push_back(std::move(n));
            continue;
        }
        if (!gated.value().hits.empty()) {
            // 命中拦截：脱敏预览只进审计口径，原文零落库；不产提案
            ++report.blocked;
            std::string cats;
            for (const auto& c : sanitize::SanitizeResult{gated.value()}.categories()) {
                if (!cats.empty()) cats += ",";
                cats += c;
            }
            RoundNote n = blockedNote(
                seg, "拦截 · 会话 " + seg.sessionId + " 检出 " + cats +
                         "（已隔离不入提案）");
            if (onNote) onNote(n);
            report.notes.push_back(std::move(n));
            continue;
        }
        std::vector<proposals::ProposalEvidence> evidence;
        evidence.reserve(seg.events.size());
        for (const auto& ev : seg.events) {
            evidence.push_back(proposals::ProposalEvidence{
                ev.sessionId, static_cast<std::int64_t>(ev.eventSeq),
                seg.text, now});
        }
        // G-1 提炼暂存：过闸段入队（commit 之前；崩溃重放唯一键去重）。入队
        // 失败不阻断卡片链（主通道），该段提炼机会丢失如实留痕
        if (inferQueue != nullptr) {
            const std::int64_t startSeq =
                seg.events.empty()
                    ? 0
                    : static_cast<std::int64_t>(seg.events.front().eventSeq);
            if (auto q = inferQueue->enqueue(seg.sessionId, startSeq,
                                             gated.value().text, now);
                q.isFail()) {
                LOG_WARN("提炼入队失败: " + q.errorMessage());
            }
        }
        auto card = proposals::Curator::proposeAdd(store, gated.value().text,
                                                   evidence, 0.9, now);
        if (card.isFail()) {
            // 单段建卡失败不中断整轮（下轮水位未推进该段事件级去重兜底）
            LOG_WARN("采集建卡失败: " + card.errorMessage());
            continue;
        }
        ++report.cardsCreated;
        RoundNote n;
        n.blocked = false;
        n.sessionId = seg.sessionId;
        n.text = "采集 · 会话 " + seg.sessionId + " 生成待审新增卡";
        if (onNote) onNote(n);
        report.notes.push_back(std::move(n));
    }
    // 记账：处理完才推进水位/事件（拦截段也记已处理——不入提案但不重复扫）
    if (auto c = collector.commit(segments.value()); c.isFail()) {
        return core::Result<CollectRoundReport>::fail(c.errorCode(),
                                                      c.errorMessage());
    }
    return core::Result<CollectRoundReport>::ok(std::move(report));
}

core::Result<int> runDecayScan(proposals::ProposalStore& store,
                               const proposals::MemoryListFn& memories,
                               int idleThresholdDays,
                               const std::function<void(const RoundNote&)>& onNote) {
    auto created = proposals::Curator::proposeForget(
        store, memories, idleThresholdDays, nowSec());
    if (created.isFail()) {
        return core::Result<int>::fail(created.errorCode(),
                                       created.errorMessage());
    }
    if (onNote && !created.value().empty()) {
        RoundNote n;
        n.blocked = false;
        n.text = "整理 · 闲置扫描生成 " +
                 std::to_string(created.value().size()) + " 张待审遗忘卡";
        onNote(n);
    }
    return core::Result<int>::ok(static_cast<int>(created.value().size()));
}

}  // namespace module::memory::pipeline
