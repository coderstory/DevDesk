// 只断言 Database::migrate() 落下的**当前** schema 形态：src/core/schema.h 是一份幂等
// 脚本，applySchema 一个事务跑完，没有版本号 / schema_version 表 / 旧库升级路径。
// 改 schema 的唯一途径就是改这份脚本，所以本文件只钉「最终形态」与「幂等」
#include "doctest.h"
#include "test_support.h"
#include "core/database.h"
#include "core/result.h"
#include "core/schema.h"
#include "core/sqlite_db.h"

#include <filesystem>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace {

// 给测试生成一个临时 db 路径，不污染应用数据库
constexpr const char* kDbPrefix = "devdesk_test_schema_";

std::set<std::string> listObjects(core::data::SqliteDb& db, const std::string& type) {
    auto prep = db.prepare("SELECT name FROM sqlite_master WHERE type = ?");
    if (prep.isFail()) return {};
    if (prep.value().bind(1, type).isFail()) return {};
    auto rows = prep.value().fetchAll();
    if (rows.isFail()) return {};
    std::set<std::string> names;
    for (const auto& row : rows.value()) {
        auto name = row.getString("name");
        if (name.isOk()) names.insert(name.value());
    }
    return names;
}

// 读一列的完整定义序列（name:type:notnull:default），顺序 = 建表顺序。default 为 NULL
// 时返回 "<none>"，与 sqlite_master 里 dflt_value=NULL 对齐。
// notnull 是 PRAGMA table_info 的**值**（0/1 整数），不是「有没有这列」——INTEGER
// PRIMARY KEY 也报 notnull=0，这是 SQLite 的历史行为，照抄实际值，不要写成 1
std::vector<std::string> columnDefs(core::data::SqliteDb& db, const std::string& table) {
    auto prep = db.prepare("PRAGMA table_info(" + table + ")");
    if (prep.isFail()) return {};
    auto rows = prep.value().fetchAll();
    if (rows.isFail()) return {};
    std::vector<std::string> out;
    for (const auto& row : rows.value()) {
        std::string def = row.getString("name").valueOr("?") + ":" +
                          row.getString("type").valueOr("?") + ":" +
                          std::to_string(row.getInt64("notnull").valueOr(-1)) + ":" +
                          (row.isNull("dflt_value") ? std::string("<none>")
                                                     : row.getString("dflt_value").valueOr("?"));
        out.push_back(def);
    }
    return out;
}

} // namespace


REGISTER_TEST(schema_creates_all_tables,
              "schema: applySchema 建出全部 15 张应用表", {
    auto path = test_support::tempDbPath(kDbPrefix, "create");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    {
        auto r = core::Database::open(path);
        DOCTEST_REQUIRE(r.isOk());
        DOCTEST_REQUIRE(r.value().migrate().isOk());

        auto tables = listObjects(r.value().db(), "table");
        std::set<std::string> expected{
            "markdown_index",
            "markdown_tags",
            "markdown_note_tags",
            "markdown_fts",  // FTS5 虚表
            "todo_columns",
            "todo_tasks",
            "claude_profiles",
            "claude_call_logs",
            "claude_shared_configs",
            "agent_tasks",
            "agent_runs",
            "agent_messages",
            // 六类 JSON 配置的落点：通用表 + run-settings 记录表
            "app_config",
            "claude_run_settings",
        };
        for (const auto& t : expected) {
            bool found = tables.count(t) > 0;
            DOCTEST_CHECK(found);
            if (!found) {
                std::cout << "  [DEBUG] missing table: " << t << std::endl;
            }
        }

                // 9 个索引（含 1 个部分唯一索引 + 3 个带 WHERE 的部分索引 + agent 3 个 + run-settings 1 个）
        auto indexes = listObjects(r.value().db(), "index");
        std::set<std::string> expectedIndexes{
            "idx_tasks_column",
            "idx_tasks_due",
            "idx_claude_active",
            "idx_call_logs_started",
            "idx_call_logs_profile",
            "idx_agent_runs_task",
            "idx_agent_messages_run",
            // agent_runs 按 status 的部分索引：只收 running 行（轮询 + 启动清扫）
            "idx_agent_runs_running",
            // 直连会话记录按写入时刻倒序取最近
            "idx_run_settings_written",
        };
        for (const auto& i : expectedIndexes) {
            bool found = indexes.count(i) > 0;
            DOCTEST_CHECK(found);
            if (!found) {
                std::cout << "  [DEBUG] missing index: " << i << std::endl;
            }
        }
    } // Database 在此析构（关闭 sqlite 句柄），Windows 不允许删除打开中的文件

    std::filesystem::remove(path, ec);
    return true;
})

// 库里不得再有 schema_version 表：口径是「单份当前 schema、无历史包袱」，留着这张表
// 等于留了个版本号残骸
REGISTER_TEST(schema_has_no_version_table,
              "schema: 不存在 schema_version 表（无版本化残骸）", {
    auto path = test_support::tempDbPath(kDbPrefix, "no_version");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    {
        auto r = core::Database::open(path);
        DOCTEST_REQUIRE(r.isOk());
        DOCTEST_REQUIRE(r.value().migrate().isOk());
        auto& db = r.value().db();

        auto tables = listObjects(db, "table");
        DOCTEST_CHECK(tables.count("schema_version") == 0);

        // 也不该有任何触发器 / 视图（本项目 schema 只有表 + 索引）
        DOCTEST_CHECK(listObjects(db, "trigger").empty());
        DOCTEST_CHECK(listObjects(db, "view").empty());

        // 查询旧列也必须失败（不是改名）
        DOCTEST_CHECK(db.prepare("SELECT MAX(version) FROM schema_version").isFail());
    }

    std::filesystem::remove(path, ec);
    return true;
})

// 逐表逐列钉最终形态：每张表的 name:type:notnull:default 序列逐字比对，顺序敏感。
// 从 schema.h 删掉任一列 / 改任一默认值 / 类型漂移，对应用例都会红

