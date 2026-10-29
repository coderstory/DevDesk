// tests/test_core/test_sql_dump.cpp
// core::sql_dump：索引/触发器导出、skip 表与 FTS 影子表/自动索引排除、
// 回环（dump → 重建 → 再 dump → 比对）、同一份 dump 幂等重导入、任一句失败整体回滚。
// 夹具 schema 特意摆出三条会真炸的形态：FK 子表（agent_runs）按表名字典序先于父表
// （agent_tasks）落库（导不关外键 = 导入必败）；AFTER INSERT 触发器写另一张被导出的表
// audit_log（触发器若被挪回 INSERT 之前建，回环比对必红——audit_log 多出点火行）；
// 名字带 _config 的真实业务表 app_config（影子表若按子串误判 = 静默从备份消失）。
#include "doctest.h"

#include "core/sql_dump.h"
#include "core/sqlite_db.h"

#include <cstdint>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

namespace {

// 源库 schema + 数据。执行成败回传给调用方断言（doctest 断言宏只能在用例体内用）
bool makeSourceDb(core::data::SqliteDb& db) {
    const char* schema = R"(
        CREATE TABLE agent_tasks (id INTEGER PRIMARY KEY, title TEXT NOT NULL, status TEXT);
        CREATE TABLE agent_runs (id INTEGER PRIMARY KEY, task_id INTEGER NOT NULL REFERENCES agent_tasks(id), status TEXT);
        CREATE TABLE audit_log (id INTEGER PRIMARY KEY AUTOINCREMENT, note TEXT NOT NULL);
        CREATE TABLE tags_unique (name TEXT NOT NULL UNIQUE);
        CREATE TABLE app_config (k TEXT PRIMARY KEY, v TEXT NOT NULL DEFAULT '');
        CREATE TABLE claude_call_logs (id INTEGER PRIMARY KEY, model TEXT);
        CREATE TABLE claude_run_settings (k TEXT PRIMARY KEY, v TEXT NOT NULL);
        CREATE TABLE github_sync_state (key TEXT PRIMARY KEY, value TEXT NOT NULL);
        CREATE VIRTUAL TABLE markdown_fts USING fts5(title, body);
        CREATE INDEX idx_runs_task ON agent_runs(task_id);
        CREATE UNIQUE INDEX idx_tasks_title ON agent_tasks(title);
        CREATE TRIGGER trg_runs_audit AFTER INSERT ON agent_runs
            BEGIN INSERT INTO audit_log(note) VALUES('fired:' || NEW.id); END;
    )";
    const char* rows = R"(
        INSERT INTO agent_tasks(id, title, status) VALUES (1, 'task one', NULL), (2, 'task ''quoted''', 'done');
        INSERT INTO audit_log(note) VALUES ('seed-a');
        INSERT INTO agent_runs(id, task_id, status) VALUES (10, 1, 'ok'), (11, 2, NULL);
        INSERT INTO tags_unique(name) VALUES ('alpha');
        INSERT INTO app_config(k, v) VALUES ('p1', '[{"model":"m"}]');
        INSERT INTO claude_call_logs(id, model) VALUES (1, 'skip-me');
        INSERT INTO claude_run_settings(k, v) VALUES ('k1', 'v1');
        INSERT INTO github_sync_state(key, value) VALUES ('sha:notes/a.md', 'baseline-secret-sha'), ('first_sync_done', 'true');
        INSERT INTO markdown_fts(rowid, title, body) VALUES (1, 'note one', 'hello world'), (2, 'note two', 'second doc');
    )";
    return db.exec(schema).isOk() && db.exec(rows).isOk();
}

// 两份 dump 只允许生成时间行不同：剥掉该行再比
std::string stripTimestampLine(const std::string& dump) {
    std::istringstream in(dump);
    std::ostringstream out;
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("-- Generated at:", 0) == 0) continue;
        out << line << '\n';
    }
    return out.str();
}

bool queryCount(core::data::SqliteDb& db, const char* table, int64_t* out) {
    auto pr = db.prepare(std::string("SELECT COUNT(*) FROM \"") + table + "\"");
    if (pr.isFail()) return false;
    auto stepped = pr.value().step();
    if (stepped.isFail() || !stepped.value()) return false;
    auto row = pr.value().currentRow();
    if (row.isFail() || row.value().values.empty()) return false;
    if (auto v = std::get_if<int64_t>(&row.value().values[0])) {
        *out = *v;
        return true;
    }
    return false;
}

} // namespace

