// src/core/database.cpp
#include "database.h"
#include "logging.h"
#include "migrations.h"

#include <atomic>

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