REGISTER_TEST(schema_markdown_tables_columns,
              "schema: markdown 三表 + FTS5 虚表列定义符合最终形态", {
    auto path = test_support::tempDbPath(kDbPrefix, "cols_md");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    {
        auto r = core::Database::open(path);
        DOCTEST_REQUIRE(r.isOk());
        DOCTEST_REQUIRE(r.value().migrate().isOk());
        auto& db = r.value().db();

        std::vector<std::string> expectIndex{
            "id:INTEGER:0:<none>",
            "path:TEXT:1:<none>",
            "title:TEXT:0:<none>",
            "size_bytes:INTEGER:1:<none>",
            "modified_at:INTEGER:1:<none>",
            "created_at:INTEGER:1:<none>",
        };
        DOCTEST_CHECK_EQ(columnDefs(db, "markdown_index"), expectIndex);

        std::vector<std::string> expectTags{
            "id:INTEGER:0:<none>",
            "name:TEXT:1:<none>",
        };
        DOCTEST_CHECK_EQ(columnDefs(db, "markdown_tags"), expectTags);

        std::vector<std::string> expectNoteTags{
            "note_id:INTEGER:1:<none>",
            "tag_id:INTEGER:1:<none>",
        };
        DOCTEST_CHECK_EQ(columnDefs(db, "markdown_note_tags"), expectNoteTags);

        // FTS5 虚表：path 不入索引（UNINDEXED），title/content 入索引。虚表的 type
        // 一律是空串（FTS5 自己不声明列类型），照抄 PRAGMA 的实际返回值，不臆断成 TEXT
        std::vector<std::string> expectFts{
            "path::0:<none>",
            "title::0:<none>",
            "content::0:<none>",
        };
        DOCTEST_CHECK_EQ(columnDefs(db, "markdown_fts"), expectFts);

        // 虚表真能用：写入后可 MATCH 命中。查询词必须用拉丁词元——unicode61 把连续 CJK
        // 当成单个 token，CJK 子串命中靠 service 层的 LIKE 兜底（markdown_service.cpp
        // 路径 B），不是 FTS MATCH
        DOCTEST_REQUIRE(db.exec(
            "INSERT INTO markdown_fts (path, title, content) VALUES "
            "('a.md', 'Title', 'needlework 正文关键词')").isOk());
        auto q = db.prepare("SELECT path FROM markdown_fts WHERE markdown_fts MATCH ?");
        DOCTEST_REQUIRE(q.isOk());
        DOCTEST_REQUIRE(q.value().bind(1, std::string("needlework")).isOk());
        auto rows = q.value().fetchAll();
        DOCTEST_REQUIRE(rows.isOk());
        DOCTEST_REQUIRE_EQ(rows.value().size(), 1u);
        DOCTEST_CHECK_EQ(rows.value()[0].getString("path").valueOr("?"),
                         std::string("a.md"));
    }

    std::filesystem::remove(path, ec);
    return true;
})

REGISTER_TEST(schema_todo_tables_columns,
              "schema: todo 两表列定义符合最终形态（含 kind 语义标记列）", {
    auto path = test_support::tempDbPath(kDbPrefix, "cols_todo");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    {
        auto r = core::Database::open(path);
        DOCTEST_REQUIRE(r.isOk());
        DOCTEST_REQUIRE(r.value().migrate().isOk());
        auto& db = r.value().db();

        // kind 可空、无默认值：NULL 表示普通列
        std::vector<std::string> expectColumns{
            "id:INTEGER:0:<none>",
            "title:TEXT:1:<none>",
            "position:INTEGER:1:<none>",
            "color:TEXT:0:<none>",
            "created_at:INTEGER:1:<none>",
            "kind:TEXT:0:<none>",
        };
        DOCTEST_CHECK_EQ(columnDefs(db, "todo_columns"), expectColumns);

        std::vector<std::string> expectTasks{
            "id:INTEGER:0:<none>",
            "column_id:INTEGER:1:<none>",
            "title:TEXT:1:<none>",
            "description:TEXT:0:<none>",
            "priority:INTEGER:1:0",
            "due_at:INTEGER:0:<none>",
            "tags:TEXT:0:<none>",
            "position:INTEGER:1:<none>",
            "completed_at:INTEGER:0:<none>",
            "created_at:INTEGER:1:<none>",
            "updated_at:INTEGER:1:<none>",
            "archived_at:INTEGER:0:<none>",
        };
        DOCTEST_CHECK_EQ(columnDefs(db, "todo_tasks"), expectTasks);
    }

    std::filesystem::remove(path, ec);
    return true;
})

REGISTER_TEST(schema_claude_tables_columns,
              "schema: claude 三表列定义符合最终形态（17/15/5 列，片段表无 extra_env）", {
    auto path = test_support::tempDbPath(kDbPrefix, "cols_claude");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    {
        auto r = core::Database::open(path);
        DOCTEST_REQUIRE(r.isOk());
        DOCTEST_REQUIRE(r.value().migrate().isOk());
        auto& db = r.value().db();

        std::vector<std::string> expectProfiles{
            "id:INTEGER:0:<none>",
            "name:TEXT:1:<none>",
            "base_url:TEXT:1:<none>",
            "api_key_hint:TEXT:1:<none>",
            "api_key_check:TEXT:1:<none>",
            "api_key_encrypted:TEXT:1:''",
            "model:TEXT:1:<none>",
            "is_active:INTEGER:1:0",
            "created_at:INTEGER:1:<none>",
            "updated_at:INTEGER:1:<none>",
            "models_config:TEXT:1:''",
            "extra_env:TEXT:1:''",
            "shared_config_id:INTEGER:1:0",
            "auth_field:TEXT:1:'both'",
            "dialect_override:TEXT:1:''",
            "fallback_profile_id:INTEGER:1:0",
            "extra_settings_json:TEXT:1:''",
        };
        DOCTEST_CHECK_EQ(columnDefs(db, "claude_profiles"), expectProfiles);

                // 调用日志 15 列：末尾 5 列是 usage，全部可空 = 未采集，与真实的 0 区分开
        std::vector<std::string> expectLogs{
            "id:INTEGER:0:<none>",
            "profile_id:INTEGER:1:<none>",
            "method:TEXT:1:<none>",
            "path:TEXT:1:<none>",
            "status_code:INTEGER:0:<none>",
            "latency_ms:INTEGER:0:<none>",
            "request_bytes:INTEGER:0:<none>",
            "response_bytes:INTEGER:0:<none>",
            "error:TEXT:0:<none>",
            "started_at:INTEGER:1:<none>",
            "input_tokens:INTEGER:0:<none>",
            "output_tokens:INTEGER:0:<none>",
            "cache_read_tokens:INTEGER:0:<none>",
            "cache_write_tokens:INTEGER:0:<none>",
            "first_event_ms:INTEGER:0:<none>",
        };
        DOCTEST_CHECK_EQ(columnDefs(db, "claude_call_logs"), expectLogs);

        // 共享片段表 5 列，无 extra_env
        std::vector<std::string> expectShared{
            "id:INTEGER:0:<none>",
            "name:TEXT:1:<none>",
            "created_at:INTEGER:1:<none>",
            "updated_at:INTEGER:1:<none>",
            "extra_settings_json:TEXT:1:''",
        };
        DOCTEST_CHECK_EQ(columnDefs(db, "claude_shared_configs"), expectShared);

        // 旧列不存在（不是改名）
        DOCTEST_CHECK(
            db.prepare("SELECT extra_env FROM claude_shared_configs").isFail());
    }

    std::filesystem::remove(path, ec);
    return true;
})

