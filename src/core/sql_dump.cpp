// src/core/sql_dump.cpp
// SQLite 导出/导入实现。
// 导出：遍历 sqlite_master 拿表名 + DDL，每表 SELECT * 生成 INSERT。
// 导入：逐 SQL 语句 exec，一个事务。
#include "sql_dump.h"

#include <algorithm>
#include <ctime>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "atomic_write.h"
#include "fs_text.h"
#include "logging.h"
#include "result.h"

#include <sqlite3.h>

namespace core::sql_dump {

namespace {

// 引号转义：SQL 字符串字面量里的单引号 → ''（SQL 标准转义）
std::string escapeSql(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        if (c == '\'') out += "''";
        else out += c;
    }
    return out;
}

// SQL 值字面量：NULL / 整数 / 实数 / 字符串 / BLOB（hex）
std::string sqlValueLiteral(sqlite3_stmt* stmt, int col) {
    const int type = sqlite3_column_type(stmt, col);
    switch (type) {
        case SQLITE_NULL:
            return "NULL";
        case SQLITE_INTEGER:
            return std::to_string(sqlite3_column_int64(stmt, col));
        case SQLITE_FLOAT:
            return std::to_string(sqlite3_column_double(stmt, col));
        case SQLITE_BLOB: {
            const int n = sqlite3_column_bytes(stmt, col);
            const unsigned char* p = static_cast<const unsigned char*>(
                sqlite3_column_blob(stmt, col));
            std::string hex = "X'";
            hex.reserve(3 + n * 2);
            static const char kHex[] = "0123456789ABCDEF";
            for (int i = 0; i < n; ++i) {
                hex += kHex[p[i] >> 4];
                hex += kHex[p[i] & 0x0F];
            }
            hex += '\'';
            return hex;
        }
        case SQLITE_TEXT:
        default:
            return "'" + escapeSql(
                std::string(reinterpret_cast<const char*>(
                    sqlite3_column_text(stmt, col)),
                    sqlite3_column_bytes(stmt, col))) + "'";
    }
}

// 列出所有用户表（跳过 sqlite_ 内部表 + FTS 影子表）
std::vector<std::string> listUserTables(sqlite3* handle) {
    std::vector<std::string> tables;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT name FROM sqlite_master "
        "WHERE type='table' AND name NOT LIKE 'sqlite_%' "
        "ORDER BY name";
    if (sqlite3_prepare_v2(handle, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return tables;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* name = reinterpret_cast<const char*>(
            sqlite3_column_text(stmt, 0));
        if (name) tables.push_back(name);
    }
    sqlite3_finalize(stmt);
    return tables;
}

// 获取表的 CREATE TABLE DDL
std::string tableDdl(sqlite3* handle, const std::string& table) {
    std::string ddl;
    sqlite3_stmt* stmt = nullptr;
    std::string sql = "SELECT sql FROM sqlite_master "
                      "WHERE type='table' AND name=?";
    if (sqlite3_prepare_v2(handle, sql.c_str(), -1, &stmt, nullptr)
        != SQLITE_OK) {
        return ddl;
    }
    sqlite3_bind_text(stmt, 1, table.c_str(), -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* text = reinterpret_cast<const char*>(
            sqlite3_column_text(stmt, 0));
        if (text) ddl = text;
    }
    sqlite3_finalize(stmt);
    return ddl;
}

} // namespace

core::Result<std::string> exportToString(
    core::data::SqliteDb& db,
    const std::vector<std::string>& skipTables) {

    std::lock_guard<std::recursive_mutex> lock(db.mutex());
    sqlite3* handle = db.handle();

    std::set<std::string> skip(skipTables.begin(), skipTables.end());

    // FTS 影子表前缀：这些表有对应的 VIRTUAL TABLE，DDL 里已有，影子表不单独导
    // 但 sqlite_master 里影子表也列出 type='table'（而非 virtual），需跳过
    const auto isFtsShadow = [](const std::string& name) -> bool {
        return name.find("_content") != std::string::npos ||
               name.find("_idx") != std::string::npos ||
               name.find("_docsize") != std::string::npos ||
               name.find("_config") != std::string::npos ||
               name.find("_data") != std::string::npos;
    };

    std::ostringstream out;
    out << "-- DevDesk SQLite dump (auto-generated)\n";
    out << "-- Generated at: " << std::time(nullptr) << " (unix epoch)\n";
    out << "-- Tables: ";
    bool first = true;
    for (const auto& t : listUserTables(handle)) {
        if (skip.count(t) || isFtsShadow(t)) continue;
        if (!first) out << ", ";
        out << t;
        first = false;
    }
    out << "\n\nBEGIN TRANSACTION;\n\n";

    for (const auto& table : listUserTables(handle)) {
        if (skip.count(table) || isFtsShadow(table)) continue;

        // DDL
        std::string ddl = tableDdl(handle, table);
        if (ddl.empty()) continue;
        out << "-- Table: " << table << "\n";
        out << "DROP TABLE IF EXISTS \"" << table << "\";\n";
        out << ddl << ";\n\n";

        // INSERT
        std::string selectSql = "SELECT * FROM \"" + table + "\"";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(handle, selectSql.c_str(), -1, &stmt, nullptr)
            != SQLITE_OK) {
            continue;  // 空表 / 无法查询 → 跳过
        }

        int rowCount = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const int colCount = sqlite3_column_count(stmt);
            out << "INSERT INTO \"" << table << "\" VALUES(";
            for (int i = 0; i < colCount; ++i) {
                if (i > 0) out << ", ";
                out << sqlValueLiteral(stmt, i);
            }
            out << ");\n";
            ++rowCount;
        }
        sqlite3_finalize(stmt);
        if (rowCount > 0) out << "\n";
    }

    out << "COMMIT;\n";

    std::string result = out.str();
    LOG_INFO("sql_dump: 导出完成，大小=" + std::to_string(result.size()) + " 字节");
    return core::Result<std::string>::ok(std::move(result));
}

core::Result<void> exportToFile(
    core::data::SqliteDb& db,
    const std::filesystem::path& file,
    const std::vector<std::string>& skipTables) {

    auto sql = exportToString(db, skipTables);
    if (sql.isFail()) {
        return core::Result<void>::fail(sql.errorCode(), sql.errorMessage());
    }
    return core::atomicWriteFile(file, sql.value(), "SQL dump");
}

core::Result<void> importFromString(
    core::data::SqliteDb& db,
    const std::string& sql) {

    return db.transaction([&]() -> core::Result<void> {
        return db.exec(sql);
    });
}

core::Result<void> importFromFile(
    core::data::SqliteDb& db,
    const std::filesystem::path& file) {

    bool readFailed = false;
    std::string sql = core::readTextFile(file, &readFailed);
    if (readFailed) {
        return core::Result<void>::fail(
            core::ErrorCode::FileReadFailed,
            "无法读取 SQL dump 文件: " + file.string());
    }
    if (sql.empty()) {
        return core::Result<void>::fail(
            core::ErrorCode::FileReadFailed,
            "SQL dump 文件为空: " + file.string());
    }
    return importFromString(db, sql);
}

} // namespace core::sql_dump