// src/module_memory/pipeline/infer_queue.cpp
#include "infer_queue.h"

#include <utility>

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
    InferQueueStore store;
    store.db_ = std::make_shared<core::data::SqliteDb>(std::move(db.value()));
    return core::Result<InferQueueStore>::ok(std::move(store));
}

core::Result<void> InferQueueStore::enqueue(const std::string& sessionId,
                                            std::int64_t startSeq,
                                            const std::string& text,
                                            std::int64_t nowSec) {
    auto stmt = db_->prepare(
        "INSERT OR IGNORE INTO infer_queue(session_id, start_seq, text, "
        "created_at) VALUES(?, ?, ?, ?)");
    if (stmt.isFail()) {
        return core::Result<void>::fail(stmt.errorCode(), stmt.errorMessage());
    }
    if (!stmt.value().bind(1, sessionId).isOk() ||
        !stmt.value().bind(2, startSeq).isOk() ||
        !stmt.value().bind(3, text).isOk() ||
        !stmt.value().bind(4, nowSec).isOk()) {
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
        "SELECT id, session_id, text FROM infer_queue ORDER BY id LIMIT ?");
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
        e.text = row.value().getString("text").valueOr("");
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
