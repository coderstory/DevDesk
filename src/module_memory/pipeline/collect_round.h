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
#include "module_memory/pipeline/infer_queue.h"
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

// 溢出样本引用（bug191 BD 批）：定位被限流丢弃的段——会话 + 段首事件序号 +
// 内容指纹（归一化正文 SHA256，哈希非正文）。零正文零敏感（与 RoundNote
// 同一脱敏红线，进 report/日志安全）
struct OverflowSample {
    std::string sessionId;
    std::int64_t firstEventSeq = -1;
    std::string fingerprint;
};

struct CollectRoundReport {
    int segments = 0;          // 本轮出段数（含被拦截的；≤kMaxSegmentsPerPoll）
    int cardsCreated = 0;      // 新增 pending 卡数
    int blocked = 0;           // 拦截段数（命中命中集 + fail-closed Error）
    int washed = 0;            // F-06 L2 段级洗命中数（P3 中断行删/P4 前缀剥）
    int dedupSkipped = 0;      // F-01 指纹命中跳过（不建卡不排队）
    int lowValueSkipped = 0;   // F-03 噪声回声/极短段跳过
    int expiredSwept = 0;      // F-04 本轮 TTL 沉降的 pending 数
    int overflowDropped = 0;   // BD 批：单会话超单轮上限被丢弃段数（终局不重放）
    std::vector<OverflowSample> overflowSamples;  // 头/尾样本引用（每会话 ≤2 条）
    std::vector<RoundNote> notes;
};

// 极短段阈值（bug191 F-03② 拍板 2026-11-05）：有效字符 <50 拒。
// 有效字符 = CJK 表意文字计 1 + ASCII 字母数字计 0.5（与卡面截断同宽窄口径）
inline constexpr int kMinEffectiveUnits = 50;

// 单轮单会话出卡上限（bug191 BD 批拍板 N=10，2026-11-18，判据=evidence/
// 20261118-memory-one-session 单会话累计 195 张）：防会话级提案洪峰。
// 195≈4 轮×50 段 → 同流量降至 4×10=40（−79%）；单轮 10 张在人审负担内；
// 正常会话 30s 轮窗出 >10 张提案级段罕见（≈40KB 持续正文输出）。
// 落点在 F-06 洗 / F-03 低值 / F-01 去重之后（上限只数真卡：噪声与重复不占
// 预算也不虚增 overflow）；超限段丢弃且 commit 记已处理（终局不重放），
// overflow 计数 + 头尾样本引用进 report，不静默。
inline constexpr int kMaxCardsPerSessionRound = 10;

// F-06 L2 段级洗（need333 记忆波 1，判据=evidence/20261118-f06-gate
// scheme-card §1 P3/P4，P3 口径按实测 4 形态扩）：P3 擦除行内
// "[Request interrupted by user…]" 括号段（含 for tool use 变体；洗后空行
// 整行丢，保两侧正文）；P4 session_reader 自产 "[tool_error] " 行前缀剥
//（保留行内其余正文）。纯函数（红队/对照单测钉契约，先例=sanitize scanText）。
// *washed 出参：命中任一标记置 true（调用方计数 report.washed，F-08 对账）。
std::string stripLeakMarkers(const std::string& body, bool* washed);

// 单轮采集（同步、可重入；宿主循环与测试直驱同一实现）。
// onNote 可空（测试直驱时用返回值断言即可）。
// inferQueue 可空（默认）：注入时，过脱敏闸的段在 collector.commit 之前入提炼
// 暂存队列（G-1「新采集入队暂存」）；唯一键兜崩溃重放，入队失败只 LOG 不阻断
// 卡片链（主通道）。
core::Result<CollectRoundReport> runCollectRound(
    collector::Collector& collector, const sanitize::SanitizeGate& gate,
    proposals::ProposalStore& store,
    const std::function<void(const RoundNote&)>& onNote,
    InferQueueStore* inferQueue = nullptr);

// decay 扫描（周期触发体）：整理器遗忘扫描；清单缝由宿主注入。返回本轮新建卡数。
// 生产清单缝在引擎列举能力落地前如实报 NotImplemented（fail-closed：不出卡、不假扫描）。
core::Result<int> runDecayScan(proposals::ProposalStore& store,
                               const proposals::MemoryListFn& memories,
                               int idleThresholdDays,
                               const std::function<void(const RoundNote&)>& onNote);

// decay 扫描周期（首版固定值：引擎 Running 期间每 6h 扫一次闲置）
inline constexpr int kDecayScanIntervalSec = 6 * 3600;

}  // namespace module::memory::pipeline
