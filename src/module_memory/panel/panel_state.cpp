// src/module_memory/panel/panel_state.cpp
#include "panel_state.h"
#include <chrono>
#include <system_error>
#include <utility>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <thread>

#include "core/logging.h"
#include "core/time.h"
#include "platform/folder_picker.h"
#include "platform/json.h"
#include "platform/notification.h"
#include "toast_notice_bridge.h"
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
      }) {}

PanelState::~PanelState() {
    // F1 循环先停再拆成员：线程触碰 proposalStore_ 等 optional，必须先 join
    if (collectThread_.joinable()) {
        collectStop_.store(true);
        collectThread_.join();
    }
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
    snap_.storesOk = err.empty();
    snap_.storesError = err;
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
                    lastRefreshSec_ = 0;  // 有产出 → 下帧立刷动态流/审核数
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
                                                   *ledger_, limits, note);
                if (step.isFail()) {
                    LOG_WARN("提炼步失败: " + step.errorMessage());
                } else if (step.value().succeeded ||
                           step.value().attempted) {
                    lastRefreshSec_ = 0;  // 有动作 → 下帧立刷动态流/消耗卡
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
    const auto now = static_cast<std::int64_t>(std::time(nullptr));
    {
        std::lock_guard<std::mutex> lk(mu_);
        snap_.settings = loadPanelSettings();
        snap_.gateEnabled = gate_.enabled();
        snap_.engine = engine::TelememEngine::global().snapshot();
    }
    if (!std::filesystem::exists(dbDir())) {
        // 首跑：目录还没有 = 全零账面（不留白，不建库——等首跑向导）
        std::lock_guard<std::mutex> lk(mu_);
        snap_.storesOk = true;
        return;
    }
    if (!proposalStore_.has_value()) openStores();
    syncCollectLoop();  // F1：每帧同步（不受 1s 节流约束；引擎起停即刻响应）
    if (!force && now - lastRefreshSec_ < 1) return;
    lastRefreshSec_ = now;

    PanelSnapshot next;
    {
        std::lock_guard<std::mutex> lk(mu_);
        next = snap_;  // 携带 settings/engine/gate/stores 态
    }
    if (proposalStore_.has_value()) {
        if (auto pend = proposalStore_->listPending(); pend.isOk()) {
            next.pending = std::move(pend.value());
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
    if (now - lastSizeCheckSec_ >= 60) {
        lastSizeCheckSec_ = now;
        next.storageBytes = dirSizeBytes();
    } else {
        std::lock_guard<std::mutex> lk(mu_);
        next.storageBytes = snap_.storageBytes;
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

    std::lock_guard<std::mutex> lk(mu_);
    snap_ = std::move(next);
}

void PanelState::pollUsageAlerts() {
    if (!ledger_.has_value()) return;
    const auto now = static_cast<std::int64_t>(std::time(nullptr));
    if (now - lastAlertPollSec_ < kUsageAlertPollSec) return;
    lastAlertPollSec_ = now;
    // 直读盘而非 snap_ 缓存：记忆页不可见时 refresh 不跑，阈值改动也要即时生效
    auto settings = loadPanelSettings();
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
        snap_.settings.usageDayGate = settings.usageDayGate;
        snap_.settings.usageSoftGate = settings.usageSoftGate;
        snap_.settings.usageHardGate = settings.usageHardGate;
    }
    for (const auto& f : fires) {
        const std::string title = proposals::usageAlertTitle(f.tier);
        const std::string body = proposals::usageAlertBody(f);
        LOG_INFO("[memory][usage-alert] " + title + "：" + body);
        // 双出口都既有基建：应用级 toast（M3 同一 showAppNotice 落点，跨页可见）
        app::toast_notice::show(title, body);
        // 系统级（硬档 Error / 其余 Warning；facade 自带降级与限速，Accepted ≠ 看到）
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

PanelSnapshot PanelState::snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    return snap_;
}

void PanelState::setSanitizeEnabled(bool on) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        // 闸翻转必须在锁内：采集循环每轮锁内拷贝闸快照（跨线程可见性）
        gate_.setEnabled(on);
        snap_.settings.sanitizeEnabled = on;
        snap_.gateEnabled = on;
    }
    auto s = loadPanelSettings();
    s.sanitizeEnabled = on;
    savePanelSettings(s);
    refresh(true);
}

void PanelState::setRecallThresholdPct(int pct) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        snap_.settings.recallThresholdPct = pct;
        savePanelSettings(snap_.settings);
    }
    refresh(true);
}