REGISTER_TEST(sql_dump_skip_defaults, "SqlDump: defaultSkipTables 固定为 call_logs + run_settings + github_sync_state + project_cards", {
    const auto& tables = core::sql_dump::defaultSkipTables();
    DOCTEST_CHECK_EQ(tables.size(), static_cast<size_t>(4));
    DOCTEST_CHECK_EQ(tables[0], std::string("claude_call_logs"));
    DOCTEST_CHECK_EQ(tables[1], std::string("claude_run_settings"));
    // 同步基线（本机专属）必须整表排除：随 dump 跨设备 = 新设备拿别人写的
    // 比对基线 → 误判未变跳过下载 → 进而删远端笔记
    DOCTEST_CHECK_EQ(tables[2], std::string("github_sync_state"));
    // 项目卡（需-11）：test_db_url 可能含连接串凭据，不随 dump 上云
    DOCTEST_CHECK_EQ(tables[3], std::string("project_cards"));
    return true;
})

REGISTER_TEST(sql_dump_export_contents, "SqlDump: 导出含索引/触发器/PRAGMA，排除 skip 表、影子表、自动索引", {
    auto opened = core::data::SqliteDb::open(":memory:");
    DOCTEST_REQUIRE(opened.isOk());
    auto& db = opened.value();
    DOCTEST_CHECK(makeSourceDb(db));

    auto dump = core::sql_dump::exportToString(db, core::sql_dump::defaultSkipTables());
    DOCTEST_REQUIRE(dump.isOk());
    const std::string& s = dump.value();

    // 索引：显式 CREATE INDEX / CREATE UNIQUE INDEX 的原 DDL + 幂等 DROP
    DOCTEST_CHECK_MESSAGE(s.find("-- Index: idx_runs_task") != std::string::npos,
                          "缺显式索引导出");
    DOCTEST_CHECK(s.find("CREATE INDEX idx_runs_task ON agent_runs(task_id)") != std::string::npos);
    DOCTEST_CHECK(s.find("CREATE UNIQUE INDEX idx_tasks_title ON agent_tasks(title)") != std::string::npos);
    DOCTEST_CHECK(s.find("DROP INDEX IF EXISTS \"idx_runs_task\";") != std::string::npos);

    // 触发器：原 DDL + 幂等 DROP
    DOCTEST_CHECK_MESSAGE(s.find("-- Trigger: trg_runs_audit") != std::string::npos,
                          "缺触发器导出");
    DOCTEST_CHECK(s.find("DROP TRIGGER IF EXISTS \"trg_runs_audit\";") != std::string::npos);

    // 排除项：自动索引（sql 为 NULL 不可重建）、skip 表、FTS 影子表
    DOCTEST_CHECK(s.find("sqlite_autoindex") == std::string::npos);
    DOCTEST_CHECK(s.find("claude_call_logs") == std::string::npos);
    DOCTEST_CHECK(s.find("claude_run_settings") == std::string::npos);
    DOCTEST_CHECK(s.find("markdown_fts_content") == std::string::npos);
    DOCTEST_CHECK(s.find("markdown_fts_idx") == std::string::npos);
    DOCTEST_CHECK(s.find("markdown_fts_docsize") == std::string::npos);
    DOCTEST_CHECK(s.find("markdown_fts_data") == std::string::npos);
    // FTS 虚表本体要导
    DOCTEST_CHECK(s.find("CREATE VIRTUAL TABLE markdown_fts USING fts5(title, body)") != std::string::npos);

    // 影子表判定回归锚：真实业务表 app_config 名字带 _config，不能被误判成影子表
    DOCTEST_CHECK_MESSAGE(s.find("-- Table: app_config") != std::string::npos,
                          "app_config 被影子表误判排除");

    // 段顺序：索引 DDL 在 INSERT 之前，触发器 DDL 在 INSERT 之后
    const auto indexPos = s.find("-- Index: idx_runs_task");
    const auto triggerPos = s.find("-- Trigger: trg_runs_audit");
    const auto firstInsert = s.find("INSERT INTO \"agent_runs\"");
    DOCTEST_CHECK(indexPos < firstInsert);
    DOCTEST_CHECK(triggerPos > firstInsert);

    // CLI 直跑配套：外键开关 + 事务外壳
    DOCTEST_CHECK(s.find("PRAGMA foreign_keys=OFF;") != std::string::npos);
    DOCTEST_CHECK(s.find("PRAGMA foreign_keys=ON;") != std::string::npos);
    DOCTEST_CHECK(s.find("BEGIN TRANSACTION;") != std::string::npos);
    DOCTEST_CHECK(s.find("COMMIT;") != std::string::npos);

    // 数据保真：单引号转义与 skip 表之外的数据行都在
    DOCTEST_CHECK(s.find("task ''quoted''") != std::string::npos);
    DOCTEST_CHECK(s.find("[{\"model\":\"m\"}]") != std::string::npos);
    return true;
})

