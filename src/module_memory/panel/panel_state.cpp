// src/module_memory/panel/panel_state.cpp
#include "panel_state.h"
#include <chrono>
#include <system_error>
#include <utility>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <thread>

#include "core/logging.h"
#include "core/str_util.h"
#include "core/time.h"
#include "platform/folder_picker.h"
#include "platform/json.h"
#include "platform/notification.h"
#include "module_memory/engine/telemem_config.h"
#include "module_memory/pipeline/collect_round.h"
#include "module_memory/pipeline/infer_batch.h"
#include "module_memory/proposals/curator.h"
#include "module_memory/proposals/usage_alert.h"

namespace module::memory::panel {

namespace {

using proposals::Proposal;
using proposals::ProposalExecutor;
using proposals::ProposalStore;
using proposals::TargetVersionFn;
using proposals::UsageLedger;

std::filesystem::path dbDir() { return engine::memoryDataDir(); }

// 需-9 判定节流：装配层每帧调 pollUsageAlerts，SQLite 双求和走此间隔限频
constexpr std::int64_t kUsageAlertPollSec = 5;

// baseVersion 前置校验真值（池32 缝闭环）：引擎逐条目版本 = get_memory 的
// updated_at epoch；错误码分流语义见 makeTargetVersionFn（proposal_store.h）
TargetVersionFn realTargetVersion(std::int64_t baseVersion) {
    return proposals::makeTargetVersionFn(
        baseVersion, [](const std::string& targetId) {
            gateway::TelememHttpRetriever engine;
            return engine.getMemoryVersion(targetId);
        });
}

// 代理执行器：真接检索接口缝（引擎不在 = 执行失败，提案保持待审）
ProposalExecutor makeExecutor() {
    return [](const Proposal& p) -> core::Result<std::string> {
        gateway::TelememHttpRetriever engine;
        return proposals::executeProposal(p, engine);
    };
}

// F1 采集循环参数：stop 分片（UI 线程 join 上界）与 decay 闲置阈值（B4 设计值）
constexpr int kCollectStopTickMs = 250;
constexpr int kDecayIdleThresholdDays = 60;
// decay 清单拉取上限（与 findMemoryIdByText 既有 get_memories 口径一致；
// 超限部分本轮不扫描且无提示，个人库量级远低于此——OQ-2 留档方案卡）
constexpr int kDecayScanListLimit = 200;

// 时间文案："今天 HH:MM" / "MM-DD HH:MM"
std::string formatTime(std::int64_t ts) {
    if (ts <= 0) return "";
    std::tm tmNow{};
    std::tm tmThen{};
    const auto now = std::time(nullptr);
    core::time::localTime(now, &tmNow);
    core::time::localTime(static_cast<std::time_t>(ts), &tmThen);
    char buf[32];
    if (tmNow.tm_year == tmThen.tm_year && tmNow.tm_yday == tmThen.tm_yday) {
        std::snprintf(buf, sizeof(buf), "今天 %02d:%02d", tmThen.tm_hour,
                      tmThen.tm_min);
    } else {
        std::snprintf(buf, sizeof(buf), "%02d-%02d %02d:%02d", tmThen.tm_mon + 1,
                      tmThen.tm_mday, tmThen.tm_hour, tmThen.tm_min);
    }
    return buf;
}

FeedItem::Kind ledgerKindToFeed(const std::string& kind) {
    if (kind.rfind("inject", 0) == 0) return FeedItem::Kind::Inject;
    if (kind == "embed") return FeedItem::Kind::Collect;
    if (kind == "llm_input" || kind == "llm_output") {
        return FeedItem::Kind::Curate;
    }
    return FeedItem::Kind::Usage;
}

std::string ledgerRowText(const proposals::LedgerRow& r) {
    char buf[160];
    if (r.kind == "inject_items") {
        std::snprintf(buf, sizeof(buf),
                      "注入 · 向 Claude 注入 %lld 条相关记忆（预算 ≤5 条）",
                      r.tokens);
    } else if (r.kind == "inject_chars") {
        return "";  // 与 items 同一事件成对入账，流里只出条数那条
    } else if (r.kind == proposals::kKindLlmAttempt) {
        return "";  // 尝试行与后续 llm_input/output 成对，流里只出结果行
    } else if (r.kind == "embed") {
        std::snprintf(buf, sizeof(buf),
                      "采集 · %lld 字完成向量化（本机 embedding，零 API 费）",
                      r.tokens);
    } else if (r.kind == "llm_input") {
        std::snprintf(buf, sizeof(buf), "提炼 · 发送 %lld 字到云端提炼（已过脱敏闸）",
                      r.tokens);
    } else if (r.kind == "llm_output") {
        std::snprintf(buf, sizeof(buf), "提炼 · 云端产出 %lld 字候选记忆", r.tokens);
    } else {
        std::snprintf(buf, sizeof(buf), "调用 · %s 计 %lld token", r.kind.c_str(),
                      r.tokens);
    }
    return buf;
}

std::string proposalKindName(const std::string& kind) {
    if (kind == "add") return "新增";
    if (kind == "forget") return "遗忘";
    if (kind == "invalidate") return "失效";
    if (kind == "merge") return "合并";
    return kind;
}

}  // namespace

std::string formatFeedTime(std::int64_t ts) { return formatTime(ts); }

PanelState::PanelState()
    : gate_([](const std::string& line) {
          // 旁路/过闸审计行：只含计数与长度（SanitizeGate 契约），进日志通道
          LOG_INFO("[memory][sanitize] " + line);
      }) {
    // 首帧前即有可读快照：调用方零判空（need401 快照共享指针契约）
    pub_ = std::make_shared<PanelSnapshot>();
}

PanelState::~PanelState() {
    // F1 循环先停再拆成员：线程触碰 proposalStore_ 等 optional，必须先 join
    if (collectThread_.joinable()) {
        collectStop_.store(true);
        collectThread_.join();
    }
    queryStop_.store(true);
    if (queryThread_.joinable()) queryThread_.join();
}

void PanelState::openStores() {
    std::error_code ec;
    std::filesystem::create_directories(dbDir(), ec);
    auto prop = ProposalStore::open(dbDir() / "proposals.db");
    auto led = UsageLedger::open(dbDir() / "usage_ledger.db");
    auto col = collector::CollectorStore::open(dbDir() / "collector.db");
    auto que = pipeline::InferQueueStore::open(dbDir() / "infer_queue.db");
    auto ev = RecallEvalStore::open(dbDir() / "recall_eval.db");
    std::string err;
    if (prop.isOk()) proposalStore_.emplace(std::move(prop.value()));
    else err += "提案库打开失败；";
    if (led.isOk()) ledger_.emplace(std::move(led.value()));
    else err += "账本打开失败；";
    if (col.isOk()) collectorStore_.emplace(std::move(col.value()));
    else err += "采集账本打开失败；";
    if (que.isOk()) inferQueue_.emplace(std::move(que.value()));
    else err += "提炼队列打开失败；";
    if (ev.isOk()) evalStore_.emplace(std::move(ev.value()));
    else err += "评测库打开失败；";
    std::lock_guard<std::mutex> lk(mu_);
    live_.storesOk = err.empty();
    live_.storesError = err;
    // 重开面板取历史读数（recall_eval.h 契约「面板重开取 last()」）：publishLocked
    // 用 live_.lastEval 覆盖数据面值（e7470e05 帧内后台化引入），live_ 不在开库时
    // 种子则重启后恒显「未评测」，直到当轮评测完成
    if (evalStore_.has_value()) live_.lastEval = evalStore_->last();
}

// ── F1 背景采集循环 ─────────────────────────────────────────────────────────

void PanelState::syncCollectLoop() {
    const bool running =
        engine::TelememEngine::global().snapshot().state ==
        engine::TelememEngine::State::Running;
    if (collectThread_.joinable()) {
        if (!collectActive_.load()) {
            collectThread_.join();  // 线程自停（引擎停/stop）→ 回收句柄
        } else if (!running) {
            collectStop_.store(true);
            collectThread_.join();  // 分片等待保证 ≤kCollectStopTickMs 返回
        } else {
            return;  // 运行中：交给线程自管
        }
    }
    if (running && proposalStore_.has_value()) {
        // 引擎 Running → 起循环（首轮立即跑，此后 kPollIntervalMs 一轮）
        bool expected = false;
        if (collectActive_.compare_exchange_strong(expected, true)) {
            collectStop_.store(false);
            collectThread_ = std::thread([this]() { collectLoopMain(); });
        }
    }
}

void PanelState::collectLoopMain() {
    // 谓词自检：页未打开也随引擎停而停（引擎停 → 循环停，F1 契约）
    const auto engineRunning = []() {
        return engine::TelememEngine::global().snapshot().state ==
               engine::TelememEngine::State::Running;
    };
    const auto note = [this](const pipeline::RoundNote& n) {
        // 动态流可见（拦截段必可见）；行内只有会话名/类别/计数，零正文
        std::lock_guard<std::mutex> lk(mu_);
        runtimeFeed_.push_back(FeedItem{
            n.blocked ? FeedItem::Kind::Blocked : FeedItem::Kind::Collect,
            n.text, static_cast<std::int64_t>(std::time(nullptr))});
        if (runtimeFeed_.size() > 16) runtimeFeed_.erase(runtimeFeed_.begin());
    };
    // 生产 decay 清单缝（池31 缝闭环 2026-10-31）：get_memories 全量清单，
    // 不传 user_id → 仅默认用户（events 伪用户不在扫描范围，OQ-1 留档方案卡）。
    // updated_at 同源双用：lastUsedAt 闲置代理（老板拍板 2026-10-31，偏乐观
    // 知情接受）+ version（池32 forget 卡 baseVersion 输入）。失败如实透传
    // → runDecayScan fail → LOG 留痕下轮重试（fail-closed 不出假卡）
    const proposals::MemoryListFn decayMemories = []() {
        gateway::TelememHttpRetriever engine;
        auto items = engine.listAll(kDecayScanListLimit, 10000);
        if (items.isFail()) {
            return core::Result<std::vector<proposals::MemorySummary>>::fail(
                items.errorCode(), items.errorMessage());
        }
        std::vector<proposals::MemorySummary> out;
        out.reserve(items.value().size());
        for (const gateway::MemoryListItem& it : items.value()) {
            out.push_back(proposals::MemorySummary{
                it.id, it.text, it.updatedAtEpoch, it.updatedAtEpoch});
        }
        return core::Result<std::vector<proposals::MemorySummary>>::ok(
            std::move(out));
    };
    std::int64_t lastDecay = 0;
    for (;;) {
        if (collectStop_.load()) break;
        if (!engineRunning()) break;
        if (proposalStore_.has_value()) {
            // 闸快照锁内拷贝（脱敏开关翻转的跨线程可见性）
            sanitize::SanitizeGate gate;
            {
                std::lock_guard<std::mutex> lk(mu_);
                gate = gate_;
            }
            collector::Collector col(dbDir() / "collector.db",
                                     collector::Collector::resolveProjectsRootFromEnv());
            auto r = pipeline::runCollectRound(col, gate, *proposalStore_, note,
                                               inferQueue_ ? &*inferQueue_
                                                           : nullptr);
            if (r.isFail()) {
                LOG_WARN("采集轮失败: " + r.errorMessage());
            } else {
                if (r.value().blocked > 0 || r.value().cardsCreated > 0) {
                    needPublish_.store(true);  // 有产出 → 下帧立刷动态流/审核数
                }
            }
            // G-1 控耗接线：采集轮后置单步提炼（同线程同生命周期）。硬档暂停/
            // 节流顺延/云端失败均留队下轮补跑；闸两级读 usage_ledger 账面与
            // panel_settings 阈值（直读盘，阈值改动即时生效，同 pollUsageAlerts）
            if (inferQueue_.has_value() && ledger_.has_value()) {
                const auto settings = loadPanelSettings();
                pipeline::InferLimits limits;
                limits.hardBudgetTokens = settings.usageHardBudgetTokens;
                limits.hourlyLimit = settings.inferHourlyLimit;
                gateway::TelememHttpRetriever inferEngine;
                auto step = pipeline::runInferStep(*inferQueue_, inferEngine,
                                                   *proposalStore_, *ledger_,
                                                   limits, note);
                if (step.isFail()) {
                    LOG_WARN("提炼步失败: " + step.errorMessage());
                } else if (step.value().succeeded ||
                           step.value().attempted) {
                    needPublish_.store(true);  // 有动作 → 下帧立刷动态流/消耗卡
                }
            }
            const auto now = static_cast<std::int64_t>(std::time(nullptr));
            if (now - lastDecay >= pipeline::kDecayScanIntervalSec) {
                lastDecay = now;
                auto d = pipeline::runDecayScan(*proposalStore_, decayMemories,
                                                kDecayIdleThresholdDays, note);
                if (d.isFail()) {
                    LOG_INFO("decay 扫描未执行: " + d.errorMessage());
                }
            }
        }
        // 有界分片等待：stop 请求最迟 kCollectStopTickMs 生效（UI join 不被拖 30s）
        for (int waited = 0;
             waited < collector::kPollIntervalMs && !collectStop_.load() &&
             engineRunning();
             waited += kCollectStopTickMs) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(kCollectStopTickMs));
        }
    }
    collectActive_.store(false);
}

