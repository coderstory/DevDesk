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

const std::vector<std::string>& defaultSkipTables() {
    static const std::vector<std::string> kTables = {
        "claude_call_logs", "claude_run_settings",
        // 同步基线（sha/hash/首次方向/未同步提示）是本机专属状态：随 dump 跨设备
        // 转移会让新设备拿别人写的基线比对 → 误判未变跳过下载 → 进而删远端笔记。
        "github_sync_state"};
    return kTables;
}

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

// 索引 / 触发器共用的三要素：对象名、所属表名、原始 DDL 原文
struct SchemaObject {
    std::string name;
    std::string table;
    std::string ddl;
};

// 列出 sqlite_master 里某类对象的 DDL（type 传 'index' 或 'trigger'）。
// 排除 name LIKE 'sqlite_autoindex_%'：那是 UNIQUE / PRIMARY KEY 约束的**内部**自动索引，
// sql 列为 NULL 不可重建，而重建表本身就会自动恢复它——导出它既没有 DDL 可写，回导也必失败。
// 必须用 tbl_name 回带所属表：索引/触发器的导出范围跟着表的 skip / FTS 影子判据走。
std::vector<SchemaObject> listSchemaObjects(sqlite3* handle, const char* type) {
    std::vector<SchemaObject> objects;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT name, tbl_name, sql FROM sqlite_master "
        "WHERE type=? AND name NOT LIKE 'sqlite_autoindex_%' "
        "ORDER BY name";
    if (sqlite3_prepare_v2(handle, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return objects;
    }
    sqlite3_bind_text(stmt, 1, type, -1, SQLITE_STATIC);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* name = reinterpret_cast<const char*>(
            sqlite3_column_text(stmt, 0));
        const char* table = reinterpret_cast<const char*>(
            sqlite3_column_text(stmt, 1));
        const char* ddl = reinterpret_cast<const char*>(
            sqlite3_column_text(stmt, 2));
        if (name && ddl) objects.push_back({name, table ? table : "", ddl});
    }
    sqlite3_finalize(stmt);
    return objects;
}

// 全部 VIRTUAL TABLE 名（CREATE VIRTUAL TABLE ...）：FTS5 影子表判定的锚点
std::set<std::string> listVirtualTables(sqlite3* handle) {
    std::set<std::string> names;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT name FROM sqlite_master "
        "WHERE type='table' AND sql LIKE 'CREATE VIRTUAL TABLE%'";
    if (sqlite3_prepare_v2(handle, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return names;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* name = reinterpret_cast<const char*>(
            sqlite3_column_text(stmt, 0));
        if (name) names.insert(name);
    }
    sqlite3_finalize(stmt);
    return names;
}

// FTS5 影子表 = 表名恰为某个 VIRTUAL TABLE 名加五个影子后缀之一。
// 不能按「名字含 _config 等」子串判：真实业务表 profile_quota_config 会被误判成
// 影子表，从备份里静默消失。后缀清单与 FTS5 模块落表名一致，别按记忆改写。
bool isFtsShadowName(const std::string& name,
                     const std::set<std::string>& virtualTables) {
    static const char* kSuffixes[] = {
        "_content", "_idx", "_docsize", "_config", "_data"};
    for (const char* suffix : kSuffixes) {
        const size_t n = std::char_traits<char>::length(suffix);
        if (name.size() > n &&
            name.compare(name.size() - n, n, suffix) == 0) {
            if (virtualTables.count(name.substr(0, name.size() - n)) > 0) {
                return true;
            }
        }
    }
    return false;
}

// 剥掉 dump 自带的事务外壳：db.transaction 会再包一层 BEGIN，脚本里的
// BEGIN TRANSACTION / COMMIT 原样执行会报 "cannot start a transaction within a
// transaction"（早到的 COMMIT 还会把外层事务提前提交）。外壳是给 sqlite3 CLI
// 直跑用的；导入侧原子性由外层事务兜（失败整体 ROLLBACK）。
// 只剥首个 BEGIN 与最后一个 COMMIT 两个 token，其余原样保留——真外壳先于全部
// 数据出现、后于全部数据结束，数据字面量里即便有同名文本也轮不到被剥；
// COMMIT 之后的尾巴（外键恢复 PRAGMA 等在事务内是 no-op）**不能丢**：坏 dump
// 尾部多出的语句要原样执行并报错，静默截断 = 悄悄少数据。
// 无 BEGIN 的脚本原样返回。
std::string stripTransactionShell(const std::string& sql) {
    static const std::string kBegin = "BEGIN TRANSACTION;";
    static const std::string kCommit = "COMMIT;";
    const size_t b = sql.find(kBegin);
    if (b == std::string::npos) {
        return sql;
    }
    const size_t e = sql.rfind(kCommit);
    std::string out = sql.substr(0, b);  // 外壳前的头注释保留
    if (e != std::string::npos && e > b) {
        out += sql.substr(b + kBegin.size(), e - b - kBegin.size());
        out += sql.substr(e + kCommit.size());
    } else {
        out += sql.substr(b + kBegin.size());
    }
    return out;
}

} // namespace