REGISTER_TEST(schema_agent_tables_columns,
              "schema: agent 三表列定义符合最终形态（13/9/7 列，双 FK CASCADE）", {
    auto path = test_support::tempDbPath(kDbPrefix, "cols_agent");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    {
        auto r = core::Database::open(path);
        DOCTEST_REQUIRE(r.isOk());
        DOCTEST_REQUIRE(r.value().migrate().isOk());
        auto& db = r.value().db();

        // agent_tasks 14 列（UAT6：新增 system_prompt 列）
        // 默认值钉死（max_iterations=10 / allow_destructive=0 /
        // enabled=1 / model_slot='default' / context_note='' / auto_note=1 / system_prompt=''）
        std::vector<std::string> expectTasks{
            "id:INTEGER:0:<none>",
            "name:TEXT:1:<none>",
            "goal:TEXT:1:<none>",
            "trigger_type:TEXT:1:<none>",
            "trigger_config:TEXT:1:''",
            "model_slot:TEXT:1:'default'",
            "max_iterations:INTEGER:1:10",
            "allow_destructive:INTEGER:1:0",
            "enabled:INTEGER:1:1",
            "context_note:TEXT:1:''",
            "auto_note:INTEGER:1:1",
            "system_prompt:TEXT:1:''",
            "created_at:INTEGER:1:<none>",
            "updated_at:INTEGER:1:<none>",
        };
        DOCTEST_CHECK_EQ(columnDefs(db, "agent_tasks"), expectTasks);

        // agent_runs 9 列：ended_at/tokens/error 全可空，运行中 / 未采集 / 无错误
        // 与真实值区分
        std::vector<std::string> expectRuns{
            "id:INTEGER:0:<none>",
            "task_id:INTEGER:1:<none>",
            "status:TEXT:1:<none>",
            "started_at:INTEGER:1:<none>",
            "ended_at:INTEGER:0:<none>",
            "iterations:INTEGER:1:0",
            "input_tokens:INTEGER:0:<none>",
            "output_tokens:INTEGER:0:<none>",
            "error:TEXT:0:<none>",
        };
        DOCTEST_CHECK_EQ(columnDefs(db, "agent_runs"), expectRuns);

        // agent_messages 7 列：tool_call_id 可空，assistant 文本块没有调用 id
        std::vector<std::string> expectMessages{
            "id:INTEGER:0:<none>",
            "run_id:INTEGER:1:<none>",
            "seq:INTEGER:1:<none>",
            "role:TEXT:1:<none>",
            "content_json:TEXT:1:<none>",
            "tool_call_id:TEXT:0:<none>",
            "created_at:INTEGER:1:<none>",
        };
        DOCTEST_CHECK_EQ(columnDefs(db, "agent_messages"), expectMessages);

        // FK CASCADE 生效：删 task 清 runs、删 run 清 messages，审计链随宿主走
        auto ins = db.prepare(
            "INSERT INTO agent_tasks (name, goal, trigger_type, created_at, "
            "updated_at) VALUES ('t','g','manual',1,1)");
        DOCTEST_REQUIRE(ins.isOk());
        DOCTEST_REQUIRE(ins.value().step().isOk());
        auto insRun = db.prepare(
            "INSERT INTO agent_runs (task_id, status, started_at) "
            "VALUES (1, 'running', 1)");
        DOCTEST_REQUIRE(insRun.isOk());
        DOCTEST_REQUIRE(insRun.value().step().isOk());
        auto insMsg = db.prepare(
            "INSERT INTO agent_messages (run_id, seq, role, content_json, "
            "created_at) VALUES (1, 0, 'user', '[]', 1)");
        DOCTEST_REQUIRE(insMsg.isOk());
        DOCTEST_REQUIRE(insMsg.value().step().isOk());
        DOCTEST_REQUIRE(
            db.prepare("PRAGMA foreign_keys=ON").isOk());  // 连接级开关先开
        auto del = db.prepare("DELETE FROM agent_tasks WHERE id=1");
        DOCTEST_REQUIRE(del.isOk());
        DOCTEST_REQUIRE(del.value().step().isOk());
        auto leftover = db.prepare("SELECT COUNT(*) AS n FROM agent_messages");
        DOCTEST_REQUIRE(leftover.isOk());
        DOCTEST_REQUIRE(leftover.value().step().isOk());
        const auto row = leftover.value().currentRow();
        DOCTEST_REQUIRE(row.isOk());
        DOCTEST_CHECK_EQ(row.value().getInt64("n").valueOr(-1), 0);
    }

    std::filesystem::remove(path, ec);
    return true;
})