void PanelState::refresh(bool force) {
    lastPokeSec_.store(static_cast<std::int64_t>(std::time(nullptr)));
    std::string pipIndexUrl;
    std::string pipExtraIndexUrl;
    {
        std::lock_guard<std::mutex> lk(mu_);
        live_.settings = loadPanelSettings();
        live_.gateEnabled = gate_.enabled();
        live_.engine = engine::TelememEngine::global().snapshot();
        pipIndexUrl = live_.settings.pipIndexUrl;
        pipExtraIndexUrl = live_.settings.pipExtraIndexUrl;
    }
    // bug476：镜像设置随帧同步到安装引擎（UI 值 > 环境变量由引擎侧 resolver 决定）
    engine::TelememEngine::global().setPipMirror(pipIndexUrl, pipExtraIndexUrl);
    if (!std::filesystem::exists(dbDir())) {
        // 首跑：目录还没有 = 全零账面（不留白，不建库——等首跑向导）
        std::lock_guard<std::mutex> lk(mu_);
        live_.storesOk = true;
        // 首帧也要有可读快照（调用方零判空）；稳态帧 live 面未变则不重发
        if (!pub_ || liveStateChangedLocked()) publishLocked(nullptr);
        return;
    }
    if (!proposalStore_.has_value()) openStores();
    syncCollectLoop();  // F1：每帧同步（不受 1s 节流约束；引擎起停即刻响应）
    syncQueryLoop();    // 数据面查询线程（1s 节拍；首帧起，此后自驱）

    std::shared_ptr<PanelSnapshot> next;
    bool liveOnly = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        const bool kick = needPublish_.exchange(false);
        next = force ? nullptr : std::move(fresh_);  // force 必同步重建（后台结果可能缺刚落的账）
        if (!next && !force) {
            // 零拷贝门：数据面无新结果 → 只有 live 面变了才重发，否则本帧不拷；
            // 进页首帧/清库后（!dataBuilt_）落到下面同步构建，避免首个 1s 空白
            if (dataBuilt_) {
                if (kick || liveStateChangedLocked()) {
                    liveOnly = true;
                } else {
                    return;
                }
            }
        }
    }
    if (!next && !liveOnly) {
        // 同步构建：force（动作后立刷）或进页首帧/清库后（否则首 1s 空白）
        next = std::make_shared<PanelSnapshot>();
        fillDataSnapshot(*next);  // 查询在主线程：单次，#411 已限窗
        std::lock_guard<std::mutex> lk(mu_);
        fresh_.reset();  // 本次同步结果覆盖后台旧结果
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        dataBuilt_ = true;
        publishLocked(std::move(next));
    }
}

