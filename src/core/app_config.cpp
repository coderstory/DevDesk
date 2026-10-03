// src/core/app_config.cpp
// 通用「键 → JSON 值」配置存储（接口见 app_config.h）
//
// 一律经 SqliteDb 的公共入口落语句，**本文件不出现任何裸 sqlite3_* 调用**：
// 这样「每个句柄操作入锁」由 SqliteDb / Statement 一处兜住（含 finalize ——
// 那是 2026-10-02 真机 SIGSEGV 的头号漏点，见 sqlite_db.h 抬头）。
#include "app_config.h"
#include "time.h"

#include <ctime>
#include <utility>

namespace core::appconfig {

namespace {

// 写入时刻（unix 秒）。2026-10-03 轻量化：「core 没有『现在』的统一入口，各存储
// 层各自取系统时钟」这句话当时是事实、现在不是了 —— 入口补在 core/time.h，
// 4 份逐字相同的本地 nowUnix() 已收敛到那里。
using core::time::nowUnix;

constexpr const char* kSetSql =
    "INSERT INTO app_config (key, value, updated_at) VALUES (?, ?, ?) "
    "ON CONFLICT(key) DO UPDATE SET value = excluded.value, "
    "updated_at = excluded.updated_at";

} // namespace

Result<void> set(core::data::SqliteDb& db, const std::string& key,
                 const std::string& value) {
    auto prep = db.prepare(kSetSql);
    if (prep.isFail()) {
        return Result<void>::fail(prep.errorCode(), prep.errorMessage());
    }
    if (auto b = prep.value().bind(1, key); b.isFail()) return b;
    if (auto b = prep.value().bind(2, value); b.isFail()) return b;
    if (auto b = prep.value().bind(3, static_cast<int64_t>(nowUnix())); b.isFail()) {
        return b;
    }
    auto stepped = prep.value().step();
    if (stepped.isFail()) {
        return Result<void>::fail(stepped.errorCode(), stepped.errorMessage());
    }
    return Result<void>::ok();
}

Result<void> setMany(core::data::SqliteDb& db,
                     const std::vector<Entry>& entries) {
    if (entries.empty()) return Result<void>::ok();
    // 整批一个事务：要么全成要么全不成，且别的线程在这段期间被 transaction()
    // 持有的锁整段挡住（读不到「只写了一半的键」这种中间态）。
    return db.transaction([&]() -> Result<void> {
        const auto now = static_cast<int64_t>(nowUnix());
        auto prep = db.prepare(kSetSql);
        if (prep.isFail()) {
            return Result<void>::fail(prep.errorCode(), prep.errorMessage());
        }
        for (const auto& e : entries) {
            if (auto b = prep.value().bind(1, e.key); b.isFail()) return b;
            if (auto b = prep.value().bind(2, e.value); b.isFail()) return b;
            if (auto b = prep.value().bind(3, now); b.isFail()) return b;
            auto stepped = prep.value().step();
            if (stepped.isFail()) {
                return Result<void>::fail(stepped.errorCode(),
                                          stepped.errorMessage());
            }
            // 复位后才能安全地绑下一行（复用同一条语句，免每行一次 prepare）
            auto reset = prep.value().reset();
            if (reset.isFail()) {
                return Result<void>::fail(reset.errorCode(), reset.errorMessage());
            }
        }
        return Result<void>::ok();
    });
}

Result<std::string> get(core::data::SqliteDb& db, const std::string& key) {
    auto prep = db.prepare("SELECT value FROM app_config WHERE key = ?");
    if (prep.isFail()) {
        return Result<std::string>::fail(prep.errorCode(), prep.errorMessage());
    }
    if (auto b = prep.value().bind(1, key); b.isFail()) {
        return Result<std::string>::fail(b.errorCode(), b.errorMessage());
    }
    auto stepped = prep.value().step();
    if (stepped.isFail()) {
        return Result<std::string>::fail(stepped.errorCode(), stepped.errorMessage());
    }
    if (!stepped.value()) {
        // 键不存在 ≠ 值为空串：前者是「没配过」，后者是合法的配置值
        return Result<std::string>::fail(ErrorCode::NotFound,
                                         "app_config 无此键: " + key);
    }
    auto row = prep.value().currentRow();
    if (row.isFail()) {
        return Result<std::string>::fail(row.errorCode(), row.errorMessage());
    }
    return row.value().getString("value");
}

Result<bool> has(core::data::SqliteDb& db, const std::string& key) {
    auto prep = db.prepare("SELECT 1 FROM app_config WHERE key = ?");
    if (prep.isFail()) {
        return Result<bool>::fail(prep.errorCode(), prep.errorMessage());
    }
    if (auto b = prep.value().bind(1, key); b.isFail()) {
        return Result<bool>::fail(b.errorCode(), b.errorMessage());
    }
    auto stepped = prep.value().step();
    if (stepped.isFail()) {
        return Result<bool>::fail(stepped.errorCode(), stepped.errorMessage());
    }
    return Result<bool>::ok(stepped.value());
}

Result<void> erase(core::data::SqliteDb& db, const std::string& key) {
    auto prep = db.prepare("DELETE FROM app_config WHERE key = ?");
    if (prep.isFail()) {
        return Result<void>::fail(prep.errorCode(), prep.errorMessage());
    }
    if (auto b = prep.value().bind(1, key); b.isFail()) return b;
    auto stepped = prep.value().step();
    if (stepped.isFail()) {
        return Result<void>::fail(stepped.errorCode(), stepped.errorMessage());
    }
    return Result<void>::ok();
}

Result<std::vector<Entry>> listWithPrefix(core::data::SqliteDb& db,
                                          const std::string& prefix) {
    auto prep = db.prepare(
        "SELECT key, value, updated_at FROM app_config "
        "WHERE key LIKE ? ESCAPE '\\' ORDER BY key");
    if (prep.isFail()) {
        return Result<std::vector<Entry>>::fail(prep.errorCode(),
                                                prep.errorMessage());
    }
    // LIKE 的元字符（% / _ / \）在键名里没有语义，但配置键一旦含它们就会
    // 变成通配符匹配到别人的行——显式转义，前缀按字面比。
    std::string pattern;
    pattern.reserve(prefix.size() * 2);
    for (char c : prefix) {
        if (c == '%' || c == '_' || c == '\\') pattern.push_back('\\');
        pattern.push_back(c);
    }
    pattern += "%";
    if (auto b = prep.value().bind(1, pattern); b.isFail()) {
        return Result<std::vector<Entry>>::fail(b.errorCode(), b.errorMessage());
    }
    auto rows = prep.value().fetchAll();
    if (rows.isFail()) {
        return Result<std::vector<Entry>>::fail(rows.errorCode(),
                                                rows.errorMessage());
    }
    std::vector<Entry> out;
    out.reserve(rows.value().size());
    for (const auto& row : rows.value()) {
        Entry e;
        e.key = row.getString("key").valueOr(std::string());
        e.value = row.getString("value").valueOr(std::string());
        e.updatedAt = row.getInt64("updated_at").valueOr(0);
        out.push_back(std::move(e));
    }
    return Result<std::vector<Entry>>::ok(std::move(out));
}

Result<void> eraseWithPrefixExcept(core::data::SqliteDb& db,
                                   const std::string& prefix,
                                   const std::vector<std::string>& keep) {
    std::string pattern;
    pattern.reserve(prefix.size() * 2);
    for (char c : prefix) {
        if (c == '%' || c == '_' || c == '\\') pattern.push_back('\\');
        pattern.push_back(c);
    }
    pattern += "%";

    // 整段一个事务：与调用方的 setMany 组合成「重写一份配置」的原子步骤
    return db.transaction([&]() -> Result<void> {
        std::string sql =
            "DELETE FROM app_config WHERE key LIKE ? ESCAPE '\\'";
        if (!keep.empty()) {
            sql += " AND key NOT IN (";
            for (size_t i = 0; i < keep.size(); ++i) {
                sql += (i == 0) ? "?" : ", ?";
            }
            sql += ")";
        }
        auto prep = db.prepare(sql);
        if (prep.isFail()) {
            return Result<void>::fail(prep.errorCode(), prep.errorMessage());
        }
        if (auto b = prep.value().bind(1, pattern); b.isFail()) return b;
        for (size_t i = 0; i < keep.size(); ++i) {
            if (auto b = prep.value().bind(static_cast<int>(i + 2), keep[i]);
                b.isFail()) {
                return b;
            }
        }
        auto stepped = prep.value().step();
        if (stepped.isFail()) {
            return Result<void>::fail(stepped.errorCode(), stepped.errorMessage());
        }
        return Result<void>::ok();
    });
}

} // namespace core::appconfig
