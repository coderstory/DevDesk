// src/module_memory/panel/recall_eval.cpp
#include "recall_eval.h"
#include <utility>

#include <ctime>
#include <set>
#include <string>
#include <vector>

#include "core/logging.h"
#include "core/time.h"
#include "module_memory/gateway/telemem_client.h"
#include "module_memory/panel/recall_eval_data.h"

namespace module::memory::panel {

namespace {

using gateway::MemoryHit;

// 结果归一：命中条目 id 优先，缺 id 时按单行化正文对齐 doc 文本
std::string normalizeText(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\r' || c == '\n') c = ' ';
        out.push_back(c);
    }
    return out;
}

constexpr int kEvalSearchLimit = 5;   // Recall@5 口径
constexpr int kEvalTimeoutMs = 3000;  // 评测非 hook 通道：放宽到 3s/查询

}  // namespace

core::Result<RecallEvalStore> RecallEvalStore::open(
    const std::filesystem::path& dbFile) {
    auto db = core::data::SqliteDb::open(dbFile);
    if (db.isFail()) {
        return core::Result<RecallEvalStore>::fail(db.errorCode(),
                                                   db.errorMessage());
    }
    const auto applied = db.value().applySchema(
        "CREATE TABLE IF NOT EXISTS recall_eval ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " ts INTEGER NOT NULL, recall_pct INTEGER NOT NULL,"
        " hits INTEGER NOT NULL, queries INTEGER NOT NULL)");
    if (applied.isFail()) {
        return core::Result<RecallEvalStore>::fail(applied.errorCode(),
                                                   applied.errorMessage());
    }
    RecallEvalStore store;
    store.db_ = std::make_shared<core::data::SqliteDb>(std::move(db.value()));
    return core::Result<RecallEvalStore>::ok(std::move(store));
}

core::Result<void> RecallEvalStore::save(const RecallResult& result) {
    auto stmt = db_->prepare(
        "INSERT INTO recall_eval(ts, recall_pct, hits, queries) "
        "VALUES(?, ?, ?, ?)");
    if (stmt.isFail()) {
        return core::Result<void>::fail(stmt.errorCode(), stmt.errorMessage());
    }
    if (!stmt.value().bind(1, result.ts).isOk() ||
        !stmt.value()
             .bind(2, static_cast<std::int64_t>(result.recallPct))
             .isOk() ||
        !stmt.value().bind(3, static_cast<std::int64_t>(result.hits)).isOk() ||
        !stmt.value().bind(4, static_cast<std::int64_t>(result.queries))
             .isOk()) {
        return core::Result<void>::fail(core::ErrorCode::Internal,
                                        "评测结果绑定失败");
    }
    auto step = stmt.value().step();
    if (step.isFail()) {
        return core::Result<void>::fail(step.errorCode(), step.errorMessage());
    }
    return core::Result<void>::ok();
}

RecallResult RecallEvalStore::last() const {
    RecallResult r;
    auto stmt = db_->prepare(
        "SELECT ts, recall_pct, hits, queries FROM recall_eval "
        "ORDER BY ts DESC, id DESC LIMIT 1");
    if (stmt.isFail()) return r;
    auto step = stmt.value().step();
    if (!step.isOk() || !step.value()) return r;
    auto row = stmt.value().currentRow();
    if (row.isFail()) return r;
    r.ts = row.value().getInt64("ts").valueOr(0);
    r.recallPct = static_cast<int>(row.value().getInt64("recall_pct").valueOr(-1));
    r.hits = static_cast<int>(row.value().getInt64("hits").valueOr(0));
    r.queries = static_cast<int>(row.value().getInt64("queries").valueOr(0));
    return r;
}

core::Result<RecallResult> runFrozenEval(gateway::IMemoryRetriever& engine) {
    // ① 入库 27 条冻结 doc（直存语义；记录 doc 下标 → 引擎 id 映射）
    std::vector<std::string> docTexts(kFrozenDocCount);
    std::vector<std::string> docIds(kFrozenDocCount);
    for (std::size_t i = 0; i < kFrozenDocCount; ++i) {
        docTexts[i] = kFrozenDocs[i];
        auto added = engine.addDirect(kFrozenDocs[i]);
        docIds[i] = added.isOk() ? added.value() : std::string();
    }

    // ② 15 查询逐条检索，前 5 名命中任一 gold 记 1
    const auto now = static_cast<std::int64_t>(std::time(nullptr));
    int hits = 0;
    int failed = 0;
    for (const auto& q : kFrozenQueries) {
        auto result = engine.search(q.text, kEvalSearchLimit, kEvalTimeoutMs);
        if (result.isFail()) {
            ++failed;
            continue;  // 失败查询按未命中计，不虚增
        }
        std::set<std::string> goldIds;
        std::set<std::string> goldTexts;
        for (std::size_t g = 0; g < q.goldCount; ++g) {
            const int docIndex = q.goldIds[g];
            if (docIndex < 0 ||
                docIndex >= static_cast<int>(kFrozenDocCount)) continue;
            goldIds.insert(docIds[static_cast<std::size_t>(docIndex)]);
            goldTexts.insert(normalizeText(docTexts[static_cast<std::size_t>(docIndex)]));
        }
        bool hit = false;
        for (const auto& h : result.value()) {
            if (!h.memoryId.empty() && goldIds.count(h.memoryId) > 0) {
                hit = true;
                break;
            }
            if (!h.text.empty() && goldTexts.count(normalizeText(h.text)) > 0) {
                hit = true;
                break;
            }
            // telemem 1.10.1 融合段落形态（2026-10-28 live 定案）：前 5 名被空格
            // 连接成单串、无条目 id——gold 正文出现在融合段内 = 该 gold 在前 5 名
            //（Recall@5 等价口径；旧判定只认逐条 id/全文相等，对真引擎恒 0 命中）
            if (!h.text.empty()) {
                const std::string fusedText = normalizeText(h.text);
                for (const auto& gold : goldTexts) {
                    if (!gold.empty() && fusedText.find(gold) != std::string::npos) {
                        hit = true;
                        break;
                    }
                }
            }
            if (hit) break;
        }
        if (hit) ++hits;
    }
    if (failed == static_cast<int>(kFrozenQueryCount)) {
        return core::Result<RecallResult>::fail(
            core::ErrorCode::Internal, "引擎检索全部失败（引擎未运行或不可达）");
    }

    // ③ 尽力清理：本次入库条目移出「自动想起」（评测数据不进注入通道）
    for (std::size_t i = 0; i < kFrozenDocCount; ++i) {
        if (docIds[i].empty()) continue;
        auto marked = engine.setMemoryForgotten(docIds[i], true);
        if (marked.isFail()) {
            LOG_WARN("评测数据清理标记失败（尽力而为，不影响结果）");
        }
    }

    RecallResult r;
    r.ts = now;
    r.hits = hits;
    r.queries = static_cast<int>(kFrozenQueryCount);
    r.recallPct = hits * 100 / static_cast<int>(kFrozenQueryCount);
    return core::Result<RecallResult>::ok(r);
}

}  // namespace module::memory::panel