// 数据面重建：提案/账本/采集/评测/体积/动态流。查询全在锁外（后台 1s 节拍或
// force 主线程），共享 store 连接靠 SQLite FULLMUTEX 串行化。
void PanelState::fillDataSnapshot(PanelSnapshot& next) {
    const auto now = static_cast<std::int64_t>(std::time(nullptr));
    if (proposalStore_.has_value()) {
        // need401 #411：待审只取窗口上限内的最早 N 条（created_at ASC），真实总数走
        // countByStatus 的单条 GROUP BY。无上限时 1s 刷新在提案风暴下逐秒拉行解码
        // 全部待审（1630 条实测 135ms），并把全量拷进每帧均拷贝的快照。
        constexpr int kPendingWindow = 200;
        next.pendingTotal = 0;
        if (auto cnt = proposalStore_->countByStatus(); cnt.isOk()) {
            for (const auto& kv : cnt.value()) {
                if (kv.first == "pending") next.pendingTotal = static_cast<int>(kv.second);
            }
        }
        if (auto pend = proposalStore_->listPending(kPendingWindow); pend.isOk()) {
            next.pending = std::move(pend.value());
        }
        if (next.pendingTotal < static_cast<int>(next.pending.size())) {
            next.pendingTotal = static_cast<int>(next.pending.size());
        }
        if (auto dec = proposalStore_->listRecentDecided(20); dec.isOk()) {
            next.decided = std::move(dec.value());
            next.decidedUndone.clear();
            for (const auto& d : next.decided) {
                next.decidedUndone.push_back(proposalStore_->isUndone(d.id));
            }
        }
        next.proposalTotal = static_cast<int>(next.pending.size()) +
                             static_cast<int>(next.decided.size());
    }
    if (ledger_.has_value()) {
        const auto nowSec = static_cast<std::int64_t>(std::time(nullptr));
        auto num = [](core::Result<long long> r, long long dflt) {
            return r.isOk() ? r.value() : dflt;
        };
        next.todayTokens = num(ledger_->todayTokens(nowSec), 0);
        next.monthTokens = num(ledger_->monthTokens(nowSec), 0);
        next.todayCostMilli = num(ledger_->todayCostMilliYuan(nowSec), 0);
        next.monthCostMilli = num(ledger_->monthCostMilliYuan(nowSec), 0);
        next.injectItems = num(ledger_->todayInjectItems(nowSec), 0);
        next.injectChars = num(ledger_->todayInjectChars(nowSec), 0);
        if (auto rows = ledger_->recentRows(12); rows.isOk()) {
            next.recentLedger = std::move(rows.value());
        }
    }
    if (collectorStore_.has_value()) {
        if (auto n = collectorStore_->processedCount(); n.isOk()) {
            next.collectorEvents = n.value();
        }
        if (auto n = collectorStore_->sessionCount(); n.isOk()) {
            next.collectorSessions = n.value();
        }
    }
    if (evalStore_.has_value()) next.lastEval = evalStore_->last();

    // 数据目录体积（60s 一查，递归统计不进每秒档）
    bool statNow = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        next.storageBytes = live_.storageBytes;
        if (now - lastSizeCheckSec_ >= 60) {
            lastSizeCheckSec_ = now;
            statNow = true;
        }
    }
    if (statNow) {
        next.storageBytes = dirSizeBytes();
        std::lock_guard<std::mutex> lk(mu_);
        live_.storageBytes = next.storageBytes;
    }

    // 动态流 = ledger + store 混排（数据源口径见任务书 U1）
    {
        std::vector<FeedItem> items;
        {
            // F1 运行期行（拦截/出卡 note）先入混排，与账面行按 ts 统一排序
            std::lock_guard<std::mutex> lk(mu_);
            for (const FeedItem& it : runtimeFeed_) items.push_back(it);
        }
        for (const auto& r : next.recentLedger) {
            FeedItem it;
            it.kind = ledgerKindToFeed(r.kind);
            it.text = ledgerRowText(r);
            it.ts = r.ts;
            if (!it.text.empty()) items.push_back(std::move(it));
        }
        auto pushCreated = [&](const Proposal& p) {
            FeedItem it;
            it.kind = FeedItem::Kind::Merge;
            it.text = "提案 · 「" + proposalKindName(p.kind) + "」待你审核";
            it.ts = p.createdAt;
            items.push_back(std::move(it));
        };
        for (const auto& p : next.pending) pushCreated(p);
        for (const auto& p : next.decided) {
            if (p.status != "accepted") continue;
            FeedItem it;
            it.kind = FeedItem::Kind::Merge;
            it.text = "卡片 · 已接受「" + proposalKindName(p.kind) + "」提案" +
                      (p.appliedAt > 0 ? "并已在引擎执行" : "（引擎执行待重试）");
            it.ts = p.decidedAt;
            items.push_back(std::move(it));
        }
        std::stable_sort(items.begin(), items.end(),
                         [](const FeedItem& a, const FeedItem& b) {
                             return a.ts > b.ts;
                         });
        if (items.size() > 8) items.resize(8);
        next.feed = std::move(items);
    }
}

