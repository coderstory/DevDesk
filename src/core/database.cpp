// src/core/database.cpp
#include "database.h"
#include "logging.h"
#include "schema.h"

#include <atomic>
#include <ctime>
#include <utility>

#include <sqlite3.h>

namespace core {

Database::Database(core::data::SqliteDb db) : db_(std::move(db)) {}

Database::Database(Database&& other) noexcept : db_(std::move(other.db_)) {}

Database& Database::operator=(Database&& other) noexcept {
    if (this != &other) {
        db_ = std::move(other.db_);
    }
    return *this;
}

core::Result<Database> Database::open(const std::filesystem::path& path) {
    auto dbResult = core::data::SqliteDb::open(path);
    if (dbResult.isFail()) {
        return core::Result<Database>::fail(dbResult.errorCode(), dbResult.errorMessage());
    }

    // Result::value() 返回 T&（lvalue ref），用 std::move 转成 xvalue 触发 move ctor
    core::data::SqliteDb db(std::move(dbResult.value()));
    Database inst(std::move(db));
    return core::Result<Database>::ok(std::move(inst));
}

core::Result<void> Database::migrate() {
    auto r = db_.applySchema(core::schema::kSql);
    if (r.isFail()) {
        LOG_ERROR("Database schema 应用失败: " + r.errorMessage());
    }
    return r;
}

core::Result<int64_t> Database::pruneCallLogs(int retentionDays) {
    return core::pruneCallLogs(db_, retentionDays);
}

core::Result<int64_t> pruneCallLogs(core::data::SqliteDb& db, int retentionDays) {
    if (retentionDays < 0) retentionDays = 0;
    const int64_t cutoff = static_cast<int64_t>(std::time(nullptr)) -
                           static_cast<int64_t>(retentionDays) * 86400;

    // 持锁覆盖 step + sqlite3_changes，避免并发语句插队导致计数失真
    //（recursive_mutex，Statement 内部加锁可重入）
    std::lock_guard<std::recursive_mutex> lock(db.mutex());

    auto prep = db.prepare(
        "DELETE FROM claude_call_logs WHERE started_at < ?");
    if (prep.isFail()) {
        return core::Result<int64_t>::fail(prep.errorCode(), prep.errorMessage());
    }
    auto& stmt = prep.value();
    if (auto b = stmt.bind(1, cutoff); b.isFail()) {
        return core::Result<int64_t>::fail(b.errorCode(), b.errorMessage());
    }
    auto step = stmt.step();
    if (step.isFail()) {
        return core::Result<int64_t>::fail(step.errorCode(), step.errorMessage());
    }
    return core::Result<int64_t>::ok(
        static_cast<int64_t>(sqlite3_changes(db.handle())));
}

// ============ 进程级数据库共享访问器 ============

namespace {
std::atomic<core::data::SqliteDb*> g_appDatabase{nullptr};
} // namespace

void registerAppDatabase(core::data::SqliteDb* db) {
    g_appDatabase.store(db, std::memory_order_release);
}

core::data::SqliteDb* appDatabase() {
    return g_appDatabase.load(std::memory_order_acquire);
}

} // namespace core
