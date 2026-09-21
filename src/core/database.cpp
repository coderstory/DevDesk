// src/core/database.cpp
#include "database.h"
#include "logging.h"
#include "migrations.h"

#include <atomic>
#include <ctime>

#include <sqlite3.h>

namespace core {

Database::Database(core::data::SqliteDb db, std::vector<core::data::Migration> migrations)
    : db_(std::move(db)), migrations_(std::move(migrations)) {}

Database::Database(Database&& other) noexcept
    : db_(std::move(other.db_)), migrations_(std::move(other.migrations_)) {}

Database& Database::operator=(Database&& other) noexcept {
    if (this != &other) {
        db_ = std::move(other.db_);
        migrations_ = std::move(other.migrations_);
    }
    return *this;
}

core::Result<Database> Database::open(const std::filesystem::path& path) {
    auto dbResult = core::data::SqliteDb::open(path);
    if (dbResult.isFail()) {
        return core::Result<Database>::fail(dbResult.errorCode(), dbResult.errorMessage());
    }

    std::vector<core::data::Migration> ms(migrations::all().begin(),
                                          migrations::all().end());

    // Result::value() 返回 T&（lvalue ref），用 std::move 转成 xvalue 触发 move ctor
    core::data::SqliteDb db(std::move(dbResult.value()));
    Database inst(std::move(db), std::move(ms));
    return core::Result<Database>::ok(std::move(inst));
}

core::Result<void> Database::migrate() {
    auto r = db_.runMigrations(migrations_);
    if (r.isFail()) {
        LOG_ERROR("Database migrate failed: " + r.errorMessage());
    }
    return r;
}

core::Result<int64_t> Database::pruneCallLogs(int retentionDays) {
    if (retentionDays < 0) retentionDays = 0;
    const int64_t cutoff = static_cast<int64_t>(std::time(nullptr)) -
                           static_cast<int64_t>(retentionDays) * 86400;

    // 持锁覆盖 step + sqlite3_changes，避免并发语句插队导致计数失真
    //（recursive_mutex，Statement 内部加锁可重入）
    std::lock_guard<std::recursive_mutex> lock(db_.mutex());

    auto prep = db_.prepare(
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
        static_cast<int64_t>(sqlite3_changes(db_.handle())));
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