void PanelState::syncQueryLoop() {
    if (queryThread_.joinable()) {
        if (!queryStop_.load()) return;  // 运行中：自驱
        queryThread_.join();             // 已自停（闲置/清库）→ 回收句柄
    }
    queryStop_.store(false);
    queryThread_ = std::thread([this]() { queryLoopMain(); });
}

void PanelState::queryLoopMain() {
    // 1s 节拍（100ms 分片保证 stop 及时，与采集循环同法）：提案/账本查询不占 UI
    // 帧；结果落 fresh_，由 refresh() 下一帧发布（后到覆盖，UI 只要最新）
    for (;;) {
        for (int i = 0; i < 10; ++i) {
            if (queryStop_.load()) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        // 页不在用（compose 不再调 refresh）→ 自停，不带走量（空转 CPU 预算）
        if (static_cast<std::int64_t>(std::time(nullptr)) - lastPokeSec_.load() > 3) {
            queryStop_.store(true);
            return;
        }
        auto next = std::make_shared<PanelSnapshot>();
        fillDataSnapshot(*next);
        std::lock_guard<std::mutex> lk(mu_);
        fresh_ = std::move(next);
    }
}

void PanelState::publishLocked(std::shared_ptr<PanelSnapshot> next) {
    if (!next) {
        // 仅 live 面变化（引擎起停/评测结果）：复用上帧数据面重发
        next = std::make_shared<PanelSnapshot>();
        if (pub_) *next = *pub_;
    }
    next->settings = live_.settings;
    next->gateEnabled = live_.gateEnabled;
    next->engine = live_.engine;
    next->storesOk = live_.storesOk;
    next->storesError = live_.storesError;
    next->lastEvalError = live_.lastEvalError;
    next->lastEval = live_.lastEval;
    pub_ = std::move(next);
}

// 发布门（mu_ 内）：只比切换级标量——计数/文案变化归数据面 tick，不逐帧比
bool PanelState::liveStateChangedLocked() const {
    if (!pub_) return true;
    return pub_->gateEnabled != live_.gateEnabled ||
           pub_->storesOk != live_.storesOk ||
           pub_->storesError != live_.storesError ||
           pub_->engine.state != live_.engine.state ||
           pub_->engine.pythonFound != live_.engine.pythonFound ||
           pub_->engine.llamaMissing != live_.engine.llamaMissing ||
           pub_->engine.embedReady != live_.engine.embedReady ||
           pub_->engine.embedState != live_.engine.embedState;
}

void PanelState::pollUsageAlerts() {
    if (!ledger_.has_value()) return;
    const auto now = static_cast<std::int64_t>(std::time(nullptr));
    if (now - lastAlertPollSec_ < kUsageAlertPollSec) return;
    lastAlertPollSec_ = now;
    // 直读盘而非 live_ 缓存：记忆页不可见时 refresh 不跑，阈值改动也要即时生效
    auto settings = loadPanelSettings();
    // need:380 B-5：单开关关闭 = 静默（闸态原样保留，重开即恢复判定）
    if (!settings.usageAlertsEnabled) return;
    const proposals::UsageAlertGate dayBefore = settings.usageDayGate;
    const proposals::UsageAlertGate softBefore = settings.usageSoftGate;
    const proposals::UsageAlertGate hardBefore = settings.usageHardGate;
    proposals::UsageAlertThresholds th;
    th.dayTokens = settings.usageDayAlertTokens;
    th.monthSoftTokens = settings.usageSoftBudgetTokens;
    th.monthHardTokens = settings.usageHardBudgetTokens;
    const auto num = [](core::Result<long long> r) {
        return r.isOk() ? r.value() : 0;
    };
    const long long todayTk = num(ledger_->todayTokens(now));
    const long long monthTk = num(ledger_->monthTokens(now));
    const long long dayKey = proposals::localDayKey(now);
    const auto fires = proposals::evaluateUsageAlert(
        th, todayTk, monthTk, dayKey, settings.usageDayGate,
        settings.usageSoftGate, settings.usageHardGate);
    const bool gateMoved =
        settings.usageDayGate.armed != dayBefore.armed ||
        settings.usageDayGate.lastFireDay != dayBefore.lastFireDay ||
        settings.usageSoftGate.armed != softBefore.armed ||
        settings.usageSoftGate.lastFireDay != softBefore.lastFireDay ||
        settings.usageHardGate.armed != hardBefore.armed ||
        settings.usageHardGate.lastFireDay != hardBefore.lastFireDay;
    if (gateMoved) {
        // 闸态迁移即落盘（击发卸武/回落重武装）：重启不重复轰炸、也不丢解除态
        if (savePanelSettings(settings).isFail()) {
            LOG_WARN("[usage-alert] 闸状态落盘失败，下次判定按旧闸重算");
        }
        std::lock_guard<std::mutex> lk(mu_);
        live_.settings.usageDayGate = settings.usageDayGate;
        live_.settings.usageSoftGate = settings.usageSoftGate;
        live_.settings.usageHardGate = settings.usageHardGate;
    }
    for (const auto& f : fires) {
        const std::string title = proposals::usageAlertTitle(f.tier);
        const std::string body = proposals::usageAlertBody(f);
        LOG_INFO("[memory][usage-alert] " + title + "：" + body);
        // 只走系统级（bug179 对齐 agent 侧 161 口径：去应用内 toast 防同事件双发；
        // 硬档 Error / 其余 Warning；facade 自带降级与限速，Accepted ≠ 看到）
        platform::notify::Notification note;
        note.id = std::string("memory.usage_alert.") +
                  proposals::usageAlertTierTag(f.tier) + "." +
                  std::to_string(dayKey);
        note.title = title;
        note.body = body;
        note.level = f.tier == proposals::UsageAlertTier::MonthHard
                         ? platform::notify::Level::Error
                         : platform::notify::Level::Warning;
        if (auto r = platform::notify::send(note); r.isFail()) {
            LOG_WARN("[memory][usage-alert] 系统通知未受理: " +
                     r.errorMessage());
        }
    }
}

std::int64_t PanelState::dirSizeBytes() const {
    // 记忆数据体积：排除 venv（引擎运行时，非记忆资产）
    std::error_code ec;
    std::int64_t total = 0;
    const std::filesystem::path root = dbDir();
    std::filesystem::recursive_directory_iterator it(
        root, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) return -1;
    for (; it != std::filesystem::recursive_directory_iterator();
         it.increment(ec)) {
        if (ec) break;
        if (it->is_directory(ec) && it->path().filename() == "venv") {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(ec)) {
            total += static_cast<std::int64_t>(it->file_size(ec));
        }
    }
    return total;
}

std::shared_ptr<const PanelSnapshot> PanelState::snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    return pub_;  // 只拷指针（无变化帧零拷贝）；快照对象发布后不再改
}

