// src/module_memory/panel/panel_state.cpp
#include "panel_state.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <thread>

#include "core/logging.h"
#include "core/time.h"
#include "platform/folder_picker.h"
#include "platform/json.h"
#include "module_memory/engine/telemem_config.h"
#include "module_memory/proposals/curator.h"

namespace module::memory::panel {

namespace {

using proposals::Proposal;
using proposals::ProposalExecutor;
using proposals::ProposalStore;
using proposals::TargetVersionFn;
using proposals::UsageLedger;

std::filesystem::path dbDir() { return engine::memoryDataDir(); }

// A 阶段引擎无版本查询缝（头文件契约说明）：以提案快照版本放行
TargetVersionFn trustSnapshotVersion(std::int64_t baseVersion) {
    return [baseVersion](std::int64_t) {
        return core::Result<std::int64_t>::ok(baseVersion);
    };
}

// 代理执行器：真接检索接口缝（引擎不在 = 执行失败，提案保持待审）
ProposalExecutor makeExecutor() {
    return [](const Proposal& p) -> core::Result<std::string> {
        gateway::TelememHttpRetriever engine;
        return proposals::executeProposal(p, engine);
    };
}

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

PanelState::~PanelState() = default;

void PanelState::openStores() {
    std::error_code ec;
    std::filesystem::create_directories(dbDir(), ec);
    auto prop = ProposalStore::open(dbDir() / "proposals.db");
    auto led = UsageLedger::open(dbDir() / "usage_ledger.db");
    auto col = collector::CollectorStore::open(dbDir() / "collector.db");
    auto ev = RecallEvalStore::open(dbDir() / "recall_eval.db");
    std::string err;
    if (prop.isOk()) proposalStore_.emplace(std::move(prop.value()));
    else err += "提案库打开失败；";
    if (led.isOk()) ledger_.emplace(std::move(led.value()));
    else err += "账本打开失败；";
    if (col.isOk()) collectorStore_.emplace(std::move(col.value()));
    else err += "采集账本打开失败；";
    if (ev.isOk()) evalStore_.emplace(std::move(ev.value()));
    else err += "评测库打开失败；";
    std::lock_guard<std::mutex> lk(mu_);
    snap_.storesOk = err.empty();
    snap_.storesError = err;
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
    gate_.setEnabled(on);
    {
        std::lock_guard<std::mutex> lk(mu_);
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
        trustSnapshotVersion(current.value().baseVersion), makeExecutor());
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

core::Result<std::string> PanelState::createUndoCard(
    const std::string& originalId) {
    if (!proposalStore_.has_value()) {
        return core::Result<std::string>::fail(core::ErrorCode::Internal,
                                               "提案库未打开");
    }
    auto undo = proposalStore_->createUndo(
        originalId, static_cast<std::int64_t>(std::time(nullptr)),
        trustSnapshotVersion(-1));
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
            } else if (result.isFail()) {
                LOG_WARN("触发线评测失败: " + result.errorMessage());
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
    evalStore_.reset();
    std::error_code ec;
    for (const char* name :
         {"proposals.db", "usage_ledger.db", "collector.db", "recall_eval.db"}) {
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
