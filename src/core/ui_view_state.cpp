// src/core/ui_view_state.cpp（接口与设计理由见 ui_view_state.h）
#include "ui_view_state.h"

#include "app_config.h"
#include "logging.h"
#include "time.h"

namespace core::ui_view_state {

namespace {

constexpr const char* kSetSql =
    "INSERT INTO ui_view_state (module, value, updated_at) VALUES (?, ?, ?) "
    "ON CONFLICT(module) DO UPDATE SET value = excluded.value, "
    "updated_at = excluded.updated_at";

int64_t nowUnix() { return static_cast<int64_t>(core::time::nowUnix()); }

}  // namespace

Result<std::string> get(core::data::SqliteDb& db, const std::string& module) {
    auto prep = db.prepare("SELECT value FROM ui_view_state WHERE module = ?");
    if (prep.isFail()) return propagate<std::string>(prep);
    if (auto b = prep.value().bind(1, module); b.isFail()) {
        return propagate<std::string>(b);
    }
    auto stepped = prep.value().step();
    if (stepped.isFail()) return propagate<std::string>(stepped);
    if (!stepped.value()) {
        return Result<std::string>::fail(ErrorCode::NotFound,
                                         "ui_view_state 无此模块: " + module);
    }
    auto row = prep.value().currentRow();
    if (row.isFail()) return propagate<std::string>(row);
    return row.value().getString("value");
}

Result<bool> has(core::data::SqliteDb& db, const std::string& module) {
    auto r = get(db, module);
    if (r.isOk()) return Result<bool>::ok(true);
    if (r.errorCode() == ErrorCode::NotFound) return Result<bool>::ok(false);
    return Result<bool>::fail(r.errorCode(), r.errorMessage());
}

Result<void> set(core::data::SqliteDb& db, const std::string& module,
                 const std::string& value) {
    auto prep = db.prepare(kSetSql);
    if (prep.isFail()) return propagate<void>(prep);
    if (auto b = prep.value().bind(1, module); b.isFail()) return b;
    if (auto b = prep.value().bind(2, value); b.isFail()) return b;
    if (auto b = prep.value().bind(3, nowUnix()); b.isFail()) return b;
    auto stepped = prep.value().step();
    if (stepped.isFail()) return propagate<void>(stepped);
    return Result<void>::ok();
}

Result<void> erase(core::data::SqliteDb& db, const std::string& module) {
    auto prep = db.prepare("DELETE FROM ui_view_state WHERE module = ?");
    if (prep.isFail()) return propagate<void>(prep);
    if (auto b = prep.value().bind(1, module); b.isFail()) return b;
    auto stepped = prep.value().step();
    if (stepped.isFail()) return propagate<void>(stepped);
    return Result<void>::ok();
}

Result<std::string> getOrMigrate(core::data::SqliteDb& db,
                                 const std::string& module,
                                 const std::string& legacyKey) {
    auto cur = get(db, module);
    if (cur.isOk()) return cur;
    if (cur.errorCode() != ErrorCode::NotFound || legacyKey.empty()) return cur;
    // 老库迁移：把 app_config 里的旧键搬到本机表，再删旧键（停止跨机同步）。
    auto legacy = appconfig::get(db, legacyKey);
    if (legacy.isFail()) return cur;  // 旧键也没有 = 首次启动，保持 NotFound
    auto moved = set(db, module, legacy.value());
    if (moved.isFail()) {
        LOG_WARN("ui_view_state: 旧视图状态迁移失败（保留旧键）: " +
                 moved.errorMessage());
        return cur;
    }
    if (auto erased = appconfig::erase(db, legacyKey); erased.isFail()) {
        LOG_WARN("ui_view_state: 旧视图状态键删除失败（下次会再迁移一次）: " +
                 erased.errorMessage());
    }
    return legacy;
}

}  // namespace core::ui_view_state