void PanelState::setSanitizeEnabled(bool on) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        // 闸翻转必须在锁内：采集循环每轮锁内拷贝闸快照（跨线程可见性）
        gate_.setEnabled(on);
        live_.settings.sanitizeEnabled = on;
        live_.gateEnabled = on;
    }
    auto s = loadPanelSettings();
    s.sanitizeEnabled = on;
    if (auto sr = savePanelSettings(s); sr.isFail()) {
        LOG_WARN("面板设置落盘失败（sanitize）: " + sr.errorMessage());
    }
    refresh(true);
}

void PanelState::setRecallThresholdPct(int pct) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        live_.settings.recallThresholdPct = pct;
        if (auto sr = savePanelSettings(live_.settings); sr.isFail()) {
            LOG_WARN("面板设置落盘失败: " + sr.errorMessage());
        }
    }
    refresh(true);
}

void PanelState::setTwoWordBaseline(int n) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        live_.settings.twoWordBaseline = n;
        if (auto sr = savePanelSettings(live_.settings); sr.isFail()) {
            LOG_WARN("面板设置落盘失败: " + sr.errorMessage());
        }
    }
    refresh(true);
}

void PanelState::setUsageAlertsEnabled(bool on) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        live_.settings.usageAlertsEnabled = on;
        if (auto sr = savePanelSettings(live_.settings); sr.isFail()) {
            LOG_WARN("面板设置落盘失败: " + sr.errorMessage());
        }
    }
    refresh(true);
}

