// 补列/修列迁移（Database::migrate 的启动路径）：
//   1) agent 三表（agent_tasks / agent_runs / agent_messages）缺列补齐——
//      CREATE IF NOT EXISTS 对已存在表跳过 → 老库永远缺 DDL 新增列（2026-10-19 真机
//      context_note 报障根因）；
//   2) Todo 固定四列迁移（UAT6.2 H20）——与 Todo 模块开关无关，启动时跑一次。
// 均幂等：新库不炸、老库补齐、旧数据行保留不丢。
// 票 11：补列迁移的表/列标识符一律 enum → schema.h constexpr 白名单查表后拼接，
// 非法值在查表处（加载期）显式报错——不再有裸字符串直接进 ALTER 语句。
#include "schema.h"

#include "db_backup.h"
#include "logging.h"
#include "sqlite_db.h"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <sqlite3.h>

namespace core::schema {

namespace {

// 每张 agent 表当前 DDL 定义的完整列（不含 id —— id 是 INTEGER PRIMARY KEY AUTOINCREMENT，
// 表存在则一定含 id，且 SQLite 不允许 ALTER 改 id 语义）。顺序无关；只用于“是否存在”判据。
// 票 11：列不再写字符串对（name/def 直接进拼接），一律枚举值——列名与列定义由
// schema.h 的 constexpr 白名单按枚举查表给出（见 ensureTableColumns），
// 非法枚举值在查表处显式报错，绝不拼进 SQL。
std::vector<MigrationColumn> agentTasksColumns() {
    return {
        MigrationColumn::TasksName,
        MigrationColumn::TasksGoal,
        MigrationColumn::TasksTriggerType,
        MigrationColumn::TasksTriggerConfig,
        MigrationColumn::TasksModelSlot,
        MigrationColumn::TasksMaxIterations,
        MigrationColumn::TasksAllowDestructive,
        MigrationColumn::TasksEnabled,
        MigrationColumn::TasksContextNote,
        MigrationColumn::TasksAutoNote,
        MigrationColumn::TasksSystemPrompt,
        MigrationColumn::TasksCreatedAt,
        MigrationColumn::TasksUpdatedAt,
    };
}

std::vector<MigrationColumn> agentRunsColumns() {
    return {
        MigrationColumn::RunsTaskId,
        MigrationColumn::RunsStatus,
        MigrationColumn::RunsStartedAt,
        MigrationColumn::RunsEndedAt,
        MigrationColumn::RunsIterations,
        MigrationColumn::RunsInputTokens,
        MigrationColumn::RunsOutputTokens,
        MigrationColumn::RunsError,
    };
}

std::vector<MigrationColumn> agentMessagesColumns() {
    return {
        MigrationColumn::MessagesRunId,
        MigrationColumn::MessagesSeq,
        MigrationColumn::MessagesRole,
        MigrationColumn::MessagesContentJson,
        MigrationColumn::MessagesToolCallId,
        MigrationColumn::MessagesCreatedAt,
    };
}

// 白名单查询实现见匿名命名空间之后的 migrationTableName / migrationColumnName /
// migrationColumnDdl（公开 API，测试直接调用做「非法迁移名被拒」验证）。

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

// 对齐一张表的列：缺哪个补哪个（幂等——已存在列跳过）。
// 票 11 口径：表名与全部列先过白名单（任一非法值在拼接前显式报错并整体放弃，
// 一个字都不会进 SQLite），通过后拼接只接触白名单返回的静态标识符。
core::Result<void> ensureTableColumns(
    core::data::SqliteDb& db,
    const MigrationTable table,
    const std::vector<MigrationColumn>& expected) {
    // 1) 表名白名单校验
    auto tableName = migrationTableName(table);
    if (tableName.isFail()) {
        return core::Result<void>::fail(tableName.errorCode(), tableName.errorMessage());
    }

    // 2) 全部列的列名/列定义白名单校验——先整体解析，后统一执行（all-or-nothing）
    struct ResolvedColumn {
        const char* name;
        const char* ddl;
    };
    std::vector<ResolvedColumn> resolved;
    resolved.reserve(expected.size());
    for (const auto column : expected) {
        auto name = migrationColumnName(column);
        if (name.isFail()) {
            return core::Result<void>::fail(name.errorCode(), name.errorMessage());
        }
        auto ddl = migrationColumnDdl(column);
        if (ddl.isFail()) {
            return core::Result<void>::fail(ddl.errorCode(), ddl.errorMessage());
        }
        resolved.push_back({name.value(), ddl.value()});
    }

    auto exists = tableExists(db, tableName.value());
    if (exists.isFail()) return core::Result<void>::fail(exists.errorCode(), exists.errorMessage());
    if (!exists.value()) return core::Result<void>::ok(); // 表不存在，applySchema 会建

    auto existing = existingColumns(db, tableName.value());
    if (existing.isFail())
        return core::Result<void>::fail(existing.errorCode(), existing.errorMessage());
    const auto& cols = existing.value();

    for (const auto& [name, def] : resolved) {
        if (cols.count(name) > 0) continue; // 已存在，跳过
        std::string sql = std::string("ALTER TABLE ") + tableName.value() +
                          " ADD COLUMN " + name + " " + def;
        auto r = db.exec(sql);
        if (r.isFail()) return r;
    }
    return core::Result<void>::ok();
}

// ---- Todo 固定四列迁移（UAT6.2 H20）用的小工具 ----
// 顺序即最终 position 0..3；缺失列按固定标题/色点重建（色点 token 与
// module_todo/todo_ui_logic.h 的 columnDotColor 映射一致）。
struct FixedColumnSeed {
    const char* kind;
    const char* title;
    const char* color;
};
constexpr FixedColumnSeed kFixedColumnSeeds[] = {
    {"todo", "待办", "claude-clay"},
    {"doing", "进行中", "warm-amber"},
    {"done", "已完成", "muted-sage"},
    {"archive", "已归档", "warm-gray"},
};
constexpr size_t kFixedColumnCount =
    sizeof(kFixedColumnSeeds) / sizeof(kFixedColumnSeeds[0]);

// 标量查询（SQL 必须把唯一结果列别名成 v）；无行按 0。
core::Result<int64_t> queryScalarInt(core::data::SqliteDb& db, const std::string& sql) {
    auto prep = db.prepare(sql);
    if (prep.isFail()) return core::Result<int64_t>::fail(prep.errorCode(), prep.errorMessage());
    auto rows = prep.value().fetchAll();
    if (rows.isFail()) return core::Result<int64_t>::fail(rows.errorCode(), rows.errorMessage());
    if (rows.value().empty()) return core::Result<int64_t>::ok(0);
    auto value = rows.value()[0].getInt64("v");
    if (value.isFail()) return core::Result<int64_t>::fail(value.errorCode(), value.errorMessage());
    return core::Result<int64_t>::ok(value.value());
}

// 带一个 int64 绑定的标量查询（列别名 v）
core::Result<int64_t> queryScalarInt1(core::data::SqliteDb& db, const std::string& sql,
                                      int64_t bindValue) {
    auto prep = db.prepare(sql);
    if (prep.isFail()) return core::Result<int64_t>::fail(prep.errorCode(), prep.errorMessage());
    auto& stmt = prep.value();
    if (auto r = stmt.bind(1, bindValue); r.isFail()) {
        return core::Result<int64_t>::fail(r.errorCode(), r.errorMessage());
    }
    auto rows = stmt.fetchAll();
    if (rows.isFail()) return core::Result<int64_t>::fail(rows.errorCode(), rows.errorMessage());
    if (rows.value().empty()) return core::Result<int64_t>::ok(0);
    auto value = rows.value()[0].getInt64("v");
    if (value.isFail()) return core::Result<int64_t>::fail(value.errorCode(), value.errorMessage());
    return core::Result<int64_t>::ok(value.value());
}

struct KindColumnHit {
    int64_t id = 0;        // 0 = 没有该 kind 的列
    int64_t position = 0;
};

// 按 kind 找列；同 kind 多列时 position 最小（再按 id）者为「正主」，其余在迁移里并进来。
core::Result<KindColumnHit> findKindColumn(core::data::SqliteDb& db, const std::string& kind) {
    auto prep = db.prepare(
        "SELECT id, position FROM todo_columns WHERE kind = ? "
        "ORDER BY position ASC, id ASC LIMIT 1");
    if (prep.isFail())
        return core::Result<KindColumnHit>::fail(prep.errorCode(), prep.errorMessage());
    auto& stmt = prep.value();
    if (auto r = stmt.bind(1, kind); r.isFail()) {
        return core::Result<KindColumnHit>::fail(r.errorCode(), r.errorMessage());
    }
    auto rows = stmt.fetchAll();
    if (rows.isFail())
        return core::Result<KindColumnHit>::fail(rows.errorCode(), rows.errorMessage());
    KindColumnHit hit;
    if (rows.value().empty()) return core::Result<KindColumnHit>::ok(hit);
    auto id = rows.value()[0].getInt64("id");
    if (id.isFail()) return core::Result<KindColumnHit>::fail(id.errorCode(), id.errorMessage());
    auto pos = rows.value()[0].getInt64("position");
    if (pos.isFail()) return core::Result<KindColumnHit>::fail(pos.errorCode(), pos.errorMessage());
    hit.id = id.value();
    hit.position = pos.value();
    return core::Result<KindColumnHit>::ok(hit);
}

// 迁移预检（只读）：true = 需要改写，用于决定迁移前是否先备份库文件。
// 判据与 migrateTodoFixedColumns 的步骤一一对应（未知/NULL kind 列 + 同 kind 重复列 +
// 四列缺任一 + position 偏差），宁可多报不可漏报。
core::Result<bool> fixedColumnsNeedMigration(core::data::SqliteDb& db) {
    bool hasKind = false;
    {
        auto prep = db.prepare("PRAGMA table_info(todo_columns)");
        if (prep.isFail())
            return core::Result<bool>::fail(prep.errorCode(), prep.errorMessage());
        auto cols = prep.value().fetchAll();
        if (cols.isFail())
            return core::Result<bool>::fail(cols.errorCode(), cols.errorMessage());
        for (const auto& col : cols.value()) {
            auto name = col.getString("name");
            if (name.isOk() && name.value() == "kind") {
                hasKind = true;
                break;
            }
        }
    }
    if (!hasKind) return core::Result<bool>::ok(true);  // 连 kind 都没有，必然要改

    static constexpr const char* kProbeSql =
        "SELECT "
        "(SELECT COUNT(*) FROM todo_columns "
        " WHERE kind IS NULL OR kind NOT IN ('todo','doing','done','archive')) + "
        "(SELECT COUNT(*) FROM todo_columns c "
        " WHERE c.kind IN ('todo','doing','done','archive') AND c.id <> "
        "  (SELECT c2.id FROM todo_columns c2 WHERE c2.kind = c.kind "
        "   ORDER BY c2.position ASC, c2.id ASC LIMIT 1)) + "
        "(SELECT COUNT(*) FROM todo_columns "
        " WHERE (kind='todo' AND position<>0) OR (kind='doing' AND position<>1) OR "
        "       (kind='done' AND position<>2) OR (kind='archive' AND position<>3)) + "
        "(SELECT COUNT(*) FROM "
        "  (SELECT 'todo' AS k UNION ALL SELECT 'doing' UNION ALL "
        "   SELECT 'done' UNION ALL SELECT 'archive') k "
        " WHERE NOT EXISTS (SELECT 1 FROM todo_columns WHERE kind = k.k)) "
        "AS v";
    auto problems = queryScalarInt(db, kProbeSql);
    if (problems.isFail())
        return core::Result<bool>::fail(problems.errorCode(), problems.errorMessage());
    return core::Result<bool>::ok(problems.value() > 0);
}

// 迁移前备份（复用 core::dbmaint 的在线备份）。路径取自 SQLite 连接本身
// （sqlite3_db_filename，UTF-8）——SqliteDb 不暴露路径、测试里也是临时库，不能写死
// paths::sqliteDbFile()。备份失败/路径不可转换只 WARN，不阻断迁移：正确性由事务兜底。
void backupBeforeFixedColumnMigration(core::data::SqliteDb& db) {
    std::string dbFile;
    {
        // 裸句柄调用须自持锁（见 sqlite_db.h 并发纪律），锁只覆盖取路径这一步
        std::lock_guard<std::recursive_mutex> lock(db.mutex());
        const char* fileName = sqlite3_db_filename(db.handle(), "main");
        if (fileName != nullptr) dbFile = fileName;
    }
    if (dbFile.empty()) {
        LOG_WARN("Todo 固定四列迁移: 无库文件路径（内存库），跳过迁移前备份");
        return;
    }
    // SQLite 给的是 UTF-8；Windows 下 std::filesystem::path(char*) 走窄字符 locale 转换，
    // 本 target -fno-exceptions（转换失败会 abort）——非 ASCII 路径直接跳过备份，不冒险。
    const bool asciiOnly = std::all_of(dbFile.begin(), dbFile.end(),
                                       [](unsigned char c) { return c < 0x80; });
    if (!asciiOnly) {
        LOG_WARN("Todo 固定四列迁移: 库路径含非 ASCII 字符，跳过迁移前备份（事务兜底）: " +
                 dbFile);
        return;
    }
    auto backup = core::dbmaint::createBackup(std::filesystem::path(dbFile), 5);
    if (backup.isOk()) {
        LOG_INFO("Todo 固定四列迁移前备份: " + backup.value().string());
    } else {
        LOG_WARN("Todo 固定四列迁移前备份失败（不阻断迁移，事务兜底）: " +
                 backup.errorMessage());
    }
}

// 重建缺失的固定列（position 先按固定序号；末尾统一归位）。
core::Result<void> insertFixedColumn(core::data::SqliteDb& db, const FixedColumnSeed& seed,
                                     int position) {
    auto prep = db.prepare(
        "INSERT INTO todo_columns (title, position, color, created_at, kind) "
        "VALUES (?, ?, ?, ?, ?)");
    if (prep.isFail()) return core::Result<void>::fail(prep.errorCode(), prep.errorMessage());
    auto& stmt = prep.value();
    if (auto r = stmt.bind(1, seed.title); r.isFail()) return r;
    if (auto r = stmt.bind(2, static_cast<int64_t>(position)); r.isFail()) return r;
    if (auto r = stmt.bind(3, seed.color); r.isFail()) return r;
    if (auto r = stmt.bind(4, static_cast<int64_t>(std::time(nullptr))); r.isFail()) return r;
    if (auto r = stmt.bind(5, seed.kind); r.isFail()) return r;
    auto step = stmt.step();
    if (step.isFail()) return core::Result<void>::fail(step.errorCode(), step.errorMessage());
    return core::Result<void>::ok();
}

} // namespace

// ---- 迁移白名单查询（公开 API，schema.h 声明）----
// 枚举值先过范围校验，再按下标直取（下标直取的合法前提是
// migrationWhitelistsAligned() 的 static_assert——数组与枚举一一对应且同序）。
// 范围外的值（越界强转）在此显式报错：非法值到不了 SQL 拼接那一行。
core::Result<const char*> migrationTableName(const MigrationTable table) {
    const auto index = static_cast<size_t>(table);
    if (index >= sizeof(kMigrationTableWhitelist) / sizeof(kMigrationTableWhitelist[0])) {
        return core::Result<const char*>::fail(
            core::ErrorCode::DatabaseMigrationFailed,
            "迁移白名单拒绝非法表枚举值: " + std::to_string(static_cast<int>(table)));
    }
    return core::Result<const char*>::ok(kMigrationTableWhitelist[index].name);
}

core::Result<const char*> migrationColumnName(const MigrationColumn column) {
    const auto index = static_cast<size_t>(column);
    if (index >= sizeof(kMigrationColumnWhitelist) / sizeof(kMigrationColumnWhitelist[0])) {
        return core::Result<const char*>::fail(
            core::ErrorCode::DatabaseMigrationFailed,
            "迁移白名单拒绝非法列枚举值: " + std::to_string(static_cast<int>(column)));
    }
    return core::Result<const char*>::ok(kMigrationColumnWhitelist[index].name);
}

core::Result<const char*> migrationColumnDdl(const MigrationColumn column) {
    const auto index = static_cast<size_t>(column);
    if (index >= sizeof(kMigrationColumnWhitelist) / sizeof(kMigrationColumnWhitelist[0])) {
        return core::Result<const char*>::fail(
            core::ErrorCode::DatabaseMigrationFailed,
            "迁移白名单拒绝非法列定义枚举值: " + std::to_string(static_cast<int>(column)));
    }
    return core::Result<const char*>::ok(kMigrationColumnWhitelist[index].ddl);
}

core::Result<void> migrateTodoFixedColumns(core::data::SqliteDb& db) {
    // 迁移是破坏性操作（会删多余列）。迁移前备份由本函数按预检触发（复用 core::dbmaint；
    // 路径取自 sqlite3_db_filename——main 的启动备份排在模块 onStart 之后，不能当迁移前快照）。
    // 整段单事务：任一步失败整体 ROLLBACK，绝不留下「任务搬了一半 / 列删了一半」的中间态。
    auto needsMigration = fixedColumnsNeedMigration(db);
    if (needsMigration.isFail()) {
        LOG_WARN("Todo 固定四列迁移预检失败（按需要迁移处理，先备份）: " +
                 needsMigration.errorMessage());
        backupBeforeFixedColumnMigration(db);
    } else if (needsMigration.value()) {
        backupBeforeFixedColumnMigration(db);
    }

    return db.transaction([&db]() -> core::Result<void> {
        // 0) 极老库可能连 kind 列都没有（单份 schema 不升级旧库）：先补列（幂等）。
        bool hasKind = false;
        {
            auto prep = db.prepare("PRAGMA table_info(todo_columns)");
            if (prep.isFail())
                return core::Result<void>::fail(prep.errorCode(), prep.errorMessage());
            auto cols = prep.value().fetchAll();
            if (cols.isFail())
                return core::Result<void>::fail(cols.errorCode(), cols.errorMessage());
            for (const auto& col : cols.value()) {
                auto name = col.getString("name");
                if (name.isOk() && name.value() == "kind") {
                    hasKind = true;
                    break;
                }
            }
        }
        if (!hasKind) {
            auto r = db.exec("ALTER TABLE todo_columns ADD COLUMN kind TEXT");
            if (r.isFail()) return r;
        }

        // 1) 迁移前快照：列数 / 任务数（日志与数据守恒校验用）
        auto beforeCols = queryScalarInt(db, "SELECT COUNT(*) AS v FROM todo_columns");
        if (beforeCols.isFail())
            return core::Result<void>::fail(beforeCols.errorCode(), beforeCols.errorMessage());
        auto beforeTasks = queryScalarInt(db, "SELECT COUNT(*) AS v FROM todo_tasks");
        if (beforeTasks.isFail())
            return core::Result<void>::fail(beforeTasks.errorCode(), beforeTasks.errorMessage());

        bool changed = false;

        // 2) 历史「进行中」识别：kind 为 NULL 且标题仍是「进行中」→ kind='doing'。
        //    必须先补值再做「缺失列重建」：补过之后它才作为 doing 正主被找到，
        //    否则会被当成无 kind 的自定义列并进待办列。改过名的 NULL 列不在此列，
        //    按自定义列规则并入目标默认列（见第 4 步）。
        {
            auto prep = db.prepare(
                "UPDATE todo_columns SET kind = 'doing' "
                "WHERE kind IS NULL AND title = '进行中'");
            if (prep.isFail())
                return core::Result<void>::fail(prep.errorCode(), prep.errorMessage());
            auto step = prep.value().step();
            if (step.isFail())
                return core::Result<void>::fail(step.errorCode(), step.errorMessage());
            if (sqlite3_changes(db.handle()) > 0) changed = true;
        }

        // 3) 确保四列存在：按 kind 找正主（同 kind 多列取 position 最小者）；
        //    缺失的按固定标题/色点/序号重建。
        int64_t fixedIds[kFixedColumnCount] = {};
        int createdColumns = 0;
        for (size_t i = 0; i < kFixedColumnCount; ++i) {
            auto hit = findKindColumn(db, kFixedColumnSeeds[i].kind);
            if (hit.isFail())
                return core::Result<void>::fail(hit.errorCode(), hit.errorMessage());
            if (hit.value().id == 0) {
                auto inserted = insertFixedColumn(db, kFixedColumnSeeds[i],
                                                  static_cast<int>(i));
                if (inserted.isFail()) return inserted;
                hit = findKindColumn(db, kFixedColumnSeeds[i].kind);
                if (hit.isFail())
                    return core::Result<void>::fail(hit.errorCode(), hit.errorMessage());
                ++createdColumns;
            }
            fixedIds[i] = hit.value().id;
        }

        // 4) 多余自定义列归并：任务按 position 顺序追加到目标默认列——
        //    列自身 kind 对应某个固定 kind → 并入该 kind 列；NULL/未知 kind → 并入待办列。
        //    同 kind 的重复列（非正主）也走这里并入正主。
        //    ⚠ 顺序硬约束：先 UPDATE todo_tasks.column_id，再 DELETE todo_columns。
        //      column_id 外键是 ON DELETE CASCADE，先删列会把该列下所有任务连带删光。
        struct ExtraColumn {
            int64_t id;
            std::string kind;
        };
        std::vector<ExtraColumn> extras;
        {
            auto prep = db.prepare(
                "SELECT id, kind FROM todo_columns ORDER BY position ASC, id ASC");
            if (prep.isFail())
                return core::Result<void>::fail(prep.errorCode(), prep.errorMessage());
            auto cols = prep.value().fetchAll();
            if (cols.isFail())
                return core::Result<void>::fail(cols.errorCode(), cols.errorMessage());
            for (const auto& col : cols.value()) {
                auto idResult = col.getInt64("id");
                if (idResult.isFail())
                    return core::Result<void>::fail(idResult.errorCode(), idResult.errorMessage());
                const int64_t id = idResult.value();
                bool isFixed = false;
                for (size_t i = 0; i < kFixedColumnCount; ++i) {
                    if (fixedIds[i] == id) {
                        isFixed = true;
                        break;
                    }
                }
                if (isFixed) continue;
                ExtraColumn extra{id, ""};
                if (!col.isNull("kind")) {
                    auto kindResult = col.getString("kind");
                    if (kindResult.isOk()) extra.kind = kindResult.value();
                }
                extras.push_back(std::move(extra));
            }
        }

        int mergedColumns = 0;
        for (const auto& extra : extras) {
            size_t targetIdx = 0;  // 无对应 kind → 待办列
            for (size_t i = 0; i < kFixedColumnCount; ++i) {
                if (extra.kind == kFixedColumnSeeds[i].kind) {
                    targetIdx = i;
                    break;
                }
            }
            const int64_t targetId = fixedIds[targetIdx];

            // 目标列当前末尾位置；offset 让源列任务整体落到目标列尾部，
            // 且源列内相对顺序不变（全体 position 加同一个 offset）。
            auto maxPos = queryScalarInt1(
                db,
                "SELECT COALESCE(MAX(position), -1) AS v FROM todo_tasks WHERE column_id = ?",
                targetId);
            if (maxPos.isFail())
                return core::Result<void>::fail(maxPos.errorCode(), maxPos.errorMessage());
            {
                auto prep = db.prepare(
                    "UPDATE todo_tasks SET column_id = ?, position = position + ? "
                    "WHERE column_id = ?");
                if (prep.isFail())
                    return core::Result<void>::fail(prep.errorCode(), prep.errorMessage());
                auto& stmt = prep.value();
                if (auto r = stmt.bind(1, targetId); r.isFail()) return r;
                if (auto r = stmt.bind(2, maxPos.value() + 1); r.isFail()) return r;
                if (auto r = stmt.bind(3, extra.id); r.isFail()) return r;
                auto step = stmt.step();
                if (step.isFail())
                    return core::Result<void>::fail(step.errorCode(), step.errorMessage());
            }
            {
                // 该列任务已全部搬到目标列 → 删列不再触发任何任务级联删除
                auto prep = db.prepare("DELETE FROM todo_columns WHERE id = ?");
                if (prep.isFail())
                    return core::Result<void>::fail(prep.errorCode(), prep.errorMessage());
                auto& stmt = prep.value();
                if (auto r = stmt.bind(1, extra.id); r.isFail()) return r;
                auto step = stmt.step();
                if (step.isFail())
                    return core::Result<void>::fail(step.errorCode(), step.errorMessage());
            }
            ++mergedColumns;
        }
        if (mergedColumns > 0) changed = true;

        // 5) 四列 position 归位 0..3（只有偏差时才写，幂等重跑零改动）
        bool positionsDirty = false;
        for (size_t i = 0; i < kFixedColumnCount; ++i) {
            auto pos = queryScalarInt1(
                db, "SELECT position AS v FROM todo_columns WHERE id = ?", fixedIds[i]);
            if (pos.isFail())
                return core::Result<void>::fail(pos.errorCode(), pos.errorMessage());
            if (pos.value() != static_cast<int64_t>(i)) positionsDirty = true;
        }
        if (positionsDirty) {
            for (size_t i = 0; i < kFixedColumnCount; ++i) {
                auto prep = db.prepare("UPDATE todo_columns SET position = ? WHERE id = ?");
                if (prep.isFail())
                    return core::Result<void>::fail(prep.errorCode(), prep.errorMessage());
                auto& stmt = prep.value();
                if (auto r = stmt.bind(1, static_cast<int64_t>(i)); r.isFail()) return r;
                if (auto r = stmt.bind(2, fixedIds[i]); r.isFail()) return r;
                auto step = stmt.step();
                if (step.isFail())
                    return core::Result<void>::fail(step.errorCode(), step.errorMessage());
            }
            changed = true;
        }

        // 6) 迁移后快照 + 数据守恒校验：迁移只搬 column_id，任务行数必须不变。
        //    不等 = 有任务被 CASCADE 连带删除；返回失败让整段 ROLLBACK，绝不带数据损失提交。
        auto afterCols = queryScalarInt(db, "SELECT COUNT(*) AS v FROM todo_columns");
        if (afterCols.isFail())
            return core::Result<void>::fail(afterCols.errorCode(), afterCols.errorMessage());
        auto afterTasks = queryScalarInt(db, "SELECT COUNT(*) AS v FROM todo_tasks");
        if (afterTasks.isFail())
            return core::Result<void>::fail(afterTasks.errorCode(), afterTasks.errorMessage());

        if (afterTasks.value() != beforeTasks.value()) {
            LOG_ERROR("Todo 固定四列迁移任务数不守恒（前 " +
                      std::to_string(beforeTasks.value()) + " / 后 " +
                      std::to_string(afterTasks.value()) +
                      "）——疑似 CASCADE 误删，整段回滚");
            return core::Result<void>::fail(core::ErrorCode::DatabaseMigrationFailed,
                                            "fixed-column migration lost tasks; rolled back");
        }

        if (changed) {
            LOG_INFO("Todo 固定四列迁移: 列 " + std::to_string(beforeCols.value()) + "→" +
                     std::to_string(afterCols.value()) + "（重建 " +
                     std::to_string(createdColumns) + " / 合并删列 " +
                     std::to_string(mergedColumns) + "），任务 " +
                     std::to_string(beforeTasks.value()) + "→" +
                     std::to_string(afterTasks.value()));
        }
        return core::Result<void>::ok();
    });
}

core::Result<void> addMissingAgentColumns(core::data::SqliteDb& db) {
    // 逐个表对齐：缺列补列，不缺列零操作。三张表各自独立——任一张补列失败主调方中止。
    // 表/列一律枚举入参，标识符只从白名单取（见 ensureTableColumns 的两段校验）。
    auto r1 = ensureTableColumns(db, MigrationTable::AgentTasks, agentTasksColumns());
    if (r1.isFail()) return r1;
    auto r2 = ensureTableColumns(db, MigrationTable::AgentRuns, agentRunsColumns());
    if (r2.isFail()) return r2;
    auto r3 = ensureTableColumns(db, MigrationTable::AgentMessages, agentMessagesColumns());
    if (r3.isFail()) return r3;
    return core::Result<void>::ok();
}

core::Result<void> migrateDropDeprecatedSharedConfig(core::data::SqliteDb& db) {
    // 共享片段功能已整体删除：老库的表与列必须清掉；全新库两者都不存在，直接 no-op。
    // 幂等：表/列存在性检查后动 DDL，重复调用不再命中（DROP TABLE IF EXISTS 亦兼容）。
    auto table = tableExists(db, "claude_shared_configs");
    if (table.isFail())
        return core::Result<void>::fail(table.errorCode(), table.errorMessage());
    if (table.value()) {
        auto drop = db.exec("DROP TABLE IF EXISTS claude_shared_configs");
        if (drop.isFail()) return drop;
    }
    auto cols = existingColumns(db, "claude_profiles");
    if (cols.isFail())
        return core::Result<void>::fail(cols.errorCode(), cols.errorMessage());
    if (cols.value().count("shared_config_id") != 0) {
        auto alter = db.exec("ALTER TABLE claude_profiles DROP COLUMN shared_config_id");
        if (alter.isFail()) return alter;
    }
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

core::Result<void> migrate(core::data::SqliteDb& db) {
    // 先建当前 schema（老库缺的表/索引补建），再补列与 Todo 固定四列，最后清遗留表。
    auto r = db.applySchema(kSql);
    if (r.isFail()) {
        LOG_ERROR("Database schema 应用失败: " + r.errorMessage());
        return r;
    }
    // 共享片段表/列已废弃：老库在补列前先清掉，避免遗留 schema 继续被带着走。
    auto dropShared = migrateDropDeprecatedSharedConfig(db);
    if (dropShared.isFail()) {
        LOG_ERROR("共享片段遗留 schema 清理失败: " + dropShared.errorMessage());
        return dropShared;
    }
    auto colMig = addMissingAgentColumns(db);
    if (colMig.isFail()) {
        LOG_ERROR("agent 表列对齐失败: " + colMig.errorMessage());
        return colMig;
    }
    // Todo 固定四列迁移：与 Todo 模块是否启用无关（模块关闭时首页逾期/待办口径
    // 仍正确）。必须晚于 applySchema（表/种子已建），整段自带幂等与单事务。
    auto todoMig = migrateTodoFixedColumns(db);
    if (todoMig.isFail()) {
        LOG_ERROR("Todo 固定四列迁移失败: " + todoMig.errorMessage());
        return todoMig;
    }
    auto drop = dropRemovedTables(db);
    if (drop.isFail()) {
        LOG_ERROR("遗留表清理失败: " + drop.errorMessage());
        return drop;
    }
    return core::Result<void>::ok();
}

} // namespace core::schema