REGISTER_TEST(sql_dump_round_trip, "SqlDump: 回环 dump→重建→再 dump 比对一致", {
    auto src = core::data::SqliteDb::open(":memory:");
    DOCTEST_REQUIRE(src.isOk());
    DOCTEST_CHECK(makeSourceDb(src.value()));
    auto dump1 = core::sql_dump::exportToString(src.value(), core::sql_dump::defaultSkipTables());
    DOCTEST_REQUIRE(dump1.isOk());

    // 重建：全新库导入。FK 子表行先于父表行落库 + 触发器点火数据 + FTS 行都在比对范围内
    auto rebuilt = core::data::SqliteDb::open(":memory:");
    DOCTEST_REQUIRE(rebuilt.isOk());
    auto imported = core::sql_dump::importFromString(rebuilt.value(), dump1.value());
    DOCTEST_CHECK_MESSAGE(imported.isOk(), "导入失败: " + imported.errorMessage());

    auto dump2 = core::sql_dump::exportToString(rebuilt.value(), core::sql_dump::defaultSkipTables());
    DOCTEST_REQUIRE(dump2.isOk());
    DOCTEST_CHECK_EQ(stripTimestampLine(dump1.value()), stripTimestampLine(dump2.value()));

    // 回环后数据行数不增不减（触发器点火不会放大 audit_log）
    int64_t auditRows = -1;
    DOCTEST_CHECK(queryCount(rebuilt.value(), "audit_log", &auditRows));
    DOCTEST_CHECK_EQ(auditRows, static_cast<int64_t>(3));
    int64_t runRows = -1;
    DOCTEST_CHECK(queryCount(rebuilt.value(), "agent_runs", &runRows));
    DOCTEST_CHECK_EQ(runRows, static_cast<int64_t>(2));

    // 全新库恢复边界：skip 表不在 dump 里 → 重建库没有这些表（编排层需先建好 schema 再导入）
    auto probe = rebuilt.value().prepare("SELECT COUNT(*) FROM claude_call_logs");
    DOCTEST_CHECK(probe.isFail());
    return true;
})

REGISTER_TEST(sql_dump_reimport_idempotent, "SqlDump: 同一份 dump 对已导入库重复导入结果不变", {
    auto src = core::data::SqliteDb::open(":memory:");
    DOCTEST_REQUIRE(src.isOk());
    DOCTEST_CHECK(makeSourceDb(src.value()));
    auto dump1 = core::sql_dump::exportToString(src.value(), core::sql_dump::defaultSkipTables());
    DOCTEST_REQUIRE(dump1.isOk());

    auto db = core::data::SqliteDb::open(":memory:");
    DOCTEST_REQUIRE(db.isOk());
    DOCTEST_CHECK(core::sql_dump::importFromString(db.value(), dump1.value()).isOk());
    // 第二次原样导入：DROP IF EXISTS 吃掉既有对象，不得报错、不得翻倍
    DOCTEST_CHECK_MESSAGE(core::sql_dump::importFromString(db.value(), dump1.value()).isOk(),
                          "重复导入失败（幂等破坏）");
    auto dump3 = core::sql_dump::exportToString(db.value(), core::sql_dump::defaultSkipTables());
    DOCTEST_REQUIRE(dump3.isOk());
    DOCTEST_CHECK_EQ(stripTimestampLine(dump1.value()), stripTimestampLine(dump3.value()));

    int64_t auditRows = -1;
    DOCTEST_CHECK(queryCount(db.value(), "audit_log", &auditRows));
    DOCTEST_CHECK_EQ(auditRows, static_cast<int64_t>(3));
    return true;
})