core::Result<void> PanelState::setPipMirror(const std::string& indexUrl,
                                            const std::string& extraIndexUrl) {
    const std::string index = core::str::trimAscii(indexUrl);
    const std::string extra = core::str::trimAscii(extraIndexUrl);
    core::Result<void> saved = core::Result<void>::ok();
    {
        std::lock_guard<std::mutex> lk(mu_);
        live_.settings.pipIndexUrl = index;
        live_.settings.pipExtraIndexUrl = extra;
        saved = savePanelSettings(live_.settings);
        if (saved.isFail()) {
            LOG_WARN("面板设置落盘失败（镜像源）: " + saved.errorMessage());
        }
    }
    // 刷新即把落盘值同步到安装引擎（落盘失败则回退文件旧值，UI 由返回值提示）
    refresh(true);
    return saved;
}

void PanelState::markLlmImported(const std::string& profileName) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        live_.settings.llmImportedAtSec =
            static_cast<std::int64_t>(std::time(nullptr));
        live_.settings.llmProfileName = profileName;
        if (auto sr = savePanelSettings(live_.settings); sr.isFail()) {
            LOG_WARN("面板设置落盘失败: " + sr.errorMessage());
        }
    }
    refresh(true);
}

core::Result<void> PanelState::acceptProposal(const std::string& id) {
    if (!proposalStore_.has_value()) {
        return core::Result<void>::fail(core::ErrorCode::Internal, "提案库未打开");
    }
    auto current = proposalStore_->get(id);
    if (current.isFail()) {
        return core::propagate<void>(current);
    }
    std::string executorError;
    auto decided = proposalStore_->accept(
        id, static_cast<std::int64_t>(std::time(nullptr)),
        realTargetVersion(current.value().baseVersion), makeExecutor(),
        &executorError);
    if (decided.isFail()) {
        return core::propagate<void>(decided);
    }
    if (decided.value().appliedAt <= 0) {
        // 代理执行失败：保持待审 + 失败原因进审计（面板可见，重试 = 再接受）。
        // bug254：文案带执行器真实原因（embedding 500/400 等），不再一律
        // 「引擎未运行或不可达」。
        refresh(true);
        return core::Result<void>::fail(
            core::ErrorCode::Internal,
            "引擎执行失败：" +
                (executorError.empty() ? std::string("引擎未运行或不可达")
                                       : executorError) +
                "（提案保持待审，可再点接受重试）");
    }
    refresh(true);
    return core::Result<void>::ok();
}

