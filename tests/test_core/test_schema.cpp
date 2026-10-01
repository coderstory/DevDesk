// tests/test_core/test_schema.cpp
// 测试 Database::migrate() 落下的**当前** schema 形态（无版本化迁移）
//
// 形态（2026-09-30 起）：src/core/schema.h 一份幂等脚本，applySchema 一个事务
// 跑完。没有任何版本号 / schema_version 表 / 旧库升级路径——本文件因此只断言
// 「最终形态」与「幂等」，不再有 v1→v11 的升级用例。
#include "doctest.h"
#include "test_support.h"
#include "core/database.h"
#include "core/result.h"
#include "core/schema.h"
#include "core/sqlite_db.h"

#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace {

// 给测试生成一个临时 db 路径，不污染应用数据库
// —— 共享工厂 test_support::tempDbPath（TIN-06b）
constexpr const char* kDbPrefix = "devdesk_test_schema_";

// 读 sqlite_master，返回指定 type 的对象名集合
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

// 读一列的完整定义序列（name / type / notnull / default），顺序 = 建表顺序。
// default 为 NULL 时返回 "<none>"，与 sqlite_master 里 dflt_value=NULL 对齐。
// notnull 是 PRAGMA table_info 的**值**（0/1 整数），不是「有没有这列」——
// 注意 INTEGER PRIMARY KEY 也报 notnull=0（SQLite 的历史行为，照抄实际值）。
// 这是「逐表逐列」断言的基础：漏一列 / 改错默认值 / 类型漂移都会在这里露出来。
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

// ============ 表与索引的存在性 ============

REGISTER_TEST(schema_creates_all_tables,
              "schema: applySchema 建出全部 11 张应用表", {
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
        };
        for (const auto& t : expected) {
            bool found = tables.count(t) > 0;
            DOCTEST_CHECK(found);
            if (!found) {
                std::cout << "  [DEBUG] missing table: " << t << std::endl;
            }
        }

        // 7 个索引（含 1 个部分唯一索引 + 1 个带 WHERE 的到期索引 + agent 2 个）
        auto indexes = listObjects(r.value().db(), "index");
        std::set<std::string> expectedIndexes{
            "idx_tasks_column",
            "idx_tasks_due",
            "idx_claude_active",
            "idx_call_logs_started",
            "idx_call_logs_profile",
            "idx_agent_runs_task",
            "idx_agent_messages_run",
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

// 版本化机制已彻底删除：库里不得再有 schema_version 表。
// 口径是「单份当前 schema、无历史包袱」——留着这张表就等于留了个版本号残骸。
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

// ============ 逐表逐列：最终形态 ============
// 每张表的 name:type:notnull:default 序列逐字比对，顺序敏感。
// 这是本次重构的核心断言——旧库升级类断言已删，形态断言取而代之。
// 变异验证：从 schema.h 删掉任一列 / 改任一默认值 → 对应用例立刻红。

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

        // FTS5 虚表：path 不入索引（UNINDEXED），title/content 入索引。
        // 虚表的 type 一律是空串（FTS5 自己不声明列类型，内容恒为文本）——照抄
        // PRAGMA 的实际返回值，不臆断成 TEXT。
        std::vector<std::string> expectFts{
            "path::0:<none>",
            "title::0:<none>",
            "content::0:<none>",
        };
        DOCTEST_CHECK_EQ(columnDefs(db, "markdown_fts"), expectFts);

        // 虚表真能用：写入后可 MATCH 命中。查询词用拉丁词元——unicode61 把连续 CJK
        // 当成单个 token，CJK 子串命中靠的是 service 层的 LIKE 兜底
        // （markdown_service.cpp 路径 B），不是 FTS MATCH。
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

        // kind 可空、无默认值（NULL = 普通列）
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
            "keychain_account:TEXT:1:<none>",
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

        // 调用日志 15 列：前 10 列来自初始建表，末尾 5 列是 usage（全部可空 = 未采集，
        // 与真实的 0 区分开）
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

        // 共享片段表：5 列，无 extra_env（旧 env 域列已随重构删除，不再建）
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
              "schema: agent 三表列定义符合最终形态（11/9/7 列，双 FK CASCADE）", {
    auto path = test_support::tempDbPath(kDbPrefix, "cols_agent");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    {
        auto r = core::Database::open(path);
        DOCTEST_REQUIRE(r.isOk());
        DOCTEST_REQUIRE(r.value().migrate().isOk());
        auto& db = r.value().db();

        // agent_tasks 11 列：默认值钉死（max_iterations=10 / allow_destructive=0 /
        // enabled=1 / model_slot='default'）——AgentStore 落库契约的列形态基础
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
            "created_at:INTEGER:1:<none>",
            "updated_at:INTEGER:1:<none>",
        };
        DOCTEST_CHECK_EQ(columnDefs(db, "agent_tasks"), expectTasks);

        // agent_runs 9 列：ended_at/tokens/error 全可空（运行中 / 未采集 / 无错误
        // 与真实值区分）
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

        // agent_messages 7 列：tool_call_id 可空（assistant 文本块无调用 id）
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

        // FK CASCADE 生效：删 task 清 runs、删 run 清 messages（审计链随宿主走）
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
        DOCTEST_REQUIRE(db.prepare("DELETE FROM agent_tasks WHERE id=1")
                            .step()
                            .isOk());
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

// ============ 种子数据（自带 kind，不再「插完再按标题回填」）============

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
            {"进行中", "warm-amber",  ""},       // 普通列：kind 为 NULL
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

// ============ 幂等：连跑多次不报错、schema 不变、种子不重复 ============

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

        // 种子行不被重复插入（这是本脚本唯一的非 DDL 幂等点）
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
