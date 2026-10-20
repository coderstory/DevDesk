// 补列迁移：agent 三表（agent_tasks / agent_runs / agent_messages）已有行的列对齐。
// CREATE IF NOT EXISTS 对已存在表跳过 → 老库永远缺 DDL 新增列（2026-10-19 真机
// context_note 报障根因）。本文件在 applySchema 之后逐列 PRAGMA table_info 补缺，
// 幂等——新库全列不炸、老库缺列补齐、旧数据行保留不丢。
#include "schema.h"
#include "sqlite_db.h"

#include <set>
#include <string>
#include <utility>
#include <vector>

namespace core::schema {

namespace {

// 每张 agent 表当前 DDL 定义的完整列名（不含 id —— id 是 INTEGER PRIMARY KEY AUTOINCREMENT，
// 表存在则一定含 id，且 SQLite 不允许 ALTER 改 id 语义）。顺序无关；只用于“是否存在”判据。
// column name → column definition（ALTER TABLE ADD COLUMN 中列名之后的部分）

std::vector<std::pair<std::string, std::string>> agentTasksColumns() {
    return {
        {"name",              "TEXT NOT NULL"},
        {"goal",              "TEXT NOT NULL"},
        {"trigger_type",      "TEXT NOT NULL"},
        {"trigger_config",    "TEXT NOT NULL DEFAULT ''"},
        {"model_slot",        "TEXT NOT NULL DEFAULT 'default'"},
        {"max_iterations",    "INTEGER NOT NULL DEFAULT 10"},
        {"allow_destructive", "INTEGER NOT NULL DEFAULT 0"},
        {"enabled",           "INTEGER NOT NULL DEFAULT 1"},
        {"context_note",      "TEXT NOT NULL DEFAULT ''"},
        {"auto_note",         "INTEGER NOT NULL DEFAULT 1"},
        {"created_at",        "INTEGER NOT NULL"},
        {"updated_at",        "INTEGER NOT NULL"},
    };
}

std::vector<std::pair<std::string, std::string>> agentRunsColumns() {
    return {
        {"task_id",       "INTEGER NOT NULL"},
        {"status",        "TEXT NOT NULL"},
        {"started_at",    "INTEGER NOT NULL"},
        {"ended_at",      "INTEGER"},
        {"iterations",    "INTEGER NOT NULL DEFAULT 0"},
        {"input_tokens",  "INTEGER"},
        {"output_tokens", "INTEGER"},
        {"error",         "TEXT"},
    };
}

std::vector<std::pair<std::string, std::string>> agentMessagesColumns() {
    return {
        {"run_id",        "INTEGER NOT NULL"},
        {"seq",           "INTEGER NOT NULL"},
        {"role",          "TEXT NOT NULL"},
        {"content_json",  "TEXT NOT NULL"},
        {"tool_call_id",  "TEXT"},
        {"created_at",    "INTEGER NOT NULL"},
    };
}

// 检查表是否存在（防 ALTER 空表 PRAGMA 返回空集时误发 ALTER）
core::Result<bool> tableExists(core::data::SqliteDb& db, const std::string& table) {
    auto prep = db.prepare(
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name=?");
    if (prep.isFail()) return core::Result<bool>::fail(prep.errorCode(), prep.errorMessage());
    auto& stmt = prep.value();
    auto b = stmt.bind(1, table);
    if (b.isFail()) return core::Result<bool>::fail(b.errorCode(), b.errorMessage());
    auto s = stmt.step();
    if (s.isFail()) return core::Result<bool>::fail(s.errorCode(), s.errorMessage());
    auto row = stmt.currentRow();
    if (row.isFail()) return core::Result<bool>::fail(row.errorCode(), row.errorMessage());
    auto n = row.value().getInt64("COUNT(*)");
    if (n.isFail()) return core::Result<bool>::fail(n.errorCode(), n.errorMessage());
    return core::Result<bool>::ok(n.value() > 0);
}

// 读某表当前已有列名集合
core::Result<std::set<std::string>> existingColumns(core::data::SqliteDb& db,
                                                      const std::string& table) {
    auto prep = db.prepare("PRAGMA table_info(" + table + ")");
    if (prep.isFail())
        return core::Result<std::set<std::string>>::fail(prep.errorCode(), prep.errorMessage());
    auto rows = prep.value().fetchAll();
    if (rows.isFail())
        return core::Result<std::set<std::string>>::fail(rows.errorCode(), rows.errorMessage());
    std::set<std::string> names;
    for (const auto& row : rows.value()) {
        auto name = row.getString("name");
        if (name.isOk()) names.insert(name.value());
    }
    return core::Result<std::set<std::string>>::ok(std::move(names));
}

// 对齐一张表的列：缺哪个补哪个（幂等——已存在列跳过）
core::Result<void> ensureTableColumns(
    core::data::SqliteDb& db,
    const std::string& table,
    const std::vector<std::pair<std::string, std::string>>& expected) {
    auto exists = tableExists(db, table);
    if (exists.isFail()) return core::Result<void>::fail(exists.errorCode(), exists.errorMessage());
    if (!exists.value()) return core::Result<void>::ok(); // 表不存在，applySchema 会建

    auto existing = existingColumns(db, table);
    if (existing.isFail())
        return core::Result<void>::fail(existing.errorCode(), existing.errorMessage());
    const auto& cols = existing.value();

    for (const auto& [name, def] : expected) {
        if (cols.count(name) > 0) continue; // 已存在，跳过
        std::string sql = "ALTER TABLE " + table + " ADD COLUMN " + name + " " + def;
        auto r = db.exec(sql);
        if (r.isFail()) return r;
    }
    return core::Result<void>::ok();
}

} // namespace

core::Result<void> addMissingAgentColumns(core::data::SqliteDb& db) {
    // 逐个表对齐：缺列补列，不缺列零操作。三张表各自独立——任一张补列失败主调方中止。
    auto r1 = ensureTableColumns(db, "agent_tasks", agentTasksColumns());
    if (r1.isFail()) return r1;
    auto r2 = ensureTableColumns(db, "agent_runs", agentRunsColumns());
    if (r2.isFail()) return r2;
    auto r3 = ensureTableColumns(db, "agent_messages", agentMessagesColumns());
    if (r3.isFail()) return r3;
    return core::Result<void>::ok();
}

core::Result<void> dropRemovedTables(core::data::SqliteDb& db) {
    // 已摘除功能的遗留表（老库启动即清）。必须逐条 DROP 且保持 IF EXISTS：
    // 新库没有这些表，缺 IF EXISTS 会让全新库第一次 migrate 就红。
    // 无外键指向这些表、无任何读方——删除不可逆但无副作用。
    auto r1 = db.exec("DROP TABLE IF EXISTS profile_quota_config");
    if (r1.isFail()) return r1;
    auto r2 = db.exec("DROP TABLE IF EXISTS profile_quota_result");
    if (r2.isFail()) return r2;
    return core::Result<void>::ok();
}

} // namespace core::schema