core::Result<void> PanelState::rejectProposal(const std::string& id) {
    if (!proposalStore_.has_value()) {
        return core::Result<void>::fail(core::ErrorCode::Internal, "提案库未打开");
    }
    auto r = proposalStore_->reject(id,
                                    static_cast<std::int64_t>(std::time(nullptr)));
    if (r.isFail()) {
        return core::propagate<void>(r);
    }
    refresh(true);
    return core::Result<void>::ok();
}

core::Result<void> PanelState::editThenAccept(const std::string& id,
                                              const std::string& newText) {
    if (!proposalStore_.has_value()) {
        return core::Result<void>::fail(core::ErrorCode::Internal, "提案库未打开");
    }
    auto upd = proposalStore_->updateResultText(
        id, newText, static_cast<std::int64_t>(std::time(nullptr)));
    if (upd.isFail()) {
        return core::propagate<void>(upd);
    }
    return acceptProposal(id);
}

core::Result<std::string> PanelState::createUndoCard(
    const std::string& originalId) {
    if (!proposalStore_.has_value()) {
        return core::Result<std::string>::fail(core::ErrorCode::Internal,
                                               "提案库未打开");
    }
    auto undo = proposalStore_->createUndo(
        originalId, static_cast<std::int64_t>(std::time(nullptr)),
        realTargetVersion(-1));
    if (undo.isFail()) {
        return core::propagate<std::string>(undo);
    }
    refresh(true);
    return core::Result<std::string>::ok(undo.value().id);
}

core::Result<int> PanelState::batchAccept(const std::vector<std::string>& ids) {
    int okCount = 0;
    for (const auto& id : ids) {
        // 失败项保持待审（逐卡可见失败原因），成功计数返回
        if (acceptProposal(id).isOk()) ++okCount;
    }
    return core::Result<int>::ok(okCount);
}

bool PanelState::evalRunning() const { return evalRunning_.load(); }

core::Result<void> PanelState::startEvalAsync() {
    const auto engineState = engine::TelememEngine::global().snapshot().state;
    if (engineState != engine::TelememEngine::State::Running) {
        return core::Result<void>::fail(
            core::ErrorCode::ValidationError,
            "引擎未运行：评测需要检索引擎（先启动引擎再跑评测）");
    }
    bool expected = false;
    if (!evalRunning_.compare_exchange_strong(expected, true)) {
        return core::Result<void>::fail(core::ErrorCode::Internal, "评测已在进行中");
    }
    std::thread([this]() {
        gateway::TelememHttpRetriever engine;
        auto result = runFrozenEval(engine);
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (result.isOk() && evalStore_.has_value()) {
                (void)evalStore_->save(result.value());
                live_.lastEval = result.value();
                live_.lastEvalError.clear();  // F5：成功即清上次失败痕迹
            } else if (result.isFail()) {
                LOG_WARN("触发线评测失败: " + result.errorMessage());
                live_.lastEvalError = result.errorMessage();  // F5：UI 零反馈缺陷修复
            }
        }
        evalRunning_.store(false);
        needPublish_.store(true);  // 下一帧强制刷新读数
    }).detach();
    return core::Result<void>::ok();
}

