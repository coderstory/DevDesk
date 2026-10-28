// src/module_memory/pipeline/collect_round.h
// 采集管线唯一入口（F1 产品接线）：collector 扫描 → 脱敏闸 → curator.proposeAdd。
// 契约（审计 F1 收敛约定，偏离即契约变更）：
//   - 单一管线：出机前的每一段必须过 SanitizeGate::process；fail-closed（Error）
//     与命中拦截段一律不入提案，只出动态流可见 note（行内零正文零原文片段）；
//   - 记账时机：本轮段全部处理完才 commit（水位/事件幂等键兜底，崩溃重放不重复）；
//   - 零引擎依赖：管线不触 TelememEngine/检索缝——采集在引擎侧之前，入库（接受卡）
//     才需要引擎；线程化/触发节奏归宿主（PanelState 背景循环），本单元可同步直驱。
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "module_memory/collector/collector.h"
#include "module_memory/proposals/curator.h"
#include "module_memory/proposals/proposal_store.h"
#include "module_memory/sanitize/sanitize_gate.h"

namespace module::memory::pipeline {

// 动态流可见行（零敏感内容：只有会话名/类别/计数）
struct RoundNote {
    bool blocked = false;      // true = 拦截/隔离段；false = 正常出卡
    std::string sessionId;
    std::string text;
};

struct CollectRoundReport {
    int segments = 0;          // 本轮出段数（含被拦截的）
    int cardsCreated = 0;      // 新增 pending 卡数
    int blocked = 0;           // 拦截段数（命中命中集 + fail-closed Error）
    std::vector<RoundNote> notes;
};

// 单轮采集（同步、可重入；宿主循环与测试直驱同一实现）。
// onNote 可空（测试直驱时用返回值断言即可）。
core::Result<CollectRoundReport> runCollectRound(
    collector::Collector& collector, const sanitize::SanitizeGate& gate,
    proposals::ProposalStore& store,
    const std::function<void(const RoundNote&)>& onNote);

// decay 扫描（周期触发体）：整理器遗忘扫描；清单缝由宿主注入。返回本轮新建卡数。
// 生产清单缝在引擎列举能力落地前如实报 NotImplemented（fail-closed：不出卡、不假扫描）。
core::Result<int> runDecayScan(proposals::ProposalStore& store,
                               const proposals::MemoryListFn& memories,
                               int idleThresholdDays,
                               const std::function<void(const RoundNote&)>& onNote);

// decay 扫描周期（首版固定值：引擎 Running 期间每 6h 扫一次闲置）
inline constexpr int kDecayScanIntervalSec = 6 * 3600;

}  // namespace module::memory::pipeline