REGISTER_TEST(schema_config_and_run_settings_columns,
              "schema: app_config + claude_run_settings 列定义符合最终形态（E2）", {
    auto path = test_support::tempDbPath(kDbPrefix, "cols_cfg");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    {
        auto r = core::Database::open(path);
        DOCTEST_REQUIRE(r.isOk());
        DOCTEST_REQUIRE(r.value().migrate().isOk());
        auto& db = r.value().db();

        // app_config：通用「键 → 值」配置存储。value 恒 TEXT 且 NOT NULL，存得住任意
        // 字节（中文 / 空串 / JSON / 换行 / 超长串）；updated_at 恒 NOT NULL INTEGER。
        std::vector<std::string> expectConfig{
            "key:TEXT:0:<none>",      // TEXT PRIMARY KEY：SQLite 报 notnull=0（历史行为）
            "value:TEXT:1:<none>",
            "updated_at:INTEGER:1:<none>",
        };
        DOCTEST_CHECK_EQ(columnDefs(db, "app_config"), expectConfig);

        // claude_run_settings：run-settings 每次运行一行（单键会被覆盖，故单独建表）。
        // run_key 的 UNIQUE 由索引实现，PRAGMA table_info 不体现，故另用真插入验证。
        std::vector<std::string> expectRuns{
            "id:INTEGER:0:<none>",
            "run_key:TEXT:1:<none>",
            "profile_id:INTEGER:1:<none>",
            "profile_name:TEXT:1:''",
            "written_at:INTEGER:1:<none>",
            "file_path:TEXT:1:''",
            "settings_json:TEXT:1:''",
        };
        DOCTEST_CHECK_EQ(columnDefs(db, "claude_run_settings"), expectRuns);

        // 真能连写 N 份互不覆盖
        auto ins = db.prepare(
            "INSERT INTO claude_run_settings (run_key, profile_id, profile_name, "
            "written_at, file_path, settings_json) VALUES (?, 1, 'p', 100, ?, ?)");
        DOCTEST_REQUIRE(ins.isOk());
        for (int i = 0; i < 3; ++i) {
            DOCTEST_REQUIRE(ins.value().bind(1, "run-" + std::to_string(i)).isOk());
            DOCTEST_REQUIRE(ins.value()
                                .bind(2, "/tmp/run-" + std::to_string(i) + ".json")
                                .isOk());
            DOCTEST_REQUIRE(ins.value().bind(3, "{\"env\":{}}").isOk());
            DOCTEST_REQUIRE(ins.value().step().isOk());
            DOCTEST_REQUIRE(ins.value().reset().isOk());
        }
        auto q = db.prepare(
            "SELECT run_key, file_path FROM claude_run_settings ORDER BY run_key");
        DOCTEST_REQUIRE(q.isOk());
        auto rows = q.value().fetchAll();
        DOCTEST_REQUIRE(rows.isOk());
        DOCTEST_REQUIRE_EQ(rows.value().size(), 3u);
        DOCTEST_CHECK_EQ(rows.value()[0].getString("run_key").valueOr("?"),
                         std::string("run-0"));
        DOCTEST_CHECK_EQ(rows.value()[2].getString("run_key").valueOr("?"),
                         std::string("run-2"));
        // 每份各自的文件路径没被后一份覆盖
        DOCTEST_CHECK_EQ(rows.value()[1].getString("file_path").valueOr("?"),
                         std::string("/tmp/run-1.json"));

        // run_key 的 UNIQUE 真的生效：重复插入必须失败，不是静默覆盖
        auto dup = db.prepare(
            "INSERT INTO claude_run_settings (run_key, profile_id, written_at) "
            "VALUES ('run-1', 1, 200)");
        DOCTEST_REQUIRE(dup.isOk());
        DOCTEST_CHECK(dup.value().step().isFail());
    } // Database 在此析构（Windows 不允许删除打开中的文件）

    std::filesystem::remove(path, ec);
    return true;
})


REGISTER_TEST(schema_seeds_default_todo_columns,
              "schema: 首次建库插入 4 个默认 Todo 列且带 kind 语义标记", {
    auto path = test_support::tempDbPath(kDbPrefix, "seed");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    {
        auto r = core::Database::open(path);
        DOCTEST_REQUIRE(r.isOk());
        DOCTEST_REQUIRE(r.value().migrate().isOk());

        auto prep = r.value().db().prepare(
            "SELECT title, color, kind FROM todo_columns ORDER BY position");
        DOCTEST_REQUIRE(prep.isOk());
        auto rows = prep.value().fetchAll();
        DOCTEST_REQUIRE(rows.isOk());
        DOCTEST_REQUIRE_EQ(rows.value().size(), 4u);

        struct Expect { const char* title; const char* color; const char* kind; };
        Expect expects[4] = {
            {"待办",   "claude-clay", "todo"},
            {"进行中", "warm-amber",  "doing"},  // H20 固定语义值
            {"已完成", "muted-sage",  "done"},
            {"已归档", "warm-gray",   "archive"},
        };
        for (int i = 0; i < 4; ++i) {
            const auto& row = rows.value()[static_cast<size_t>(i)];
            DOCTEST_CHECK_EQ(row.getString("title").valueOr("?"),
                             std::string(expects[i].title));
            DOCTEST_CHECK_EQ(row.getString("color").valueOr("?"),
                             std::string(expects[i].color));
            if (expects[i].kind[0] == '\0') {
                DOCTEST_CHECK(row.isNull("kind"));
            } else {
                DOCTEST_CHECK_EQ(row.getString("kind").valueOr("?"),
                                 std::string(expects[i].kind));
            }
        }
    } // Database 在此析构（关闭 sqlite 句柄）

    std::filesystem::remove(path, ec);
    return true;
})