void PanelState::setTwoWordBaseline(int n) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        snap_.settings.twoWordBaseline = n;
        savePanelSettings(snap_.settings);
    }
    refresh(true);
}

void PanelState::setUsageAlertThresholds(long long dayTokens,
                                         long long softTokens,
                                         long long hardTokens) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        // 值未变 no-op：滑条拖动每 tick 都回调，去重后拖动才落盘/立刷
        if (snap_.settings.usageDayAlertTokens == dayTokens &&
            snap_.settings.usageSoftBudgetTokens == softTokens &&
            snap_.settings.usageHardBudgetTokens == hardTokens) {
            return;
        }
        snap_.settings.usageDayAlertTokens = dayTokens;
        snap_.settings.usageSoftBudgetTokens = softTokens;
        snap_.settings.usageHardBudgetTokens = hardTokens;
        savePanelSettings(snap_.settings);
    }
    refresh(true);
}

void PanelState::markLlmImported(const std::string& profileName) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        snap_.settings.llmImportedAtSec =
            static_cast<std::int64_t>(std::time(nullptr));
        snap_.settings.llmProfileName = profileName;
        savePanelSettings(snap_.settings);
    }
    refresh(true);
}

core::Result<void> PanelState::acceptProposal(const std::string& id) {
    if (!proposalStore_.has_value()) {
        return core::Result<void>::fail(core::ErrorCode::Internal, "提案库未打开");
    }
    auto current = proposalStore_->get(id);
    if (current.isFail()) {
        return core::Result<void>::fail(current.errorCode(),
                                        current.errorMessage());
    }
    auto decided = proposalStore_->accept(
        id, static_cast<std::int64_t>(std::time(nullptr)),
        realTargetVersion(current.value().baseVersion), makeExecutor());
    if (decided.isFail()) {
        return core::Result<void>::fail(decided.errorCode(),
                                        decided.errorMessage());
    }
    if (decided.value().appliedAt <= 0) {
        // 代理执行失败：保持待审 + 失败原因进审计（面板可见，重试 = 再接受）
        refresh(true);
        return core::Result<void>::fail(
            core::ErrorCode::Internal,
            "引擎执行失败：提案保持待审（引擎未运行或不可达，可再点接受重试）");
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
        return core::Result<void>::fail(r.errorCode(), r.errorMessage());
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
        return core::Result<void>::fail(upd.errorCode(), upd.errorMessage());
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
        return core::Result<std::string>::fail(undo.errorCode(),
                                               undo.errorMessage());
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
                snap_.lastEval = result.value();
                snap_.lastEvalError.clear();  // F5：成功即清上次失败痕迹
            } else if (result.isFail()) {
                LOG_WARN("触发线评测失败: " + result.errorMessage());
                snap_.lastEvalError = result.errorMessage();  // F5：UI 零反馈缺陷修复
            }
        }
        evalRunning_.store(false);
        lastRefreshSec_ = 0;  // 下一帧强制刷新读数
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
        return core::Result<std::string>::fail(text.errorCode(),
                                               text.errorMessage());
    }
    // 落用户选定目录（原生目录选择对话框；取消 = fail 原样上抛）
    auto dir =
        platform::folderpicker::pickFolder("选择记忆导出目录", dbDir().string());
    if (dir.isFail()) {
        return core::Result<std::string>::fail(dir.errorCode(),
                                               dir.errorMessage());
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
    FILE* f = std::fopen(out.string().c_str(), "wb");
    if (f == nullptr) {
        return core::Result<std::string>::fail(core::ErrorCode::Internal,
                                               "导出文件创建失败");
    }
    std::fwrite(text.value().data(), 1, text.value().size(), f);
    std::fclose(f);
    return core::Result<std::string>::ok(out.string());
}

core::Result<void> PanelState::clearAllOwnStores() {
    // 自家库清空：关连接 → 删库文件 → 重建空库。引擎侧无列举/全清缝
    //（telemem 未暴露 delete-all），A 阶段只能清自家账——面板文案如实声明。
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
        snap_ = PanelSnapshot();
        snap_.settings = loadPanelSettings();
        snap_.gateEnabled = gate_.enabled();
        snap_.engine = engine::TelememEngine::global().snapshot();
    }
    openStores();
    refresh(true);
    return core::Result<void>::ok();
}

}  // namespace module::memory::panel