REGISTER_TEST(sql_dump_import_rollback, "SqlDump: 导入中途一句失败整体回滚，库保持导入前原样", {
    auto src = core::data::SqliteDb::open(":memory:");
    DOCTEST_REQUIRE(src.isOk());
    DOCTEST_CHECK(makeSourceDb(src.value()));
    auto dump1 = core::sql_dump::exportToString(src.value(), core::sql_dump::defaultSkipTables());
    DOCTEST_REQUIRE(dump1.isOk());

    // 目标库先导入一次，作为「导入前原样」的基准
    auto db = core::data::SqliteDb::open(":memory:");
    DOCTEST_REQUIRE(db.isOk());
    DOCTEST_CHECK(core::sql_dump::importFromString(db.value(), dump1.value()).isOk());
    auto before = core::sql_dump::exportToString(db.value(), core::sql_dump::defaultSkipTables());
    DOCTEST_REQUIRE(before.isOk());

    // 同一份 dump 再导 + 尾部追加坏句：中途失败必须整批回滚
    std::string bad = dump1.value();
    bad += "INSERT INTO no_such_table VALUES(1);\n";
    auto result = core::sql_dump::importFromString(db.value(), bad);
    DOCTEST_CHECK_MESSAGE(result.isFail(), "坏句未报错（回滚守卫失效）");

    auto after = core::sql_dump::exportToString(db.value(), core::sql_dump::defaultSkipTables());
    DOCTEST_REQUIRE(after.isOk());
    DOCTEST_CHECK_EQ(stripTimestampLine(before.value()), stripTimestampLine(after.value()));
    return true;
})

// ---- 同步基线（github_sync_state）必须留在本机 ----

REGISTER_TEST(sql_dump_excludes_sync_baseline,
              "SqlDump: 导出不含同步基线（github_sync_state 整表排除，带在库对照）", {
    auto opened = core::data::SqliteDb::open(":memory:");
    DOCTEST_REQUIRE(opened.isOk());
    auto& db = opened.value();
    DOCTEST_REQUIRE(makeSourceDb(db));

    auto dumped = core::sql_dump::exportToString(
        db, core::sql_dump::defaultSkipTables());
    DOCTEST_REQUIRE(dumped.isOk());
    const std::string& dump = dumped.value();
    // 表名与行值都不进 dump：基线 sha / 首次方向标记随 dump 转移 = 新设备
    // 拿别人写的基线比对 → 误判未变跳过下载 → 进而删远端笔记
    DOCTEST_CHECK(dump.find("github_sync_state") == std::string::npos);
    DOCTEST_CHECK(dump.find("baseline-secret-sha") == std::string::npos);
    DOCTEST_CHECK(dump.find("first_sync_done") == std::string::npos);
    // 对照组：同库的普通表在 dump 里（证明上面的 find 不是恒假）
    DOCTEST_CHECK(dump.find("agent_tasks") != std::string::npos);
    return true;
})

REGISTER_TEST(sql_dump_import_keeps_local_sync_baseline,
              "SqlDump: 新设备导入远端 dump 后本机同步基线原样存活（审计 C1）", {
    // 设备 A：AB 各自的基线不同；A 的基线不得随 dump 落到 B
    auto openedA = core::data::SqliteDb::open(":memory:");
    DOCTEST_REQUIRE(openedA.isOk());
    DOCTEST_REQUIRE(makeSourceDb(openedA.value()));
    auto dumpedA = core::sql_dump::exportToString(
        openedA.value(), core::sql_dump::defaultSkipTables());
    DOCTEST_REQUIRE(dumpedA.isOk());

    auto openedB = core::data::SqliteDb::open(":memory:");
    DOCTEST_REQUIRE(openedB.isOk());
    auto& b = openedB.value();
    DOCTEST_REQUIRE(makeSourceDb(b));
    DOCTEST_CHECK(b.exec("UPDATE github_sync_state SET value='b-own-sha' "
                         "WHERE key='sha:notes/a.md'")
                      .isOk());

    auto imported = core::sql_dump::importFromString(b, dumpedA.value());
    DOCTEST_REQUIRE(imported.isOk());

    // B 自己的基线原样存活：既没被 A 的 dump 覆盖，也没被 DROP 重建清空
    auto pr = b.prepare(
        "SELECT value FROM github_sync_state WHERE key='sha:notes/a.md'");
    DOCTEST_REQUIRE(pr.isOk());
    auto stepped = pr.value().step();
    DOCTEST_REQUIRE(stepped.isOk());
    DOCTEST_REQUIRE(stepped.value());   // 行还在（未被清空）
    auto row = pr.value().currentRow();
    DOCTEST_REQUIRE(row.isOk());
    auto value = row.value().getString("value");
    DOCTEST_REQUIRE(value.isOk());
    DOCTEST_CHECK_EQ(value.value(), std::string("b-own-sha"));
    return true;
})
