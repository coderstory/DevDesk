// src/module_memory/pipeline/infer_queue.cpp
#include "infer_queue.h"

#include <utility>

#include "core/logging.h"
#include "core/sqlite_db.h"

namespace module::memory::pipeline {

namespace {

constexpr const char* kSchema = R"(
CREATE TABLE IF NOT EXISTS infer_queue (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id TEXT NOT NULL,
  start_seq INTEGER NOT NULL,
  text TEXT NOT NULL,
  created_at INTEGER NOT NULL,
  confidence REAL NOT NULL DEFAULT 0.3,
  attempts INTEGER NOT NULL DEFAULT 0,
  last_error TEXT NOT NULL DEFAULT '',
  UNIQUE(session_id, start_seq)
);
)";

}  // namespace

core::Result<InferQueueStore> InferQueueStore::open(
    const std::filesystem::path& dbFile) {
    auto db = core::data::SqliteDb::open(dbFile);
    if (db.isFail()) {
        return core::Result<InferQueueStore>::fail(db.errorCode(),
                                                   db.errorMessage());
    }
    auto applied = db.value().applySchema(kSchema);
    if (applied.isFail()) {
        return core::Result<InferQueueStore>::fail(applied.errorCode(),
                                                   applied.errorMessage());
    }
    // 腿 A 老库补列：applySchema 对已存在表是 no-op，波 2 之前的库缺
    // confidence/attempts/last_error。ALTER 失败的唯一现实原因是列已存在
    //（幂等重放），WARN 留痕不阻断——真写异常会在后续 enqueue/step 如实红
    for (const char* ddl : {
             "ALTER TABLE infer_queue ADD COLUMN confidence REAL NOT NULL "
             "DEFAULT 0.3",
             "ALTER TABLE infer_queue ADD COLUMN attempts INTEGER NOT NULL "
             "DEFAULT 0",
             "ALTER TABLE infer_queue ADD COLUMN last_error TEXT NOT NULL "
             "DEFAULT ''"}) {
        if (auto m = db.value().exec(ddl); m.isFail()) {
            LOG_WARN("提炼队列补列跳过（列已存在或不可加）: " +
                     m.errorMessage());
        }
    }
    InferQueueStore store;
    store.db_ = std::make_shared<core::data::SqliteDb>(std::move(db.value()));
    return core::Result<InferQueueStore>::ok(std::move(store));
}

core::Result<void> InferQueueStore::enqueue(const std::string& sessionId,
                                            std::int64_t startSeq,
                                            const std::string& text,
                                            std::int64_t nowSec,
                                            double confidence) {
    auto stmt = db_->prepare(
        "INSERT OR IGNORE INTO infer_queue(session_id, start_seq, text, "
        "created_at, confidence) VALUES(?, ?, ?, ?, ?)");
    if (stmt.isFail()) {
        return core::Result<void>::fail(stmt.errorCode(), stmt.errorMessage());
    }
    if (!stmt.value().bind(1, sessionId).isOk() ||
        !stmt.value().bind(2, startSeq).isOk() ||
        !stmt.value().bind(3, text).isOk() ||
        !stmt.value().bind(4, nowSec).isOk() ||
        !stmt.value().bind(5, confidence).isOk()) {
        return core::Result<void>::fail(core::ErrorCode::Internal,
                                        "提炼队列入队绑定失败");
    }
    auto step = stmt.value().step();
    if (step.isFail()) {
        return core::Result<void>::fail(step.errorCode(), step.errorMessage());
    }
    return core::Result<void>::ok();
}