// UAT6.2 H20：Todo 固定四列迁移挂在 Database::open / migrate 的启动路径
// （applySchema → addMissingAgentColumns → migrateTodoFixedColumns），
// 与 Todo 模块是否启用无关；老库重开后必须收敛到固定四列，且任务行数守恒。
REGISTER_TEST(schema_migrate_todo_fixed_columns,
              "schema: Todo 固定四列迁移（老库补 doing / 重建缺列 / 并多余列 / 任务守恒）", {
    auto path = test_support::tempDbPath(kDbPrefix, "todo_fixed");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    // 阶段 1：open 已跑过一次迁移；这里手工退回 H20 之前的老库形态
    {
        auto r = core::Database::open(path);
        DOCTEST_REQUIRE(r.isOk());
        auto& db = r.value().db();

        // 进行中退回 kind=NULL（老种子形态）
        DOCTEST_REQUIRE(db.exec(
            "UPDATE todo_columns SET kind = NULL WHERE kind = 'doing'").isOk());
        // 删掉已归档列（模拟用户删过默认列）——重开时应按固定形态重建
        DOCTEST_REQUIRE(db.exec(
            "DELETE FROM todo_columns WHERE kind = 'archive'").isOk());
        // 多余自定义列（kind 为空）+ 两个任务：迁移后应并入待办列再删列
        DOCTEST_REQUIRE(db.exec(
            "INSERT INTO todo_columns (title, position, color, created_at, kind) "
            "VALUES ('自定义', 9, NULL, 0, NULL)").isOk());
        DOCTEST_REQUIRE(db.exec(
            "INSERT INTO todo_tasks (column_id, title, priority, position, created_at, updated_at) "
            "VALUES ((SELECT id FROM todo_columns WHERE title = '自定义'), '并入1', 0, 0, 0, 0), "
            "((SELECT id FROM todo_columns WHERE title = '自定义'), '并入2', 0, 1, 0, 0)").isOk());
    } // 关闭库，触发下次 open 的完整 migrate 启动路径

    // 阶段 2：重开 → 迁移收敛
    {
        auto r = core::Database::open(path);
        DOCTEST_REQUIRE(r.isOk());
        auto& db = r.value().db();

        // 四列固定顺序 + kind（进行中补回 doing；已归档列重建）
        auto prep = db.prepare(
            "SELECT title, kind, position FROM todo_columns "
            "ORDER BY position ASC, id ASC");
        DOCTEST_REQUIRE(prep.isOk());
        auto rows = prep.value().fetchAll();
        DOCTEST_REQUIRE(rows.isOk());
        DOCTEST_REQUIRE_EQ(rows.value().size(), 4u);
        struct Expect { const char* title; const char* kind; int position; };
        const Expect expects[4] = {
            {"待办", "todo", 0},
            {"进行中", "doing", 1},
            {"已完成", "done", 2},
            {"已归档", "archive", 3},
        };
        for (int i = 0; i < 4; ++i) {
            const auto& row = rows.value()[static_cast<size_t>(i)];
            DOCTEST_CHECK_EQ(row.getString("title").valueOr("?"),
                             std::string(expects[i].title));
            DOCTEST_CHECK_EQ(row.getString("kind").valueOr("?"),
                             std::string(expects[i].kind));
            DOCTEST_CHECK_EQ(row.getInt64("position").valueOr(-1), expects[i].position);
        }

        // 多余列已删；两个任务原样保留并全部并入待办列（行数守恒）
        auto count = [&db](const std::string& sql) -> int64_t {
            auto p = db.prepare(sql);
            if (p.isFail()) return -1;
            auto rs = p.value().fetchAll();
            if (rs.isFail() || rs.value().empty()) return -1;
            return rs.value()[0].values.empty()
                       ? -1
                       : std::get<int64_t>(rs.value()[0].values[0]);
        };
        DOCTEST_CHECK_EQ(count("SELECT COUNT(*) FROM todo_columns WHERE title = '自定义'"), 0);
        DOCTEST_CHECK_EQ(count("SELECT COUNT(*) FROM todo_tasks"), 2);
        DOCTEST_CHECK_EQ(
            count("SELECT COUNT(*) FROM todo_tasks WHERE column_id IN "
                  "(SELECT id FROM todo_columns WHERE kind = 'todo')"),
            2);

        // 幂等：再跑两次启动迁移，列/任务数保持不变
        DOCTEST_REQUIRE(r.value().migrate().isOk());
        DOCTEST_REQUIRE(r.value().migrate().isOk());
        DOCTEST_CHECK_EQ(count("SELECT COUNT(*) FROM todo_columns"), 4);
        DOCTEST_CHECK_EQ(count("SELECT COUNT(*) FROM todo_tasks"), 2);
    }

    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + "-wal", ec);
    std::filesystem::remove(path.string() + "-shm", ec);
    // 迁移前备份文件（<db名>.backup-<时间戳>）一并清理，避免临时目录残留。
    // 用带 ec 的 increment（非抛异常重载，本 target -fno-exceptions）。
    {
        std::error_code iterEc;
        std::error_code rmEc;
        const std::string prefix = path.filename().string() + ".backup-";
        for (std::filesystem::directory_iterator it(path.parent_path(), iterEc), end;
             !iterEc && it != end; it.increment(iterEc)) {
            const std::string name = it->path().filename().string();
            if (name.rfind(prefix, 0) == 0) {
                std::filesystem::remove(it->path(), rmEc);
            }
        }
    }
    return true;
})

REGISTER_TEST(schema_is_idempotent,
              "schema: 重复 applySchema 幂等（不报错 / schema 不变 / 种子不重复）", {
    auto path = test_support::tempDbPath(kDbPrefix, "idempotent");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    // 第一次：记录落库后的完整 schema 形态
    std::vector<std::string> firstObjects;
    std::vector<std::string> firstProfilesCols;
    std::vector<std::string> firstSharedCols;
    {
        auto r1 = core::Database::open(path);
        DOCTEST_REQUIRE(r1.isOk());
        DOCTEST_REQUIRE(r1.value().migrate().isOk());
        auto& db = r1.value().db();

        auto q = db.prepare("SELECT type, name, sql FROM sqlite_master "
                            "ORDER BY type, name");
        DOCTEST_REQUIRE(q.isOk());
        auto rows = q.value().fetchAll();
        DOCTEST_REQUIRE(rows.isOk());
        for (const auto& row : rows.value()) {
            firstObjects.push_back(row.getString("type").valueOr("?") + "/" +
                                   row.getString("name").valueOr("?") + "/" +
                                   row.getString("sql").valueOr("?"));
        }
        firstProfilesCols = columnDefs(db, "claude_profiles");
        firstSharedCols = columnDefs(db, "claude_shared_configs");
    } // 句柄关闭（Windows 文件锁）

    // 关掉再打开：同一连接连跑两次 applySchema，都不报错
    {
        auto r2 = core::Database::open(path);
        DOCTEST_REQUIRE(r2.isOk());
        DOCTEST_REQUIRE(r2.value().migrate().isOk());
        DOCTEST_REQUIRE(r2.value().migrate().isOk());
        auto& db = r2.value().db();

        // sqlite_master 逐条与首次完全相同
        auto q = db.prepare("SELECT type, name, sql FROM sqlite_master "
                            "ORDER BY type, name");
        DOCTEST_REQUIRE(q.isOk());
        auto rows = q.value().fetchAll();
        DOCTEST_REQUIRE(rows.isOk());
        DOCTEST_REQUIRE_EQ(rows.value().size(), firstObjects.size());
        for (size_t i = 0; i < rows.value().size(); ++i) {
            const auto& row = rows.value()[i];
            std::string got = row.getString("type").valueOr("?") + "/" +
                              row.getString("name").valueOr("?") + "/" +
                              row.getString("sql").valueOr("?");
            DOCTEST_CHECK_EQ(got, firstObjects[i]);
        }

        // 列定义不变
        DOCTEST_CHECK_EQ(columnDefs(db, "claude_profiles"), firstProfilesCols);
        DOCTEST_CHECK_EQ(columnDefs(db, "claude_shared_configs"), firstSharedCols);

        // 种子行不被重复插入：这是本脚本唯一的非 DDL 幂等点
        auto prep = db.prepare("SELECT COUNT(*) FROM todo_columns");
        DOCTEST_REQUIRE(prep.isOk());
        auto rows2 = prep.value().fetchAll();
        DOCTEST_REQUIRE(rows2.isOk());
        DOCTEST_CHECK_EQ(std::get<int64_t>(rows2.value()[0].values[0]), 4);
    } // Database 在此析构（关闭 sqlite 句柄）

    std::filesystem::remove(path, ec);
    return true;
})

