// src/module_memory/panel/panel_state.h
// 记忆面板数据中枢：三视图共享的取数/操作面。纪律：
//   - compose 只读 snapshot()（锁内拷贝，非阻塞）；refresh() 节流（≥1s），
//     不在 UI 线程做长活（评测在自有后台线程）；
//   - 全部库文件落 engine::memoryDataDir()（随 DEVDESK_DATA_ROOT 隔离）；
//   - 密钥/记忆正文不进日志；提案正文只进 UI 与自家库。
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "core/result.h"
#include "module_memory/engine/telemem_engine.h"
#include "module_memory/collector/collector.h"
#include "module_memory/gateway/telemem_client.h"
#include "module_memory/proposals/proposal_store.h"
#include "module_memory/proposals/usage_ledger.h"
#include "module_memory/sanitize/sanitize_gate.h"
#include "module_memory/panel/recall_eval.h"
#include "module_memory/panel/settings_store.h"
#include "module_memory/pipeline/collect_round.h"
#include "module_memory/pipeline/infer_queue.h"

namespace module::memory::panel {

// 动态流单条（主控台「最近动态」）
struct FeedItem {
    enum class Kind { Collect, Blocked, Inject, Merge, Curate, Usage };
    Kind kind = Kind::Usage;
    std::string text;  // 已含动作前缀（"采集 · …"），不含敏感正文
    std::int64_t ts = 0;
};

// 时间文案（动态流/记录表共用）："今天 HH:MM" / "MM-DD HH:MM"
std::string formatFeedTime(std::int64_t ts);

struct PanelSnapshot {
    MemoryPanelSettings settings;
    bool gateEnabled = true;

    bool storesOk = false;     // 面板库打开失败时 UI 显可读错误，不留白
    std::string storesError;

    std::vector<proposals::Proposal> pending;
    std::vector<proposals::Proposal> decided;  // 最近已决（已处理视图）
    std::vector<bool> decidedUndone;           // 与 decided 对齐（isUndone 派生徽章）
    int proposalTotal = 0;

    long long todayTokens = 0;
    long long monthTokens = 0;
    long long todayCostMilli = 0;
    long long monthCostMilli = 0;
    long long injectItems = 0;  // 今日已注入条数（预算环）
    long long injectChars = 0;  // 今日已注入字数（预算环）
    std::vector<proposals::LedgerRow> recentLedger;

    std::int64_t collectorEvents = 0;
    int collectorSessions = 0;

    RecallResult lastEval;

    std::vector<FeedItem> feed;
    std::int64_t storageBytes = -1;  // 数据目录体积；<0 = 尚未统计

    std::string lastEvalError;  // 最近一轮评测失败原因（空 = 无失败；F5 UI 可见）

    // 引擎快照透传（避免 UI 同时拉两处状态）
    engine::TelememEngine::Snapshot engine;
};

class PanelState {
public:
    PanelState();
    ~PanelState();

    PanelState(const PanelState&) = delete;
    PanelState& operator=(const PanelState&) = delete;

    // 每帧可调：节流刷新（force = 动作后立刷）
    void refresh(bool force = false);

    // 需-9 用量告警判定（装配层每帧调，不依赖记忆页可见）：内部 5s 节流；越线
    // 击发 → toast 桥（应用级）+ platform::notify（带 level）双出口，闸状态即落盘
    void pollUsageAlerts();

    PanelSnapshot snapshot() const;  // 锁内拷贝

    // ── 设置写入口（改内存 + 落盘 + 联动刷新）──
    void setSanitizeEnabled(bool on);  // off = 旁路 + 审计行（UI 已做二次确认）
    void setRecallThresholdPct(int pct);
    void setTwoWordBaseline(int n);
    // 需-9 用量告警阈值（月档软/硬 = 消耗卡预算同一套数，不另起一套）
    void setUsageAlertThresholds(long long dayTokens, long long softTokens,
                                 long long hardTokens);
    void markLlmImported(const std::string& profileName);

    // ── 审核操作（真接 ProposalStore；返回值供 toast）──
    // A 阶段引擎无版本查询缝：accept 的 baseVersion 前置校验以提案快照放行
    //（校验形同旁路；升级 C 接自家库后换真版本查询——契约位置不动）。
    core::Result<void> acceptProposal(const std::string& id);
    core::Result<void> rejectProposal(const std::string& id);
    // 编辑后接受 = 改 pending 正文（审计留痕）→ 走接受全链路
    core::Result<void> editThenAccept(const std::string& id,
                                      const std::string& newText);
    // 撤销链：为已接受提案建撤销卡（pending，出现在待审列表再审一次）
    core::Result<std::string> createUndoCard(const std::string& originalId);
    core::Result<int> batchAccept(const std::vector<std::string>& ids);

    // ── 触发线评测（后台线程；引擎 Running 时才允许启动）──
    bool evalRunning() const;
    core::Result<void> startEvalAsync();  // Preparing/Running 中再调 = no-op fail

    // ── 数据与隐私 ──
    core::Result<std::string> exportAllToJson();  // 返回落盘文件绝对路径
    core::Result<void> clearAllOwnStores();       // 自家库清空（引擎侧缝见回单）

private:
    void openStores();
    void rebuildFeedLocked();
    std::int64_t dirSizeBytes() const;

    // ── F1 背景采集循环（引擎 Running 期间自驱）──
    // 状态同步：refresh 每帧调（引擎起→起循环；引擎停→通知停并回收句柄）。
    // 线程内自检引擎态：页未打开也随引擎停而停。反重构警告：循环与面板共用
    // 同一 ProposalStore 连接（唯一真相）；禁止给循环另开库连接。
    void syncCollectLoop();
    void collectLoopMain();

    mutable std::mutex mu_;
    PanelSnapshot snap_;

    sanitize::SanitizeGate gate_;
    std::optional<proposals::ProposalStore> proposalStore_;
    std::optional<proposals::UsageLedger> ledger_;
    std::optional<collector::CollectorStore> collectorStore_;
    std::optional<pipeline::InferQueueStore> inferQueue_;
    std::optional<RecallEvalStore> evalStore_;

    std::int64_t lastRefreshSec_ = 0;
    std::int64_t lastSizeCheckSec_ = 0;
    std::int64_t lastAlertPollSec_ = 0;  // 需-9：判定节流（5s 一档，SQLite 读数轻查）

    // 评测线程状态（ mu_ 保护结果落库，flag 原子）
    std::atomic<bool> evalRunning_{false};

    // F1 采集循环句柄与运行期动态行（拦截/出卡 note，mu_ 保护，封顶防涨）
    std::thread collectThread_;
    std::atomic<bool> collectStop_{false};
    std::atomic<bool> collectActive_{false};
    std::vector<FeedItem> runtimeFeed_;
};

}  // namespace module::memory::panel