core::Result<std::vector<InferQueueStore::Entry>> InferQueueStore::listBatch(
    int limit) const {
    auto stmt = db_->prepare(
        "SELECT id, session_id, start_seq, text, attempts, last_error, "
        "confidence FROM infer_queue ORDER BY id LIMIT ?");
    if (stmt.isFail()) {
        return core::Result<std::vector<Entry>>::fail(stmt.errorCode(),
                                                      stmt.errorMessage());
    }
    if (!stmt.value().bind(1, static_cast<std::int64_t>(limit)).isOk()) {
        return core::Result<std::vector<Entry>>::fail(core::ErrorCode::Internal,
                                                      "提炼队列取批绑定失败");
    }
    std::vector<Entry> out;
    for (;;) {
        auto step = stmt.value().step();
        if (step.isFail()) {
            return core::Result<std::vector<Entry>>::fail(step.errorCode(),
                                                          step.errorMessage());
        }
        if (!step.value()) break;
        auto row = stmt.value().currentRow();
        if (row.isFail()) {
            return core::Result<std::vector<Entry>>::fail(row.errorCode(),
                                                          row.errorMessage());
        }
        Entry e;
        e.id = row.value().getInt64("id").valueOr(0);
        e.sessionId = row.value().getString("session_id").valueOr("");
        e.startSeq = row.value().getInt64("start_seq").valueOr(0);
        e.text = row.value().getString("text").valueOr("");
        e.attempts =
            static_cast<int>(row.value().getInt64("attempts").valueOr(0));
        e.lastError = row.value().getString("last_error").valueOr("");
        e.confidence =
            row.value().getDouble("confidence").valueOr(kQueueDefaultConfidence);
        out.push_back(std::move(e));
    }
    return core::Result<std::vector<Entry>>::ok(std::move(out));
}

core::Result<int> InferQueueStore::depth() const {
    auto stmt = db_->prepare("SELECT COUNT(*) AS v FROM infer_queue");
    if (stmt.isFail()) {
        return core::Result<int>::fail(stmt.errorCode(), stmt.errorMessage());
    }
    auto step = stmt.value().step();
    if (step.isFail() || !step.value()) {
        return core::Result<int>::ok(0);
    }
    auto row = stmt.value().currentRow();
    if (row.isFail()) return core::Result<int>::ok(0);
    return core::Result<int>::ok(
        static_cast<int>(row.value().getInt64("v").valueOr(0)));
}

core::Result<int> InferQueueStore::recordFailure(std::int64_t id,
                                                 const std::string& error) {
    auto upd = db_->prepare(
        "UPDATE infer_queue SET attempts = attempts + 1, last_error = ? "
        "WHERE id = ?");
    if (upd.isFail()) {
        return core::Result<int>::fail(upd.errorCode(), upd.errorMessage());
    }
    const std::string brief = error.size() > 200 ? error.substr(0, 200) : error;
    if (!upd.value().bind(1, brief).isOk() ||
        !upd.value().bind(2, id).isOk()) {
        return core::Result<int>::fail(core::ErrorCode::Internal,
                                       "提炼队列失败记账绑定失败");
    }
    if (auto s = upd.value().step(); s.isFail()) {
        return core::Result<int>::fail(s.errorCode(), s.errorMessage());
    }
    auto q = db_->prepare("SELECT attempts FROM infer_queue WHERE id = ?");
    if (q.isFail()) {
        return core::Result<int>::fail(q.errorCode(), q.errorMessage());
    }
    if (!q.value().bind(1, id).isOk()) {
        return core::Result<int>::fail(core::ErrorCode::Internal,
                                       "提炼队列失败计数查询绑定失败");
    }
    auto step = q.value().step();
    if (step.isFail() || !step.value()) {
        return core::Result<int>::fail(core::ErrorCode::NotFound,
                                       "提炼队列条目不存在: " +
                                           std::to_string(id));
    }
    auto row = q.value().currentRow();
    if (row.isFail()) {
        return core::Result<int>::fail(row.errorCode(), row.errorMessage());
    }
    return core::Result<int>::ok(
        static_cast<int>(row.value().getInt64("attempts").valueOr(0)));
}

core::Result<void> InferQueueStore::remove(std::int64_t id) {
    auto stmt = db_->prepare("DELETE FROM infer_queue WHERE id = ?");
    if (stmt.isFail()) {
        return core::Result<void>::fail(stmt.errorCode(), stmt.errorMessage());
    }
    if (!stmt.value().bind(1, id).isOk()) {
        return core::Result<void>::fail(core::ErrorCode::Internal,
                                        "提炼队列出队绑定失败");
    }
    auto step = stmt.value().step();
    if (step.isFail()) {
        return core::Result<void>::fail(step.errorCode(), step.errorMessage());
    }
    return core::Result<void>::ok();
}

}  // namespace module::memory::pipeline