// 幂等的另一半：用户自己加过列 / 改过行之后，applySchema 不得踩掉他的数据
REGISTER_TEST(schema_is_idempotent_preserves_user_data,
              "schema: 重复 applySchema 不覆盖用户已改的行 / 已加的列", {
    auto path = test_support::tempDbPath(kDbPrefix, "preserve");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    {
        auto r = core::Database::open(path);
        DOCTEST_REQUIRE(r.isOk());
        DOCTEST_REQUIRE(r.value().migrate().isOk());
        auto& db = r.value().db();

        // 用户重命名了种子列、改了颜色、删了一列、加了一列
        DOCTEST_REQUIRE(db.exec(
            "UPDATE todo_columns SET title = '我的待办', color = 'x' "
            "WHERE position = 0").isOk());
        DOCTEST_REQUIRE(db.exec("DELETE FROM todo_columns WHERE position = 3").isOk());
        DOCTEST_REQUIRE(db.exec(
            "INSERT INTO todo_columns (title, position, created_at) "
            "VALUES ('临时', 9, 0)").isOk());

        // 再跑一次 applySchema：种子守卫见「表非空」，一行都不该动
        DOCTEST_REQUIRE(db.applySchema(core::schema::kSql).isOk());

        auto prep = db.prepare(
            "SELECT title, color FROM todo_columns ORDER BY position");
        DOCTEST_REQUIRE(prep.isOk());
        auto rows = prep.value().fetchAll();
        DOCTEST_REQUIRE(rows.isOk());
        DOCTEST_REQUIRE_EQ(rows.value().size(), 4u);   // 4 种子 -1 删 +1 自建
        DOCTEST_CHECK_EQ(rows.value()[0].getString("title").valueOr("?"),
                         std::string("我的待办"));
        DOCTEST_CHECK_EQ(rows.value()[0].getString("color").valueOr("?"),
                         std::string("x"));
        DOCTEST_CHECK_EQ(rows.value()[3].getString("title").valueOr("?"),
                         std::string("临时"));
    }

    std::filesystem::remove(path, ec);
    return true;
})

REGISTER_TEST(schema_migrate_agent_columns_from_old_schema,
    "schema: migrate_agent_columns 老库缺列补全（context_note/auto_note 补齐，旧数据行保留）", {
    auto path = test_support::tempDbPath(kDbPrefix, "mig_old");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    // 阶段 1：造一个"老库"——agent_tasks 只有旧列（缺 context_note / auto_note）
    {
        auto r = core::Database::open(path);
        DOCTEST_REQUIRE(r.isOk());
        auto& db = r.value().db();

        // 确认新库已有 context_note / auto_note
        auto colsBefore = columnDefs(db, "agent_tasks");
        bool hasCtxNote = false, hasAutoNote = false;
        for (const auto& c : colsBefore) {
            if (c.find("context_note:") == 0) hasCtxNote = true;
            if (c.find("auto_note:") == 0) hasAutoNote = true;
        }
        DOCTEST_CHECK(hasCtxNote);
        DOCTEST_CHECK(hasAutoNote);

        // 重建 agent_tasks 为旧结构（无 context_note, auto_note）
        DOCTEST_REQUIRE(db.exec("DROP TABLE agent_tasks").isOk());
        DOCTEST_REQUIRE(db.exec(
            "CREATE TABLE agent_tasks ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "name TEXT NOT NULL,"
            "goal TEXT NOT NULL,"
            "trigger_type TEXT NOT NULL,"
            "trigger_config TEXT NOT NULL DEFAULT '',"
            "model_slot TEXT NOT NULL DEFAULT 'default',"
            "max_iterations INTEGER NOT NULL DEFAULT 10,"
            "allow_destructive INTEGER NOT NULL DEFAULT 0,"
            "enabled INTEGER NOT NULL DEFAULT 1,"
            "created_at INTEGER NOT NULL,"
            "updated_at INTEGER NOT NULL)").isOk());

        // 旧表确认缺列
        auto colsMid = columnDefs(db, "agent_tasks");
        for (const auto& c : colsMid) {
            DOCTEST_CHECK(c.find("context_note:") != 0);
            DOCTEST_CHECK(c.find("auto_note:") != 0);
        }

        // 塞一条旧结构数据（用旧列集合）
        DOCTEST_REQUIRE(db.exec(
            "INSERT INTO agent_tasks (name, goal, trigger_type, created_at, updated_at) "
            "VALUES ('old-task', 'old-goal', 'manual', 100, 200)").isOk());
    } // 关闭库，让下次 open 走完整 migrate 路径

    // 阶段 2：重新 open（走 applySchema + addMissingAgentColumns）
    {
        auto r = core::Database::open(path);
        DOCTEST_REQUIRE(r.isOk());
        auto& db = r.value().db();

        // 列已补齐
        auto colsAfter = columnDefs(db, "agent_tasks");
        bool foundCtx = false, foundAuto = false;
        for (const auto& c : colsAfter) {
            if (c.find("context_note:TEXT:1:''") == 0) foundCtx = true;
            if (c.find("auto_note:INTEGER:1:1") == 0) foundAuto = true;
        }
        DOCTEST_CHECK(foundCtx);
        DOCTEST_CHECK(foundAuto);

        // 旧数据行保留
        auto q = db.prepare("SELECT name, goal, context_note, auto_note FROM agent_tasks WHERE name='old-task'");
        DOCTEST_REQUIRE(q.isOk());
        auto rows = q.value().fetchAll();
        DOCTEST_REQUIRE(rows.isOk());
        DOCTEST_REQUIRE_EQ(rows.value().size(), 1u);
        const auto& row = rows.value()[0];
        DOCTEST_CHECK_EQ(row.getString("name").valueOr("?"), std::string("old-task"));
        DOCTEST_CHECK_EQ(row.getString("goal").valueOr("?"), std::string("old-goal"));
        // 新列取默认值：空串 / 1
        DOCTEST_CHECK_EQ(row.getInt64("auto_note").valueOr(-1), 1);
        DOCTEST_CHECK_EQ(row.getString("context_note").valueOr("?"), std::string(""));
    }

    std::filesystem::remove(path, ec);
    return true;
})