core::Result<std::string> exportToString(
    core::data::SqliteDb& db,
    const std::vector<std::string>& skipTables) {

    std::lock_guard<std::recursive_mutex> lock(db.mutex());
    sqlite3* handle = db.handle();

    std::set<std::string> skip(skipTables.begin(), skipTables.end());
    const std::set<std::string> virtuals = listVirtualTables(handle);
    const auto isFtsShadow = [&virtuals](const std::string& name) {
        return isFtsShadowName(name, virtuals);
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
    // foreign_keys 开关与事务外壳配套：CLI 直跑时 FK 默认关，但开着 FK 的会话
    // （如 sqlite3 -cmd "PRAGMA foreign_keys=ON"）按表名字典序导数，子表行可能
    // 先于父表行落库被当场拒收。PRAGMA 在事务内是 no-op，必须落在 BEGIN 之外。
    out << "\n\nPRAGMA foreign_keys=OFF;\n\nBEGIN TRANSACTION;\n\n";

    // 导出顺序四段：① 建表 DDL ② 索引 DDL ③ INSERT 数据 ④ 触发器 DDL。
    // 索引必须在 INSERT 前，触发器必须在 INSERT 后（各自段注释写明反了会怎样）；
    // 两段 DDL 自带 DROP IF EXISTS，之间无先后依赖。
    std::vector<std::string> exported;
    for (const auto& table : listUserTables(handle)) {
        if (skip.count(table) || isFtsShadow(table)) continue;
        std::string ddl = tableDdl(handle, table);
        if (ddl.empty()) continue;
        exported.push_back(table);
        out << "-- Table: " << table << "\n";
        out << "DROP TABLE IF EXISTS \"" << table << "\";\n";
        out << ddl << ";\n\n";
    }

    // 索引写成「DROP ... IF EXISTS + 原 DDL」，重复导入同一份 dump 才幂等。
    // 不采用改写 DDL 成 CREATE ... IF NOT EXISTS 的做法：sqlite_master 里的原文形如
    // CREATE [UNIQUE] INDEX，前缀改写要分辨 UNIQUE、大小写与空白，而 DROP IF EXISTS
    // 是固定前缀、与原文无关。DROP TABLE 已连带删掉所属表的索引，这里的 DROP 兜的
    // 是「同名对象在别的表上」以及「表未被重建」的情形。
    for (const auto& obj : listSchemaObjects(handle, "index")) {
        if (skip.count(obj.table) || isFtsShadow(obj.table)) continue;
        out << "-- Index: " << obj.name << "\n";
        out << "DROP INDEX IF EXISTS \"" << obj.name << "\";\n";
        out << obj.ddl << ";\n\n";
    }

    for (const auto& table : exported) {
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

    // 触发器必须在 INSERT **之后**建（区别于索引）：先建的话，回导 INSERT 会把
    // 触发器本身点着（AFTER INSERT 落审计行一类），触发器写到的表在 dump 里还有
    // 自己的原始 INSERT，重建库的数据 = 原始 + 点火新增，还原即悄悄多数据。
    for (const auto& obj : listSchemaObjects(handle, "trigger")) {
        if (skip.count(obj.table) || isFtsShadow(obj.table)) continue;
        out << "-- Trigger: " << obj.name << "\n";
        out << "DROP TRIGGER IF EXISTS \"" << obj.name << "\";\n";
        out << obj.ddl << ";\n\n";
    }

    out << "COMMIT;\nPRAGMA foreign_keys=ON;\n";

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

    // 导入期间临时关外键：dump 的 DDL/INSERT 按表名字典序，子表行（agent_runs 对
    // agent_tasks 一类）可能先于父表行落库，open() 恒置 foreign_keys=ON 会当场拒收；
    // 结束后恢复原值。PRAGMA 在事务内是 no-op，必须在 BEGIN 之外设 → 不进 transaction。
    int fkWas = 1;  // 读不到按 ON 恢复（open() 恒置 ON）
    if (auto pr = db.prepare("PRAGMA foreign_keys"); pr.isOk()) {
        auto& stmt = pr.value();
        auto stepped = stmt.step();
        if (stepped.isOk() && stepped.value()) {
            auto row = stmt.currentRow();
            if (row.isOk() && !row.value().values.empty()) {
                if (auto v = std::get_if<int64_t>(&row.value().values[0])) {
                    fkWas = *v != 0;
                }
            }
        }
    }
    db.exec("PRAGMA foreign_keys = OFF;");

    auto result = db.transaction([&]() -> core::Result<void> {
        return db.exec(stripTransactionShell(sql));
    });

    db.exec(std::string("PRAGMA foreign_keys = ") + (fkWas ? "ON;" : "OFF;"));
    return result;
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