core::Result<std::string> PanelState::exportAllToJson() {
    if (!proposalStore_.has_value() || !ledger_.has_value()) {
        return core::Result<std::string>::fail(core::ErrorCode::Internal,
                                               "面板库未打开");
    }
    // 导出面 = 自家库全部（提案 + 账面 + 设置）。引擎侧记忆清单无列举缝
    //（telemem 未暴露 list），不在本导出内——面板文案如实声明该边界。
    platform::json::JsonObject root;
    root.set("schema", platform::json::JsonValue("devdesk-memory-export"));
    root.set("exportedAt",
             platform::json::JsonValue(static_cast<std::int64_t>(std::time(nullptr))));

    auto proposals = platform::json::makeArray();
    auto appendProposal = [&](const Proposal& p) {
        platform::json::JsonObject o;
        o.set("id", platform::json::JsonValue(p.id));
        o.set("kind", platform::json::JsonValue(p.kind));
        o.set("status", platform::json::JsonValue(p.status));
        o.set("resultText", platform::json::JsonValue(p.resultText));
        o.set("resultType", platform::json::JsonValue(p.resultType));
        o.set("confidence", platform::json::JsonValue(p.confidence));
        o.set("source", platform::json::JsonValue(p.source));
        o.set("sourceTrust", platform::json::JsonValue(p.sourceTrust));
        o.set("createdAt", platform::json::JsonValue(p.createdAt));
        o.set("decidedAt", platform::json::JsonValue(p.decidedAt));
        o.set("appliedAt", platform::json::JsonValue(p.appliedAt));
        o.set("undoOf", platform::json::JsonValue(p.undoOf));
        auto ev = platform::json::makeArray();
        for (const auto& e : p.evidence) {
            platform::json::JsonObject eo;
            eo.set("sessionId", platform::json::JsonValue(e.sessionId));
            eo.set("observedAt", platform::json::JsonValue(e.observedAt));
            eo.set("quoted", platform::json::JsonValue(e.quoted));
            ev.append(platform::json::JsonValue(eo));
        }
        o.set("evidence", ev);
        proposals.append(platform::json::JsonValue(o));
    };
    if (auto pend = proposalStore_->listPending(); pend.isOk()) {
        for (const auto& p : pend.value()) appendProposal(p);
    }
    if (auto dec = proposalStore_->listRecentDecided(1000); dec.isOk()) {
        for (const auto& p : dec.value()) appendProposal(p);
    }
    root.set("proposals", proposals);

    auto usage = platform::json::makeArray();
    if (auto rows = ledger_->recentRows(10000); rows.isOk()) {
        for (const auto& r : rows.value()) {
            platform::json::JsonObject o;
            o.set("ts", platform::json::JsonValue(r.ts));
            o.set("kind", platform::json::JsonValue(r.kind));
            o.set("tokens", platform::json::JsonValue(r.tokens));
            o.set("costMilliYuan", platform::json::JsonValue(r.costMilliYuan));
            usage.append(platform::json::JsonValue(o));
        }
    }
    root.set("usage", usage);

    auto text = platform::json::stringify(platform::json::JsonValue(root), true);
    if (text.isFail()) {
        return core::propagate<std::string>(text);
    }
    // 落用户选定目录（原生目录选择对话框；取消 = fail 原样上抛）
    auto dir =
        platform::folderpicker::pickFolder("选择记忆导出目录", dbDir().string());
    if (dir.isFail()) {
        return core::propagate<std::string>(dir);
    }
    char stamp[24];
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    core::time::localTime(t, &tm);
    std::snprintf(stamp, sizeof(stamp), "%04d%02d%02d-%02d%02d%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                  tm.tm_min, tm.tm_sec);
    const std::filesystem::path out = std::filesystem::path(dir.value()) /
                                      ("devdesk-memory-" + std::string(stamp) +
                                       ".json");
    // ofstream 直收 fs::path（Windows 宽字符原生）：禁 path::string() 喂 fopen，
    // 窄字节被按 ACP 解释，非 ASCII 目录创建导出文件必失败（bug480）
    std::ofstream stream(out, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
        return core::Result<std::string>::fail(core::ErrorCode::Internal,
                                               "导出文件创建失败");
    }
    stream.write(text.value().data(),
                 static_cast<std::streamsize>(text.value().size()));
    stream.close();
    return core::Result<std::string>::ok(out.string());
}

core::Result<void> PanelState::clearAllOwnStores() {
    // 自家库清空：关连接 → 删库文件 → 重建空库。引擎侧无列举/全清缝
    //（telemem 未暴露 delete-all），A 阶段只能清自家账——面板文案如实声明。
    // 先停后台查询线程：reset 的 optional 与其查询竞争（下面 refresh(true) 重启）
    queryStop_.store(true);
    if (queryThread_.joinable()) queryThread_.join();
    proposalStore_.reset();
    ledger_.reset();
    collectorStore_.reset();
    inferQueue_.reset();
    evalStore_.reset();
    std::error_code ec;
    for (const char* name :
         {"proposals.db", "usage_ledger.db", "collector.db", "infer_queue.db",
          "recall_eval.db"}) {
        std::filesystem::remove(dbDir() / name, ec);
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        live_ = LiveState();
        live_.settings = loadPanelSettings();
        live_.gateEnabled = gate_.enabled();
        live_.engine = engine::TelememEngine::global().snapshot();
        fresh_.reset();  // 清旧结果，下面 refresh(true) 同步重建并发布
        dataBuilt_ = false;
    }
    openStores();
    refresh(true);
    return core::Result<void>::ok();
}

}  // namespace module::memory::panel