// 回归：agent_tasks 老库（13 列、无 system_prompt）升级。契约——migrate 后列存在
// （TEXT NOT NULL DEFAULT ''）、旧行原值守恒、agent_store 读写列序的 SELECT prepare 成功
// （该 SELECT 即 Scheduler 每分钟轮询的查询形态）。迁移清单漏掉 system_prompt 时，
// CREATE IF NOT EXISTS 对已存在表跳过，本用例在「列缺失」与「prepare fail」两处红。
REGISTER_TEST(schema_migrate_agent_tasks_pre_system_prompt,
    "schema: agent_tasks 老库升级补 system_prompt（旧行守恒 + Scheduler 列序查询恢复）", {
    auto path = test_support::tempDbPath(kDbPrefix, "mig_sysprompt");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    // 阶段 1：open（先建全当前 schema），再把 agent_tasks 退回无 system_prompt 的老形态
    {
        auto r = core::Database::open(path);
        DOCTEST_REQUIRE(r.isOk());
        auto& db = r.value().db();

        DOCTEST_REQUIRE(db.exec("DROP TABLE agent_tasks").isOk());
        DOCTEST_REQUIRE(db.exec(
            "CREATE TABLE agent_tasks ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "name TEXT NOT NULL,"
            "goal TEXT NOT NULL,"
            "trigger_type TEXT NOT NULL,"
            "trigger_config TEXT NOT NULL DEFAULT '',"
            "model_slot TEXT NOT NULL DEFAULT 'default',"
            "max_iterations INTEGER NOT NULL DEFAULT 10,"
            "allow_destructive INTEGER NOT NULL DEFAULT 0,"
            "enabled INTEGER NOT NULL DEFAULT 1,"
            "context_note TEXT NOT NULL DEFAULT '',"
            "auto_note INTEGER NOT NULL DEFAULT 1,"
            "created_at INTEGER NOT NULL,"
            "updated_at INTEGER NOT NULL)").isOk());
        for (const auto& c : columnDefs(db, "agent_tasks")) {
            DOCTEST_CHECK(c.find("system_prompt:") != 0);
        }
        DOCTEST_REQUIRE(db.exec(
            "INSERT INTO agent_tasks (name, goal, trigger_type, trigger_config, "
            "model_slot, max_iterations, allow_destructive, enabled, context_note, "
            "auto_note, created_at, updated_at) "
            "VALUES ('legacy-task', 'legacy-goal', 'schedule', '{\"cron\":\"*\"}', "
            "'slot-a', 5, 1, 0, 'note', 0, 111, 222)").isOk());
    } // 关闭库，下次 open / migrate 走完整迁移路径

    // 阶段 2：迁移 → 补列、旧行守恒、Scheduler 列序查询恢复
    {
        auto r = core::Database::open(path);
        DOCTEST_REQUIRE(r.isOk());
        DOCTEST_REQUIRE(r.value().migrate().isOk());
        auto& db = r.value().db();

        bool found = false;
        for (const auto& c : columnDefs(db, "agent_tasks")) {
            if (c == "system_prompt:TEXT:1:''") found = true;
        }
        DOCTEST_CHECK_MESSAGE(found,
                              "迁移后 agent_tasks 应有 system_prompt:TEXT:1:''");

        // 列序与 agent_store 的 kTaskColumns 一致（id + 9 列）
        auto q = db.prepare(
            "SELECT id,name,goal,trigger_type,trigger_config,model_slot,"
            "max_iterations,allow_destructive,enabled,system_prompt FROM agent_tasks");
        DOCTEST_REQUIRE_MESSAGE(q.isOk(),
                                "agent_store 列序 SELECT prepare 应成功");
        auto rows = q.value().fetchAll();
        DOCTEST_REQUIRE(rows.isOk());
        DOCTEST_REQUIRE_EQ(rows.value().size(), 1u);
        const auto& row = rows.value()[0];
        DOCTEST_CHECK_EQ(row.getInt64("id").valueOr(-1), 1);
        DOCTEST_CHECK_EQ(row.getString("name").valueOr("?"), std::string("legacy-task"));
        DOCTEST_CHECK_EQ(row.getString("goal").valueOr("?"), std::string("legacy-goal"));
        DOCTEST_CHECK_EQ(row.getString("trigger_type").valueOr("?"),
                         std::string("schedule"));
        DOCTEST_CHECK_EQ(row.getString("trigger_config").valueOr("?"),
                         std::string("{\"cron\":\"*\"}"));
        DOCTEST_CHECK_EQ(row.getString("model_slot").valueOr("?"), std::string("slot-a"));
        DOCTEST_CHECK_EQ(row.getInt64("max_iterations").valueOr(-1), 5);
        DOCTEST_CHECK_EQ(row.getInt64("allow_destructive").valueOr(-1), 1);
        DOCTEST_CHECK_EQ(row.getInt64("enabled").valueOr(-1), 0);
        // 新列对旧行取建表 DEFAULT ''
        DOCTEST_CHECK_EQ(row.getString("system_prompt").valueOr("?"), std::string(""));

        // 行内其余列同样守恒（不在 kTaskColumns 里的部分）
        auto q2 = db.prepare(
            "SELECT context_note, auto_note, created_at, updated_at FROM agent_tasks");
        DOCTEST_REQUIRE(q2.isOk());
        auto rows2 = q2.value().fetchAll();
        DOCTEST_REQUIRE(rows2.isOk());
        DOCTEST_REQUIRE_EQ(rows2.value().size(), 1u);
        DOCTEST_CHECK_EQ(rows2.value()[0].getString("context_note").valueOr("?"),
                         std::string("note"));
        DOCTEST_CHECK_EQ(rows2.value()[0].getInt64("auto_note").valueOr(-1), 0);
        DOCTEST_CHECK_EQ(rows2.value()[0].getInt64("created_at").valueOr(-1), 111);
        DOCTEST_CHECK_EQ(rows2.value()[0].getInt64("updated_at").valueOr(-1), 222);

        // 幂等：二次 migrate 列定义零变化、行数不变
        auto colsBefore = columnDefs(db, "agent_tasks");
        DOCTEST_REQUIRE(r.value().migrate().isOk());
        DOCTEST_CHECK_EQ(columnDefs(db, "agent_tasks"), colsBefore);
        auto cnt = db.prepare("SELECT COUNT(*) AS n FROM agent_tasks");
        DOCTEST_REQUIRE(cnt.isOk());
        auto cntRows = cnt.value().fetchAll();
        DOCTEST_REQUIRE(cntRows.isOk());
        DOCTEST_CHECK_EQ(std::get<int64_t>(cntRows.value()[0].values[0]), 1);
    }

    std::filesystem::remove(path, ec);
    return true;
})

