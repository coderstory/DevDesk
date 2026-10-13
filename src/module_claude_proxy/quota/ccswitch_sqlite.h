// src/module_claude_proxy/quota/ccswitch_sqlite.h
// cc-switch 只读库读取的共用脚手架（RAII / 探列 / 文本列 / 只读打开），被 ccswitch_source.cpp 与 ccswitch_import.cpp 共用。
// 只读硬约束见 ccswitch_source.h（恒 SQLITE_OPEN_READONLY，不建表、不写 schema、不动 -wal/-shm）。
#pragma once

#include <sqlite3.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "ccswitch_source.h"  // kCcSwitchOpenFlags / ccSwitchDbPath（同目录相对）

namespace module::claude::quota::ccswitch_sqlite {

// 撞上 cc-switch 的写锁时短暂等待，别一上来就报失败。
inline constexpr int kBusyTimeoutMs = 2000;

// sqlite RAII（不抛异常，确定性关闭）。
struct SqliteCloser {
    void operator()(sqlite3* db) const noexcept {
        if (db != nullptr) sqlite3_close(db);
    }
};
using DbHandle = std::unique_ptr<sqlite3, SqliteCloser>;

struct SqliteStmtCloser {
    void operator()(sqlite3_stmt* stmt) const noexcept {
        if (stmt != nullptr) sqlite3_finalize(stmt);
    }
};
using StmtHandle = std::unique_ptr<sqlite3_stmt, SqliteStmtCloser>;

inline std::string columnText(sqlite3_stmt* stmt, int index) {
    const unsigned char* text = sqlite3_column_text(stmt, index);
    if (text == nullptr) return {};
    return std::string(reinterpret_cast<const char*>(text));
}

// providers 表列名清单；表不存在 → 空清单（调用方按 Schema 处理）。先探列再 SELECT，防 schema 演进。
inline std::vector<std::string> providerColumns(sqlite3* db) {
    std::vector<std::string> columns;
    const char* sql = "SELECT name FROM pragma_table_info('providers')";
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &raw, nullptr) != SQLITE_OK) return columns;
    const StmtHandle stmt(raw);
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        columns.push_back(columnText(stmt.get(), 0));
    }
    return columns;
}

inline bool hasColumn(const std::vector<std::string>& columns, const char* name) {
    return std::find(columns.begin(), columns.end(), name) != columns.end();
}

// 缺列用 NULL 占位而非整条 SELECT 失败，认得的字段仍读得回。
inline std::string columnOrNull(const std::vector<std::string>& columns,
                                const char* name) {
    return hasColumn(columns, name) ? std::string(name) : std::string("NULL");
}

inline std::string sqliteError(sqlite3* db, int code) {
    const char* message = db != nullptr ? sqlite3_errmsg(db) : nullptr;
    std::string text = message != nullptr ? message : sqlite3_errstr(code);
    if (text.empty()) text = "sqlite 错误 " + std::to_string(code);
    return text;
}

enum class OpenStatus {
    Ok,
    NotFound,    // 主目录取不到 / 库文件不存在（reason 已填中文）
    Unreadable,  // 存在但打不开：非 SQLite / 权限不足
};

// 打开只读连接；成功时 db 持有效句柄、reason 不动。noun 只进错误文案（"配置库" / "数据库"），
// 两侧口径故意不统一（不同测试各自断言）。
inline OpenStatus openReadOnly(DbHandle& db, std::string* reason,
                               const std::string& path, const char* noun) {
    if (path.empty()) {
        *reason = "找不到用户主目录，无法定位 cc-switch 配置库";
        return OpenStatus::NotFound;
    }
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        *reason = std::string("未找到 cc-switch ") + noun + "（" + path + "）";
        return OpenStatus::NotFound;
    }

    // 不含 CREATE：绝不把库建出来
    sqlite3* raw = nullptr;
    const int openCode =
        sqlite3_open_v2(path.c_str(), &raw, kCcSwitchOpenFlags, nullptr);
    db.reset(raw);
    if (openCode != SQLITE_OK) {
        // 失败时句柄可能是 nullptr，sqliteError 对两者都安全
        *reason = std::string("打不开 cc-switch ") + noun +
                  "（只读）：" + sqliteError(db.get(), openCode);
        return OpenStatus::Unreadable;
    }
    sqlite3_busy_timeout(db.get(), kBusyTimeoutMs);
    return OpenStatus::Ok;
}

// 结果体装配：ListT 只在返回类型里、无法推导，须显式给出。
template <typename ListT, typename StatusT>
inline ListT makeUnavailable(StatusT status, std::string reason) {
    ListT out;
    out.status = status;
    out.unavailableReason = std::move(reason);
    return out;
}

}  // namespace module::claude::quota::ccswitch_sqlite