REGISTER_TEST(schema_migrate_agent_columns_idempotent,
    "schema: migrate_agent_columns 幂等——新库连跑两次不报错、列定义不变", {
    auto path = test_support::tempDbPath(kDbPrefix, "mig_idem");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    // 第一次：记录列集
    std::vector<std::string> firstTasks, firstRuns, firstMsgs;
    {
        auto r = core::Database::open(path);
        DOCTEST_REQUIRE(r.isOk());
        firstTasks = columnDefs(r.value().db(), "agent_tasks");
        firstRuns  = columnDefs(r.value().db(), "agent_runs");
        firstMsgs  = columnDefs(r.value().db(), "agent_messages");
    }

    // 第二次：仅 migrate（不重新 open，同连接第二次调）
    {
        auto r = core::Database::open(path);
        DOCTEST_REQUIRE(r.isOk());
        // 第二次 migrate 幂等、不报错
        DOCTEST_REQUIRE(r.value().migrate().isOk());
        DOCTEST_REQUIRE(r.value().migrate().isOk()); // 第三次确认

        auto& db = r.value().db();
        auto t2 = columnDefs(db, "agent_tasks");
        auto r2 = columnDefs(db, "agent_runs");
        auto m2 = columnDefs(db, "agent_messages");

        // 列集与首次完全一致（幂等不增不减）
        DOCTEST_CHECK_EQ(t2, firstTasks);
        DOCTEST_CHECK_EQ(r2, firstRuns);
        DOCTEST_CHECK_EQ(m2, firstMsgs);
    }

    std::filesystem::remove(path, ec);
    return true;
})

REGISTER_TEST(schema_drop_removed_quota_tables,
    "schema: migrate() 幂等清理 profile_quota_config / profile_quota_result（UAT3 M3）", {
    // 回归：老库遗留两表，migrate 的 dropRemovedTables 段必须清掉它们；误删 dropRemoved
    // 调用或改错表名会让本用例红
    auto path = test_support::tempDbPath(kDbPrefix, "quota_drop");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    {
        auto r = core::Database::open(path);
        DOCTEST_REQUIRE(r.isOk());
        auto& db = r.value().db();

        // open 路径已跑过一次 migrate；手工重建两表模拟「老库残留」
        DOCTEST_REQUIRE(db.exec(
            "CREATE TABLE IF NOT EXISTS profile_quota_config (key TEXT PRIMARY KEY, value_json TEXT, updated_at INTEGER)").isOk());
        DOCTEST_REQUIRE(db.exec(
            "CREATE TABLE IF NOT EXISTS profile_quota_result (key TEXT PRIMARY KEY, value_json TEXT, updated_at INTEGER)").isOk());

        // 确认两表现在存在
        auto before = listObjects(db, "table");
        DOCTEST_CHECK(before.count("profile_quota_config") == 1);
        DOCTEST_CHECK(before.count("profile_quota_result") == 1);

        // 二次 migrate：dropRemovedTables 段应清掉两表
        DOCTEST_REQUIRE(r.value().migrate().isOk());

        auto after = listObjects(db, "table");
        DOCTEST_CHECK_MESSAGE(after.count("profile_quota_config") == 0,
                              "profile_quota_config 应在 migrate 后被 DROP");
        DOCTEST_CHECK_MESSAGE(after.count("profile_quota_result") == 0,
                              "profile_quota_result 应在 migrate 后被 DROP");
    }

    std::filesystem::remove(path, ec);
    return true;
})

// 票 11：迁移定义白名单。非法表/列枚举值（越界强转）必须在查表处显式报错——
// 到不了 SQL 拼接；白名单内每个枚举值都解析得出合法标识符与列定义，且标识符
// 只含 [a-z0-9_]（白名单返回值就是拼接接触到的全部「动态」内容）。
REGISTER_TEST(schema_migration_whitelist_rejects_invalid,
              "schema: 迁移白名单拒绝非法表名/列名（越界枚举显式报错，不进 SQL）", {
    using core::schema::MigrationColumn;
    using core::schema::MigrationTable;

    // 非法表枚举：查表即 fail，错误信息带非法值
    auto badTable = core::schema::migrationTableName(static_cast<MigrationTable>(99));
    DOCTEST_CHECK(badTable.isFail());
    DOCTEST_CHECK_MESSAGE(badTable.errorMessage().find("99") != std::string::npos,
                          "错误信息应带非法枚举值: " + badTable.errorMessage());

    // 非法列枚举：列名/列定义两个取词口都得拒
    const auto badColumn = static_cast<MigrationColumn>(999);
    DOCTEST_CHECK(core::schema::migrationColumnName(badColumn).isFail());
    DOCTEST_CHECK(core::schema::migrationColumnDdl(badColumn).isFail());

    // 白名单覆盖完备：0..kCount-1 每个枚举值都能解析（构建期另有 static_assert 双保险）
    auto isAsciiIdentifier = [](const char* s) {
        if (s == nullptr || *s == '\0') return false;
        for (const char* p = s; *p != '\0'; ++p) {
            const char c = *p;
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
                return false;
            }
        }
        return true;
    };
    for (int i = 0; i < static_cast<int>(MigrationTable::kCount); ++i) {
        auto name = core::schema::migrationTableName(static_cast<MigrationTable>(i));
        DOCTEST_CHECK(name.isOk());
        DOCTEST_CHECK(isAsciiIdentifier(name.valueOr("")));
    }
    for (int i = 0; i < static_cast<int>(MigrationColumn::kCount); ++i) {
        const auto column = static_cast<MigrationColumn>(i);
        auto name = core::schema::migrationColumnName(column);
        auto ddl = core::schema::migrationColumnDdl(column);
        DOCTEST_CHECK(name.isOk());
        DOCTEST_CHECK(ddl.isOk());
        DOCTEST_CHECK(isAsciiIdentifier(name.valueOr("")));
    }

    // 端到端：白名单路径产出的补列迁移在真库上照常工作（三表列齐全）
    auto path = test_support::tempDbPath(kDbPrefix, "whitelist");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    {
        auto r = core::Database::open(path);
        DOCTEST_REQUIRE(r.isOk());
        DOCTEST_REQUIRE(r.value().migrate().isOk());
        DOCTEST_CHECK_EQ(columnDefs(r.value().db(), "agent_tasks").size(), 14u);
        DOCTEST_CHECK_EQ(columnDefs(r.value().db(), "agent_runs").size(), 9u);
        DOCTEST_CHECK_EQ(columnDefs(r.value().db(), "agent_messages").size(), 7u);
    } // Database 在此析构（Windows 不允许删除打开中的文件）

    std::filesystem::remove(path, ec);
    return true;
})
