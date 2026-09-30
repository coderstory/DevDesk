# DevDesk 智能体子系统 M1（核心链路）实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 手动触发端到端跑通一次 agent run：任务配置 → 经本地代理调 LLM → tool_use 循环调内置工具 → 三表落库可审计 → 最小 UI 展示。

**Architecture:** core 新增 `ITool`/`ToolRegistry`/`LlmEndpoint` 抽象（依赖反转）；`module_agent` 内 AgentLoop（执行器）/ LlmClient（经本地代理）/ AgentStore（落库）/ AgentModule（页面）四件独立文件；todo/markdown 模块各加一个工具集文件注册 ITool。数据流见 spec §1。

**Tech Stack:** C++20 / CMake / Ninja，yyjson 薄封装（platform::json）、libcurl（platform::http）、SQLite（core::data::SqliteDb）、doctest。零新第三方依赖。

**Spec:** `docs/superpowers/specs/2026-09-30-agent-design.md`（R4）。本计划实现其 §14 M1 行；所有 D-1~D-9 决策、§5 状态机、§6 表结构、§10 安全控制以 spec 为准，冲突时以 spec 为准并回报。

## Global Constraints（每个任务隐含遵守）

- **不抛 C++ 异常**：错误一律 `core::Result<T>`（`src/core/result.h`：`Result::ok(v)` / `Result::fail(ErrorCode, msg)` / `isOk()` / `value()` 仅用于不变量保证成功处）。app 以 `-fno-exceptions` 编译。
- **零新依赖**（D-5）：只用 platform::http + platform::json + SqliteDb + EUI-NEO。禁 CPR/nlohmann/MCP 子进程。
- **模块隔离**：`module_agent` 只 include core 头（tool.h / llm_endpoint.h / claude_proxy_service.h / sqlite_db.h）+ platform 头；禁 include 任何 `module_*` 头。各 `*Tools.cpp` 只 include 自己模块 + `core/tool.h`。改完跑 `bash scripts/check-module-isolation.sh`。
- **模块化**：每工具集独立文件；module_agent 内 AgentLoop/LlmClient/AgentStore/AgentModule 各自独立文件。禁大杂烩文件。
- **TDD**：每步先写失败测试再实现；测试名/断言照抄本计划，不许为绿灯弱化断言。
- **构建命令**（worktree 内）：`cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3 -DEUI_NEO_DIR=/Users/coderstory/dev/gui/EUI-NEO` 然后 `cmake --build build`。全量测试 `./build/tests/devdesk_tests`，看输出 `Total: / Passed: / Failed:`（不写死总数）。
- **提交纪律**：原子提交、禁 `git add -A`、提交前 `git status --porcelain` 确认无意外文件（尤其仓库根的 `NUL` 垃圾文件——出现即删）。
- **新增测试文件**必须加进 `tests/CMakeLists.txt` 的 `add_executable(devdesk_tests ...)` 列表（tests/ 已在 include path，无需其他改动）。

## Review Focus（spec 未被测试覆盖、最易咬人的五类输入）

1. **模块运行中被拔工具**：`find` 与 `execute` 之间 unregister → 期望 shared_ptr keep-alive 不悬垂。钉子：Task 1「find 后 unregister，持有的 shared_ptr 仍可 execute」用例。
2. **超长 tool_result**：`md.read` 大笔记多轮迭代烧爆上下文 → 期望单 result 32KB 截断 + `[truncated]` 尾标。钉子：Task 5 截断用例。
3. **代理未启 / 模型槽位解析失败**：期望 run `failed` + 中文 error、**不发 HTTP 包**。钉子：Task 6 预检用例。
4. **破坏性工具默认未授权**：`allow_destructive=0` → 期望 run 直接 `failed`、工具**未被调用**。钉子：Task 5 用例。
5. **悬挂 run**：应用崩溃后 agent_runs 留 `running` 脏状态 → 期望启动/进页清扫为 failed。钉子：Task 3 sweep 用例。

---

### Task 0: worktree 基线实证

**Files:** 无新增（只构建+测试）。

- [ ] **Step 1: Configure + 全量构建**

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3 \
  -DEUI_NEO_DIR=/Users/coderstory/dev/gui/EUI-NEO
cmake --build build
```

Expected: 退出码 0。（ccache 命中，预计数分钟。）

- [ ] **Step 2: 全量测试记录基线**

```bash
./build/tests/devdesk_tests 2>&1 | grep -E "Total:|Passed:|Failed:|Skipped:"
```

Expected: Failed: 0。把 Total/Passed/Skipped 记进 commit message。
（本分支起点 dev `e379950` 与已测同码，此步是实证非信任。）

- [ ] **Step 3: 无代码变更，无需提交**（基线数字写进 Task 1 的提交信息尾部）

### Task 1: core/tool.h + ToolRegistry

**Files:**
- Create: `src/core/tool.h`、`src/core/tool.cpp`
- Test: `tests/test_core/test_tool_registry.cpp`
- Modify: `tests/CMakeLists.txt`（加一行 `test_core/test_tool_registry.cpp`）、`src/core/CMakeLists.txt`（源列表加 `tool.cpp`，照抄既有 core 源文件行样式）

**Interfaces:**
- Produces（后续所有任务依赖，签名逐字）:
  - `namespace core`：`struct ToolArgs { std::string json; };`、`struct ToolResult { bool isError = false; std::string contentJson; };`
  - `class ITool { virtual std::string name() const = 0; virtual std::string schemaJson() const = 0; virtual Result<ToolResult> execute(const ToolArgs&) = 0; }`（虚析构）
  - `class ToolRegistry { static ToolRegistry& instance(); Result<void> registerTool(std::shared_ptr<ITool>); void unregisterTool(const std::string&); Result<std::shared_ptr<ITool>> find(const std::string&) const; std::string allSchemasJson() const; }`

- [ ] **Step 1: 写失败测试** `tests/test_core/test_tool_registry.cpp`

```cpp
// tests/test_core/test_tool_registry.cpp
// ToolRegistry：注册/冲突/注销/find keep-alive/schema 拼装（spec §2.1）
#include "doctest.h"
#include "core/tool.h"

namespace {
class FakeTool : public core::ITool {
public:
    FakeTool(std::string n, std::string schema) : n_(std::move(n)), s_(std::move(schema)) {}
    std::string name() const override { return n_; }
    std::string schemaJson() const override { return s_; }
    core::Result<core::ToolResult> execute(const core::ToolArgs&) override {
        ++calls;
        return core::Result<core::ToolResult>::ok(core::ToolResult{false, "\"ok\""});
    }
    int calls = 0;
private:
    std::string n_, s_;
};
} // namespace

REGISTER_TEST(tool_registry_register_find, "tool: 注册后可 find，取到同一实例", {
    auto& reg = core::ToolRegistry::instance();
    auto t = std::make_shared<FakeTool>("t.a", R"({"name":"t.a"})");
    DOCTEST_REQUIRE(reg.registerTool(t).isOk());
    auto found = reg.find("t.a");
    DOCTEST_REQUIRE(found.isOk());
    DOCTEST_CHECK(found.value() == t);
    reg.unregisterTool("t.a");
    DOCTEST_CHECK(reg.find("t.a").isFail());
    return true;
})

REGISTER_TEST(tool_registry_duplicate_name_rejected, "tool: 同名注册冲突拒（ValidationError）", {
    auto& reg = core::ToolRegistry::instance();
    auto a = std::make_shared<FakeTool>("t.dup", "{}");
    auto b = std::make_shared<FakeTool>("t.dup", "{}");
    DOCTEST_REQUIRE(reg.registerTool(a).isOk());
    auto r = reg.registerTool(b);
    DOCTEST_CHECK(r.isFail());
    DOCTEST_CHECK(r.errorCode() == core::ErrorCode::ValidationError);
    reg.unregisterTool("t.dup");
    return true;
})

REGISTER_TEST(tool_registry_shared_ptr_keepalive, "tool: find 持有 shared_ptr 期间 unregister 不悬垂，仍可 execute", {
    auto& reg = core::ToolRegistry::instance();
    auto t = std::make_shared<FakeTool>("t.keep", "{}");
    DOCTEST_REQUIRE(reg.registerTool(t).isOk());
    auto held = reg.find("t.keep");
    DOCTEST_REQUIRE(held.isOk());
    reg.unregisterTool("t.keep");   // 模拟模块运行中被拔
    auto r = held.value()->execute(core::ToolArgs{"{}"});   // 仍安全
    DOCTEST_CHECK(r.isOk());
    DOCTEST_CHECK_EQ(t->calls, 1);
    return true;
})

REGISTER_TEST(tool_registry_all_schemas_json, "tool: allSchemasJson 拼数组，空表 = []", {
    auto& reg = core::ToolRegistry::instance();
    reg.unregisterTool("t.s1");
    reg.unregisterTool("t.s2");
    DOCTEST_CHECK_EQ(reg.allSchemasJson(), std::string("[]"));
    auto a = std::make_shared<FakeTool>("t.s1", R"({"name":"t.s1"})");
    auto b = std::make_shared<FakeTool>("t.s2", R"({"name":"t.s2"})");
    DOCTEST_REQUIRE(reg.registerTool(a).isOk());
    DOCTEST_REQUIRE(reg.registerTool(b).isOk());
    DOCTEST_CHECK_EQ(reg.allSchemasJson(),
                     std::string(R"([{"name":"t.s1"},{"name":"t.s2"}])"));
    reg.unregisterTool("t.s1");
    reg.unregisterTool("t.s2");
    return true;
})
```

- [ ] **Step 2: 跑测试确认失败**

```bash
cmake --build build 2>&1 | tail -3 && ./build/tests/devdesk_tests tool: 2>&1 | grep -E "Total:|Passed:|Failed:"
```
Expected: 编译失败（core/tool.h 不存在）。

- [ ] **Step 3: 实现** `src/core/tool.h`（spec §2.1 代码块逐字）+ `src/core/tool.cpp`：

```cpp
// src/core/tool.cpp
#include "core/tool.h"

namespace core {

ToolRegistry& ToolRegistry::instance() {
    static ToolRegistry g;
    return g;
}

Result<void> ToolRegistry::registerTool(std::shared_ptr<ITool> tool) {
    if (!tool || tool->name().empty()) {
        return Result<void>::fail(ErrorCode::ValidationError, "工具为空或 name 为空");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tools_.find(tool->name());
    if (it != tools_.end()) {
        if (it->second == tool) return Result<void>::ok();   // 重复注册同一指针 = no-op
        return Result<void>::fail(ErrorCode::ValidationError,
                                  "工具名冲突: " + tool->name());
    }
    tools_.emplace(tool->name(), std::move(tool));
    return Result<void>::ok();
}

void ToolRegistry::unregisterTool(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    tools_.erase(name);
}

Result<std::shared_ptr<ITool>> ToolRegistry::find(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tools_.find(name);
    if (it == tools_.end()) {
        return Result<std::shared_ptr<ITool>>::fail(
            ErrorCode::NotFound, "工具未找到: " + name + "（模块已禁用？）");
    }
    return Result<std::shared_ptr<ITool>>::ok(it->second);   // keep-alive
}

std::string ToolRegistry::allSchemasJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string out = "[";
    bool first = true;
    for (const auto& [name, tool] : tools_) {
        if (!first) out += ",";
        out += tool->schemaJson();
        first = false;
    }
    out += "]";
    return out;
}

} // namespace core
```

`tool.h` 中 `mutable std::mutex mutex_;`（spec 写"读写各一把锁"——单 mutex 已满足正确性，M1 不做读写锁优化，注释照 spec 语义写"实现时定粒度"）。

- [ ] **Step 4: 跑测试确认通过**（同 Step 2 命令）Expected: 4/4 Passed。
- [ ] **Step 5: Commit**

```bash
git add src/core/tool.h src/core/tool.cpp tests/test_core/test_tool_registry.cpp tests/CMakeLists.txt src/core/CMakeLists.txt
git commit -m "feat(agent): core ITool + ToolRegistry 依赖反转中介（spec §2.1）"
```

### Task 2: migration v12 三表 + spec §6 对齐

**Files:**
- Modify: `src/core/migrations.h`（新增 `kAgentTables` + `all()` 末尾追加）
- Test: `tests/test_core/test_migrations.cpp`（追加用例）

**Interfaces:**
- Produces: schema v12 的 `agent_tasks` / `agent_runs` / `agent_messages` 三表 + 2 索引（列定义 = spec §6 SQL 逐字）。

- [ ] **Step 1: 写失败测试**（追加到 `tests/test_core/test_migrations.cpp` 末尾，沿用该文件既有的 include 与 TestDb 用法）

```cpp
REGISTER_TEST(migrations_v12_agent_tables,
              "migrations: v12 建 agent_tasks/agent_runs/agent_messages 三表", {
    test_support::TestDb tdb("agentmig", "v12");
    auto& db = tdb.handle();
    DOCTEST_CHECK_EQ(db.schemaVersion(), 12);
    const char* const kTables[] = {"agent_tasks", "agent_runs", "agent_messages"};
    for (const char* t : kTables) {
        auto st = db.prepare("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name=?");
        DOCTEST_REQUIRE(st.isOk());
        DOCTEST_REQUIRE(st.value().bind(1, std::string(t)).isOk());
        DOCTEST_REQUIRE(st.value().step().isOk());
        const auto row = st.value().currentRow();
        DOCTEST_REQUIRE(row.isOk());
        DOCTEST_CHECK_EQ(row.value().getInt64("COUNT(*)").valueOr(0), 1);
    }
    // agent_tasks 关键列存在（allow_destructive / model_slot / trigger_type）
    auto q = db.prepare("SELECT allow_destructive, model_slot, trigger_config, enabled FROM agent_tasks");
    DOCTEST_CHECK(q.isOk());
    return true;
})
```

（若该文件既有用例的表名/断言风格不同，以「断言语义不变」为准改写适配；`getInt64("COUNT(*)")` 列名以 `SqlRow::columnNames` 实际值为准，必要时用 `values[0]`。）

- [ ] **Step 2: 跑测试确认失败**（v12 不存在，schemaVersion==11）

- [ ] **Step 3: 实现**——`migrations.h` 在 `kClaudeExtraSettingsJson` 后追加（spec §6 SQL 逐字，版本号 12）：

```cpp
// 智能体子系统三表（spec 2026-09-30-agent-design §6，R4：v11 已被
// kClaudeExtraSettingsJson 占用，顺延 v12）
inline const Migration kAgentTables = {
    .version = 12,
    .description = "agent subsystem tables: agent_tasks / agent_runs / agent_messages",
    .sql = R"(
        CREATE TABLE IF NOT EXISTS agent_tasks (
          id INTEGER PRIMARY KEY AUTOINCREMENT,
          name TEXT NOT NULL,
          goal TEXT NOT NULL,
          trigger_type TEXT NOT NULL,
          trigger_config TEXT NOT NULL DEFAULT '',
          model_slot TEXT NOT NULL DEFAULT 'default',
          max_iterations INTEGER NOT NULL DEFAULT 10,
          allow_destructive INTEGER NOT NULL DEFAULT 0,
          enabled INTEGER NOT NULL DEFAULT 1,
          created_at INTEGER NOT NULL,
          updated_at INTEGER NOT NULL
        );
        CREATE TABLE IF NOT EXISTS agent_runs (
          id INTEGER PRIMARY KEY AUTOINCREMENT,
          task_id INTEGER NOT NULL,
          status TEXT NOT NULL,
          started_at INTEGER NOT NULL,
          ended_at INTEGER,
          iterations INTEGER NOT NULL DEFAULT 0,
          input_tokens INTEGER,
          output_tokens INTEGER,
          error TEXT,
          FOREIGN KEY (task_id) REFERENCES agent_tasks(id) ON DELETE CASCADE
        );
        CREATE INDEX IF NOT EXISTS idx_agent_runs_task ON agent_runs(task_id, started_at DESC);
        CREATE TABLE IF NOT EXISTS agent_messages (
          id INTEGER PRIMARY KEY AUTOINCREMENT,
          run_id INTEGER NOT NULL,
          seq INTEGER NOT NULL,
          role TEXT NOT NULL,
          content_json TEXT NOT NULL,
          tool_call_id TEXT,
          created_at INTEGER NOT NULL,
          FOREIGN KEY (run_id) REFERENCES agent_runs(id) ON DELETE CASCADE
        );
        CREATE INDEX IF NOT EXISTS idx_agent_messages_run ON agent_messages(run_id, seq);
    )"
};
```

`all()` 列表末尾追加 `kAgentTables,`。

- [ ] **Step 4: 跑测试确认通过**（同时全量跑一遍防迁移链回归）Expected: Failed: 0。
- [ ] **Step 5: Commit** `git add src/core/migrations.h tests/test_core/test_migrations.cpp && git commit -m "feat(agent): migration v12 智能体三表（spec §6）"`

### Task 3: module_agent/AgentStore（落库层）

**Files:**
- Create: `src/module_agent/agent_store.h`、`src/module_agent/agent_store.cpp`、`src/module_agent/CMakeLists.txt`（镜像 `src/module_todo/CMakeLists.txt`：static lib 名 `devdesk_agent`、源文件列表换成本模块、链接同样的 core/platform 依赖）
- Modify: `src/CMakeLists.txt`（既有 `add_subdirectory(src/module_todo)` 旁加 `add_subdirectory(src/module_agent)`；app 链接列表加 `devdesk_agent`——先构建会发现 app 尚未引用它也无妨，Task 11 接线）
- Test: `tests/test_modules/test_agent_store.cpp` + `tests/CMakeLists.txt` 加一行

**Interfaces:**
- Consumes: `core::data::SqliteDb`（prepare/bind/step/fetchAll，见 Task 2 的表列名）
- Produces（AgentLoop/AgentModule 依赖，签名逐字）:

```cpp
// src/module_agent/agent_store.h
#pragma once
#include <optional>
#include <string>
#include <vector>
#include "core/sqlite_db.h"

namespace module::agent {

struct AgentTaskRow {
    int64_t id = 0;
    std::string name, goal, triggerType, triggerConfig, modelSlot;
    int maxIterations = 10;
    bool allowDestructive = false;
    bool enabled = true;
};
struct AgentRunRow {
    int64_t id = 0, taskId = 0;
    std::string status;                 // running/completed/failed/limited/cancelled
    int64_t startedAt = 0;
    std::optional<int64_t> endedAt;
    int iterations = 0;
    std::optional<int64_t> inputTokens, outputTokens;
    std::optional<std::string> error;
};
struct AgentMessageRow {
    int64_t seq = 0;
    std::string role, contentJson;
    std::optional<std::string> toolCallId;
};

class AgentStore {
public:
    explicit AgentStore(core::data::SqliteDb& db);
    Result<int64_t> createTask(const std::string& name, const std::string& goal,
                               const std::string& triggerType,
                               const std::string& modelSlot,
                               int maxIterations, bool allowDestructive);
    Result<std::vector<AgentTaskRow>> listTasks();
    Result<int64_t> startRun(int64_t taskId);                    // INSERT status='running'
    Result<void> finishRun(int64_t runId, const std::string& status,
                           int iterations,
                           std::optional<int64_t> inputTokens,
                           std::optional<int64_t> outputTokens,
                           const std::optional<std::string>& error);
    Result<void> appendMessage(int64_t runId, int64_t seq, const std::string& role,
                               const std::string& contentJson,
                               const std::optional<std::string>& toolCallId);
    Result<std::vector<AgentMessageRow>> listMessages(int64_t runId);   // ORDER BY seq
    Result<std::vector<AgentRunRow>> listRuns(int64_t taskId, int limit);
    Result<void> sweepStaleRuns();   // running → failed('应用异常退出（悬挂 run）')
private:
    core::data::SqliteDb& db_;
};
} // namespace module::agent
```

- [ ] **Step 1: 写失败测试** `tests/test_modules/test_agent_store.cpp`——用 `test_support::TestDb tdb("agentstore","t1")`（已 migrate 到 v12，含三表）：

```cpp
#include "doctest.h"
#include "test_support.h"
#include "module_agent/agent_store.h"

REGISTER_TEST(agent_store_task_run_message_roundtrip, "agent_store: 任务→run→消息 回写读出一致", {
    test_support::TestDb tdb("agentstore", "rt");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("整理逾期", "把逾期 todo 全部完成", "manual", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());
    auto runId = store.startRun(taskId.value());
    DOCTEST_REQUIRE(runId.isOk());
    DOCTEST_REQUIRE(store.appendMessage(runId.value(), 0, "user", R"([{"type":"text","text":"hi"}])", std::nullopt).isOk());
    DOCTEST_REQUIRE(store.appendMessage(runId.value(), 1, "assistant", R"([{"type":"tool_use","id":"t1"}])", std::nullopt).isOk());
    auto msgs = store.listMessages(runId.value());
    DOCTEST_REQUIRE(msgs.isOk());
    DOCTEST_CHECK_EQ(msgs.value().size(), 2u);
    DOCTEST_CHECK_EQ(msgs.value()[0].role, std::string("user"));
    DOCTEST_CHECK_EQ(msgs.value()[1].seq, 1);
    DOCTEST_REQUIRE(store.finishRun(runId.value(), "completed", 3, 120, 45, std::nullopt).isOk());
    auto runs = store.listRuns(taskId.value(), 10);
    DOCTEST_REQUIRE(runs.isOk());
    DOCTEST_CHECK_EQ(runs.value().size(), 1u);
    DOCTEST_CHECK_EQ(runs.value()[0].status, std::string("completed"));
    DOCTEST_CHECK_EQ(runs.value()[0].iterations, 3);
    DOCTEST_CHECK(runs.value()[0].endedAt.has_value());
    return true;
})

REGISTER_TEST(agent_store_sweep_stale_runs, "agent_store: sweep 把遗留 running 标 failed", {
    test_support::TestDb tdb("agentstore", "sw");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("t", "g", "manual", "default", 10, false);
    auto runId = store.startRun(taskId.value());   // 不 finishRun，模拟崩溃
    DOCTEST_REQUIRE(store.sweepStaleRuns().isOk());
    auto runs = store.listRuns(taskId.value(), 10);
    DOCTEST_REQUIRE(runs.isOk());
    DOCTEST_CHECK_EQ(runs.value()[0].status, std::string("failed"));
    DOCTEST_REQUIRE(runs.value()[0].error.has_value());
    DOCTEST_CHECK(runs.value()[0].error->find("悬挂") != std::string::npos);
    return true;
})
```

- [ ] **Step 2: 跑测试确认失败**（module_agent 不存在，编译失败）
- [ ] **Step 3: 实现** `agent_store.cpp`——统一模式（在 `db_.mutex()` 锁内 prepare→bind→step）：

完整示例（createTask；时间用 `std::time(nullptr)`）：

```cpp
Result<int64_t> AgentStore::createTask(const std::string& name, const std::string& goal,
                                       const std::string& triggerType,
                                       const std::string& modelSlot,
                                       int maxIterations, bool allowDestructive) {
    std::lock_guard<std::recursive_mutex> lock(db_.mutex());
    auto st = db_.prepare(
        "INSERT INTO agent_tasks(name, goal, trigger_type, trigger_config, model_slot,"
        " max_iterations, allow_destructive, enabled, created_at, updated_at)"
        " VALUES(?,?,?,?,?,?,?,1,?,?)");
    if (st.isFail()) return Result<int64_t>::fail(st.errorCode(), st.errorMessage());
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    auto& s = st.value();
    auto ok = s.bind(1, name) && s.bind(2, goal) && s.bind(3, triggerType) &&
              s.bind(4, std::string("")) && s.bind(5, modelSlot) &&
              s.bind(6, static_cast<int64_t>(maxIterations)) &&
              s.bind(7, static_cast<int64_t>(allowDestructive ? 1 : 0)) &&
              s.bind(8, now) && s.bind(9, now);
    if (!ok) return Result<int64_t>::fail(ErrorCode::DatabaseCorrupted, "bind 失败");
    auto step = s.step();
    if (step.isFail()) return Result<int64_t>::fail(step.errorCode(), step.errorMessage());
    return Result<int64_t>::ok(
        static_cast<int64_t>(sqlite3_last_insert_rowid(db_.handle())));
}
```

（`#include <sqlite3.h>` 于 cpp 顶部；`Result<void>` 的 bind 链用 `&&` 串联不可用——bind 返回 `Result<void>`，逐个 `if (r.isFail()) return fail(...)` 或写小 helper `auto chain = [](Result<void> r){...}`。**以可编译为准，禁止为省行数吞错。**）

其余方法按同模式，SQL 与列序固定如下（bind 顺序 = 问号顺序）：
| 方法 | SQL |
|---|---|
| startRun | `INSERT INTO agent_runs(task_id,status,started_at) VALUES(?,?,?)` status='running' |
| finishRun | `UPDATE agent_runs SET status=?,ended_at=?,iterations=?,input_tokens=?,output_tokens=?,error=? WHERE id=?`（optional 用 `bind(i, nullptr)` 表 NULL） |
| appendMessage | `INSERT INTO agent_messages(run_id,seq,role,content_json,tool_call_id,created_at) VALUES(?,?,?,?,?,?)` |
| listMessages | `SELECT seq,role,content_json,tool_call_id FROM agent_messages WHERE run_id=? ORDER BY seq` |
| listTasks | `SELECT id,name,goal,trigger_type,trigger_config,model_slot,max_iterations,allow_destructive,enabled FROM agent_tasks ORDER BY id` |
| listRuns | `SELECT id,task_id,status,started_at,ended_at,iterations,input_tokens,output_tokens,error FROM agent_runs WHERE task_id=? ORDER BY started_at DESC LIMIT ?` |
| sweepStaleRuns | `UPDATE agent_runs SET status='failed', ended_at=?, error='应用异常退出（悬挂 run）' WHERE status='running'` |

行取值：`row.getInt64("col")` / `row.getString("col")` / `row.isNull("col")` 映射 optional。

- [ ] **Step 4: 跑测试确认通过**（2/2）+ 全量无回归。
- [ ] **Step 5: Commit** `git commit -m "feat(agent): AgentStore 落库层（tasks/runs/messages CRUD + 悬挂清扫）"`

### Task 4: core/llm_endpoint.h + AgentLoop happy path

**Files:**
- Create: `src/core/llm_endpoint.h`（spec §2.2 代码块逐字：`LlmMessage{role, contentJson}`、`LlmResponse{stopReason, contentJson, inputTokens, outputTokens}`、`LlmEndpoint::send(messages, toolsJson, systemPrompt, model)`）
- Create: `src/module_agent/agent_loop.h`、`src/module_agent/agent_loop.cpp`
- Modify: `src/module_agent/CMakeLists.txt`（源列表加 agent_loop.cpp）
- Test: `tests/test_modules/test_agent_loop.cpp` + CMakeLists 加行

**Interfaces:**
- Consumes: Task 1 的 `core::ToolRegistry`/`ITool`、Task 3 的 `AgentStore`、`core::LlmEndpoint`。
- Produces（Task 6/12 依赖，签名逐字）:

```cpp
// src/module_agent/agent_loop.h
#pragma once
#include <atomic>
#include <string>
#include <vector>
#include "core/llm_endpoint.h"
#include "core/result.h"
#include "core/tool.h"

namespace module::agent {

struct AgentTask {
    int64_t id = 0;
    std::string name;
    std::string goal;
    std::string modelSlot;      // "default"/"sonnet"/...（profile 槽位键名）
    int maxIterations = 10;
    bool allowDestructive = false;
};

class AgentStore;

class AgentLoop {
public:
    AgentLoop(core::LlmEndpoint& llm, core::ToolRegistry& tools, AgentStore& store);
    // 同步跑一次（调用方在工作线程调）。落库 agent_runs/agent_messages。
    // 失败不抛：见 spec §5.2 状态语义（completed/failed/limited/cancelled）。
    Result<int64_t> run(const AgentTask& task, std::atomic<bool>& cancelToken);
private:
    core::LlmEndpoint& llm_;
    core::ToolRegistry& tools_;
    AgentStore& store_;
};
} // namespace module::agent
```

破坏性工具判定：ITool 增加默认方法 `virtual bool isDestructive() const { return false; }`（`tool.h`，Task 1 文件追加——M2 的 todo.delete 等再覆写 true）。测试里的 FakeTool 可设该标志。

- [ ] **Step 1: 写失败测试** `tests/test_modules/test_agent_loop.cpp`

```cpp
#include "doctest.h"
#include "test_support.h"
#include "module_agent/agent_loop.h"
#include "module_agent/agent_store.h"
#include "core/llm_endpoint.h"
#include "core/tool.h"
#include <atomic>

namespace {
using namespace module::agent;

// 脚本化 mock：按序吐响应，记录每次收到的 messages/tools/model
class MockLlm : public core::LlmEndpoint {
public:
    std::vector<core::LlmResponse> script;
    std::vector<std::vector<core::LlmMessage>> sentMessages;
    core::Result<core::LlmResponse> send(
        const std::vector<core::LlmMessage>& messages,
        const std::string& toolsJson, const std::string& systemPrompt,
        const std::string& model) override {
        (void)toolsJson; (void)systemPrompt; (void)model;
        sentMessages.push_back(messages);
        if (cursor >= script.size())
            return core::Result<core::LlmResponse>::fail(core::ErrorCode::Internal, "script 耗尽");
        return core::Result<core::LlmResponse>::ok(script[cursor++]);
    }
    size_t cursor = 0;
};

class FakeTool : public core::ITool {
public:
    FakeTool(std::string n, bool destructive = false) : n_(std::move(n)), destructive_(destructive) {}
    std::string name() const override { return n_; }
    std::string schemaJson() const override { return R"({"name":")" + n_ + R"("})"; }
    bool isDestructive() const override { return destructive_; }
    core::Result<core::ToolResult> execute(const core::ToolArgs& a) override {
        lastArgs = a.json; ++calls;
        return core::Result<core::ToolResult>::ok(core::ToolResult{false, R"("done")"});
    }
    std::string lastArgs; int calls = 0;
private:
    std::string n_; bool destructive_;
};

core::LlmResponse resp(const char* contentJson, const char* stop) {
    core::LlmResponse r;
    r.stopReason = stop;
    r.contentJson = contentJson;
    r.inputTokens = 10; r.outputTokens = 5;
    return r;
}

struct Fixture {
    test_support::TestDb tdb{test_support::TestDb("agentloop", "fx")};
    // 注意：TestDb 构造失败即抛（TIN-06b 口径），用例内直接用
    AgentStore store{tdb.handle()};
    MockLlm llm;
    core::ToolRegistry& tools = core::ToolRegistry::instance();
    AgentLoop loop{llm, tools, store};
};
} // namespace
```

（Fixture 的聚合初始化写法若与 doctest 宏冲突，改为普通 struct + 构造函数。）

Happy path 用例：

```cpp
REGISTER_TEST(agent_loop_happy_path_tool_then_end, "agent_loop: tool_use→工具执行→回喂→end_turn，消息累积与落库", {
    Fixture fx;
    FakeTool tool("t.hello");
    DOCTEST_REQUIRE(fx.tools.registerTool(std::shared_ptr<core::ITool>(&tool, [](core::ITool*){})).isOk());
    // 注册表借用栈对象：deleter 空、结尾注销，勿用 make_shared
    auto taskId = fx.store.createTask("t", "打招呼", "manual", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());
    AgentTask task; task.id = taskId.value(); task.goal = "打招呼"; task.modelSlot = "default";

    fx.llm.script = {
        resp(R"([{"type":"text","text":"调工具"},{"type":"tool_use","id":"tu1","name":"t.hello","input":{"x":1}}])", "tool_use"),
        resp(R"([{"type":"text","text":"完成"}])", "end_turn"),
    };
    std::atomic<bool> cancel{false};
    auto runId = fx.loop.run(task, cancel);
    DOCTEST_REQUIRE(runId.isOk());
    DOCTEST_CHECK_EQ(tool.calls, 1);
    DOCTEST_CHECK_EQ(tool.lastArgs, std::string(R"({"x":1})"));
    // 累积：首条 user goal → assistant1 → user(tool_result) → assistant2
    DOCTEST_REQUIRE(fx.llm.sentMessages.size() == 2);
    DOCTEST_CHECK_EQ(fx.llm.sentMessages[0].size(), 1u);           // goal
    DOCTEST_CHECK_EQ(fx.llm.sentMessages[1].size(), 3u);           // goal+asst1+tool_result
    DOCTEST_CHECK(fx.llm.sentMessages[1][2].contentJson.find("tool_result") != std::string::npos);
    DOCTEST_CHECK(fx.llm.sentMessages[1][2].contentJson.find("tu1") != std::string::npos);
    // 落库：4 条消息
    auto msgs = fx.store.listMessages(runId.value());
    DOCTEST_REQUIRE(msgs.isOk());
    DOCTEST_CHECK_EQ(msgs.value().size(), 4u);
    auto runs = fx.store.listRuns(taskId.value(), 10);
    DOCTEST_REQUIRE(runs.isOk());
    DOCTEST_CHECK_EQ(runs.value()[0].status, std::string("completed"));
    DOCTEST_CHECK_EQ(runs.value()[0].iterations, 2);
    DOCTEST_CHECK_EQ(runs.value()[0].inputTokens.value_or(0), 20);  // 10+10 累计
    DOCTEST_CHECK_EQ(runs.value()[0].outputTokens.value_or(0), 10);
    fx.tools.unregisterTool("t.hello");
    return true;
})
```

- [ ] **Step 2: 确认失败**（编译不过：llm_endpoint.h / agent_loop.h 不存在）
- [ ] **Step 3: 实现** `agent_loop.cpp` 核心 `run()`（完整逻辑，后续 Task 5 在同一函数上补分支）：

```cpp
// agent_loop.cpp 骨架（Task 5 会补的分支已留注释位）
Result<int64_t> AgentLoop::run(const AgentTask& task, std::atomic<bool>& cancelToken) {
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    auto runId = store_.startRun(task.id);
    if (runId.isFail()) return runId;

    std::vector<core::LlmMessage> messages;
    messages.push_back({"user", "[{\"type\":\"text\",\"text\":" + jsonString(task.goal) + "}]"});
    int64_t seq = 0;
    auto keep = [&](const std::string& role, const std::string& content,
                    const std::optional<std::string>& tid = std::nullopt) {
        store_.appendMessage(runId.value(), seq++, role, content, tid);
        messages.push_back({role, content});
    };
    // role=assistant 的消息不进 messages 前置——下面统一处理；keep 只用于 user 侧
    // assistant 响应直接落库+追加：
    auto keepAssistant = [&](const core::LlmResponse& r) {
        store_.appendMessage(runId.value(), seq++, "assistant", r.contentJson, std::nullopt);
        messages.push_back({"assistant", r.contentJson});
    };

    const std::string toolsJson = tools_.allSchemasJson();
    int iterations = 0;
    int64_t inTotal = 0, outTotal = 0;
    std::string status = "completed";
    std::optional<std::string> error;

    while (true) {
        if (cancelToken.load()) { status = "cancelled"; error = "用户取消"; break; }
        if (iterations >= std::min(task.maxIterations, 50)) {
            status = "limited"; error = "达到迭代上限 " + std::to_string(iterations); break;
        }
        // （Task 5 补：墙钟超时判定位）
        auto resp = llm_.send(messages, toolsJson, "", modelForTask(task));
        if (resp.isFail()) { status = "failed"; error = resp.errorMessage(); break; }
        inTotal += resp.value().inputTokens > 0 ? resp.value().inputTokens : 0;
        outTotal += resp.value().outputTokens > 0 ? resp.value().outputTokens : 0;
        ++iterations;
        keepAssistant(resp.value());
        if (resp.value().stopReason != "tool_use") {
            if (resp.value().stopReason == "max_tokens" || resp.value().stopReason == "stop_sequence") {
                status = "limited"; error = "stop_reason=" + resp.value().stopReason;
            }
            break;   // end_turn（或上面 limited）
        }
        // 解析 tool_use blocks → 执行 → tool_result
        auto blocks = core::platform_json_parse_array(resp.value().contentJson);  // 见下
        std::string toolResultsJson = "[";
        bool first = true;
        for (const auto& b : blocks) { /* 提取 type==tool_use 的 id/name/input */
            // find → NotFound（工具被拔）→ isError 回喂不中止（spec §1 L3a）
            // isDestructive() && !task.allowDestructive → status=failed + error，break 外层
            // execute 的 Result fail → ToolResult.isError=true 回喂，不中止
            // （Task 5 补：>32KB 截断）
        }
        toolResultsJson += "]";
        keep("user", toolResultsJson);
    }
    store_.finishRun(runId.value(), status, iterations,
                     inTotal > 0 ? std::optional<int64_t>(inTotal) : std::nullopt,
                     outTotal > 0 ? std::optional<int64_t>(outTotal) : std::nullopt,
                     error);
    return runId;
}
```

`modelForTask` 在 Task 6 接 LlmClient 前是**不入参**的占位（此任务 mock 不校验 model）——本任务先让 `send` 的 model 形参收 `task.modelSlot` 原样（`LlmEndpoint` 契约不变，Task 6 的 LlmClient 才负责解析为真实 model id）。上面 `modelForTask(task)` 即 `task.modelSlot`。

`jsonString(s)`：把字符串转 JSON 字符串字面量（含引号、转义 `\"` `\\` 控制符）——用 `platform::json`：`json::stringify(json::makeString(s))`。tool_use 解析用 `platform::json::parse(contentJson)` → `asObject`/数组遍历（`JsonValue::size()/operator[](int)/isObject()/asString()`），`input` 子树用 `json::stringify(inputValue)` 还原成文本传 `ToolArgs`。tool_result 块构造：`json::makeObject()` + `set("type", makeString("tool_result"))` + `set("tool_use_id", ...)` + `set("content", <解析后的 input 值或 makeString(原文)>)` + `set("is_error", makeBool(...))`，最后 `json::stringify(makeArray(...))`。**tool_result 的 content 用工具返回的 contentJson 解析后的值**（解析失败则 makeString 原文）。

（实现时以上 JSON 细节直接写成完整代码；本计划给的是无歧义的构造规则。）

- [ ] **Step 4: 跑测试确认通过**（1/1）
- [ ] **Step 5: Commit** `git commit -m "feat(agent): AgentLoop 主循环（消息累积 + tool_use 执行 + 落库）"`

### Task 5: AgentLoop 状态机分支（TDD 五连）

**Files:** Modify `src/module_agent/agent_loop.cpp`（+tool.h 的 isDestructive 已在 Task 4 加）
**Test:** `tests/test_modules/test_agent_loop.cpp` 追加

**Interfaces:** 无新增（全部走 Task 4 产出）。

每个分支 = 独立小步（写失败测试→跑红→实现/补分支→跑绿），全部通过后一次提交：

- [ ] **分支 1 迭代上限**：mock 永远返回 `stop_reason=="tool_use"`（单 tool_use 块，工具为真注册的 FakeTool），`maxIterations=3` → run 结束 `status=="limited"`、error 含"迭代上限"、`iterations==3`、`sentMessages.size()==3`。
- [ ] **分支 2 stop_reason 受限**：首响应即 `stop_reason=="max_tokens"` → `status=="limited"`、error 含 "max_tokens"。
- [ ] **分支 3 LLM 失败**：mock 返回 `Result::fail(NetworkTimeout, "超时")` → `status=="failed"`、error 含"超时"、落库 messages 里只有 goal 一条。
- [ ] **分支 4 破坏性授权**：注册 `isDestructive()==true` 的 FakeTool，mock 返回 tool_use 调它：
  - `allowDestructive=false` → `status=="failed"`、error 含"allow_destructive"、**`tool.calls==0`**（必须先拒后不调）；
  - `allowDestructive=true` → 工具被调、run 走到 end_turn `completed`。
- [ ] **分支 5 工具失败回喂**：FakeTool 返回 `Result<ToolResult>::fail(ValidationError,"参数错")` → run **不中止**，第二条请求的 messages 里 tool_result 块 `is_error==true` 且 content 含"参数错"，最终 end_turn → `completed`。
- [ ] **分支 6 tool_result 32KB 截断**：FakeTool 返回 40KB contentJson → 回喂内容长度 ≤ 32KB+标记余量、尾部含 `[truncated]`、run `completed`。
- [ ] **分支 7 cancel**：mock 首次返回后把 cancel 置 true（用自定义 MockLlm 在 send 内置位）→ 第二轮前中止，`status=="cancelled"`。
- [ ] **Commit** `git commit -m "feat(agent): AgentLoop 状态机分支（limited/failed/cancelled/破坏性授权/截断）"`

### Task 6: IClaudeProxyService::resolveModelSlot 扩展 + ClaudeService 实现

**Files:**
- Modify: `src/core/claude_proxy_service.h`（接口加虚方法 + `GlobalClaudeProxyService` 转发）
- Modify: `src/module_claude_proxy/claude_service.h` / `claude_service.cpp`（实现，**不改既有 43 签名**）
- Test: `tests/test_modules/test_claude_service.cpp` 追加用例

**Interfaces:**
- Produces（LlmClient/AgentLoop 依赖）:

```cpp
// IClaudeProxyService 追加（non-const：getActiveProfile 链全 non-const）
// 返回激活 profile 该槽位的实际 model id；无激活 profile / 槽位未配置 → nullopt
virtual std::optional<std::string> resolveModelSlot(const std::string& slot) = 0;

// GlobalClaudeProxyService 转发体：
std::optional<std::string> resolveModelSlot(const std::string& slot) override {
    T* svc = T::global();
    if (svc == nullptr) return std::nullopt;
    return svc->resolveModelSlot(slot);
}
```

- [ ] **Step 1: 写失败测试**（test_claude_service.cpp 既有模式：InMemoryDb + ClaudeService 直构）：

```cpp
REGISTER_TEST(claude_service_resolve_model_slot, "claude_service: resolveModelSlot 解析激活 profile 槽位", {
    test_support::InMemoryDb mem;
    module::claude::ClaudeService svc(mem.handle(), kTestKeychainService);
    auto p = svc.createProfile("R", "https://api.anthropic.com", "sk-test", "claude-sonnet-4-5");
    DOCTEST_REQUIRE(p.isOk());
    DOCTEST_REQUIRE(svc.setActiveProfile(p.value()).isOk());
    // 无 models_config → default 槽位回落 profile.model（既有语义：旧列是 default 的 fallback）
    DOCTEST_CHECK(svc.resolveModelSlot("default").value_or("") == "claude-sonnet-4-5");
    DOCTEST_CHECK(!svc.resolveModelSlot("sonnet").has_value());
    // 配置 models_config 后按槽位解析（用既有更新 profile 的接口写 modelsConfigJson）
    // modelsConfigJson 形如 {"def":{"model":"m-default","name":""},"sonnet":{"model":"m-s","name":""}}
    // 断言 resolveModelSlot("sonnet")=="m-s"、"def" 与 "default" 等价
    // 无激活 profile 的 service（新 InMemoryDb + 新 service 不 setActive）→ nullopt
    return true;
})
```

（modelsConfigJson 的写入用 `test_claude_service.cpp` 既有更新接口——查该文件同字段既有用例照抄调用名。）

- [ ] **Step 2: 确认失败**（resolveModelSlot 不存在）
- [ ] **Step 3: 实现** `ClaudeService::resolveModelSlot`（non-const）：

```cpp
std::optional<std::string> ClaudeService::resolveModelSlot(const std::string& slot) {
    std::lock_guard<std::mutex> lock(serviceMutex_);   // 复用类内既有互斥量；若无则新增 mutable std::mutex
    auto profile = profileRepo_.getActiveProfile();
    if (profile.isFail()) return std::nullopt;
    const std::string key = (slot == "def") ? "default" : slot;
    if (key == "default") {
        // 旧列语义：default 槽位未配置时回落 profile.model
        if (!profile.value().modelsConfigJson.empty()) {
            auto cfg = parseModelsConfig(profile.value().modelsConfigJson);
            if (cfg.isOk() && !cfg.value().def.model.empty())
                return cfg.value().def.model;
        }
        return profile.value().model.empty() ? std::nullopt
                                             : std::optional(profile.value().model);
    }
    if (profile.value().modelsConfigJson.empty()) return std::nullopt;
    auto cfg = parseModelsConfig(profile.value().modelsConfigJson);
    if (cfg.isFail()) return std::nullopt;
    // 槽位成员映射：与 DEVDESK_MODEL_SLOT_LIST 行一一对应（def/sonnet/opus/haiku/
    // fable/subagent）——查 provider_config.h 该宏全行，逐槽位 switch 返回
    // cfg.value().<member>.model；找不到键或 model 空 → nullopt
    return std::nullopt;
}
```

（成员名与槽位键以 `provider_config.h` 的 `DEVDESK_MODEL_SLOT_LIST` 为单一真相源；类内成员名 `profileRepo_` / 互斥量以 `claude_service.h` 实际字段为准，不存在则加。）
- [ ] **Step 4: 跑绿**（新用例 + test_claude_service 全文件 + 全量无回归）
- [ ] **Step 5: Commit** `git commit -m "feat(agent): IClaudeProxyService::resolveModelSlot（激活 profile 槽位→model id）"`

### Task 7: LlmClient（经本地代理的 LlmEndpoint 生产实现）

**Files:**
- Create: `src/module_agent/llm_client.h`、`src/module_agent/llm_client.cpp`
- Modify: `src/module_agent/CMakeLists.txt`
- Test: `tests/test_modules/test_llm_client.cpp` + CMakeLists 加行

**Interfaces:**
- Consumes: `platform::http::{Request,Response,send}`（`http_client.h`，自由函数）、`core::IClaudeProxyService`（含 Task 6 的 resolveModelSlot）。
- Produces: `class LlmClient final : public core::LlmEndpoint`：

```cpp
// src/module_agent/llm_client.h
#pragma once
#include <functional>
#include <string>
#include <vector>
#include "core/claude_proxy_service.h"
#include "core/llm_endpoint.h"
#include "core/result.h"
#include "platform/http_client.h"

namespace module::agent {

class LlmClient final : public core::LlmEndpoint {
public:
    using HttpFn = std::function<core::Result<platform::http::Response>(
        const platform::http::Request&)>;
    explicit LlmClient(core::IClaudeProxyService& proxy, HttpFn http = nullptr);
    core::Result<core::LlmResponse> send(
        const std::vector<core::LlmMessage>& messages,
        const std::string& toolsJson,
        const std::string& systemPrompt,
        const std::string& model) override;   // model = 已解析的实际 model id
private:
    core::IClaudeProxyService& proxy_;
    HttpFn http_;      // nullptr = 生产，用 platform::http::send
};
} // namespace module::agent
```

- [ ] **Step 1: 写失败测试**（fake IClaudeProxyService + lambda 假 http）：

```cpp
namespace {
class FakeProxy : public core::IClaudeProxyService {
public:
    bool running = true; int port = 8787;
    std::optional<std::string> resolved = std::string("claude-sonnet-5-5");
    bool isProxyRunning() const override { return running; }
    int proxyPort() const override { return port; }
    void setConfiguredProxyPort(int) override {}
    void stopProxy() override {}
    core::Result<int> startProxy(const std::string&, int) override { return core::Result<int>::fail(core::ErrorCode::Internal, "unused"); }
    bool isClaudeCodeManaged() const override { return false; }
    const std::vector<std::string>& lastSyncWarnings() const override { static const std::vector<std::string> w; return w; }
    std::string settingsFilePath() const override { return {}; }
    std::optional<std::string> resolveModelSlot(const std::string& slot) override { (void)slot; return resolved; }
};
} // namespace

REGISTER_TEST(llm_client_sends_via_local_proxy, "llm_client: 预检代理→POST 127.0.0.1:<port>/v1/messages→解析 LlmResponse", {
    FakeProxy proxy;
    platform::http::Request captured; int calls = 0;
    module::agent::LlmClient client(proxy, [&](const platform::http::Request& r) {
        ++calls; captured = r;
        platform::http::Response res;
        res.statusCode = 200;
        res.body = R"({"id":"m1","stop_reason":"end_turn","content":[{"type":"text","text":"hi"}],"usage":{"input_tokens":11,"output_tokens":7}})";
        return core::Result<platform::http::Response>::ok(res);
    });
    std::vector<core::LlmMessage> msgs{{"user", R"([{"type":"text","text":"q"}])"}};
    auto r = client.send(msgs, R"([{"name":"t.a"}])", "sys-prompt", "claude-sonnet-5-5");
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_EQ(calls, 1);
    DOCTEST_CHECK(captured.url.find("http://127.0.0.1:8787/v1/messages") != std::string::npos);
    DOCTEST_CHECK(captured.method == "POST");
    DOCTEST_CHECK(captured.body.find("\"model\":\"claude-sonnet-5-5\"") != std::string::npos);
    DOCTEST_CHECK(captured.body.find("\"system\":\"sys-prompt\"") != std::string::npos);
    DOCTEST_CHECK(captured.body.find("\"tools\":[{\"name\":\"t.a\"}]") != std::string::npos);
    DOCTEST_CHECK(captured.body.find("anthropic-version") == std::string::npos); // 代理补注入
    DOCTEST_CHECK_EQ(r.value().stopReason, std::string("end_turn"));
    DOCTEST_CHECK_EQ(r.value().inputTokens, 11);
    DOCTEST_CHECK_EQ(r.value().outputTokens, 7);
    return true;
})

REGISTER_TEST(llm_client_preflight_proxy_not_running, "llm_client: 代理未启→fail 不发包", {
    FakeProxy proxy; proxy.running = false;
    int calls = 0;
    module::agent::LlmClient client(proxy, [&](const platform::http::Request&) {
        ++calls; return core::Result<platform::http::Response>::fail(core::ErrorCode::NetworkUnreachable, "x");
    });
    auto r = client.send({{"user", "[]"}}, "[]", "", "m");
    DOCTEST_CHECK(r.isFail());
    DOCTEST_CHECK(r.errorCode() == core::ErrorCode::ValidationError);
    DOCTEST_CHECK_EQ(calls, 0);
    return true;
})

REGISTER_TEST(llm_client_slot_unresolved_fails_without_send, "llm_client: 槽位解析失败→fail 不发包", {
    FakeProxy proxy; proxy.resolved = std::nullopt;
    int calls = 0;
    module::agent::LlmClient client(proxy, [&](const platform::http::Request&) { ++calls; /*...*/ });
    auto r = client.send({{"user", "[]"}}, "[]", "", "does-not-matter");
    DOCTEST_CHECK(r.isFail());
    DOCTEST_CHECK_EQ(calls, 0);
    return true;
})
```

（注意：slot 解析失败用例里 model 形参传什么都 fail——契约是 LlmClient **忽略 model 形参重新解析**？**不是**：AgentLoop 已把 task.modelSlot 传给 send 的 model 形参？——**定案**：`LlmEndpoint::send` 的 model 形参收**已解析 model id**（Task 4 定案），解析动作在 **AgentLoop 拿到 LlmClient 前**不可行（loop 只见抽象）。**最终定案：解析在 LlmClient::send 内做**——`send` 的 model 形参语义改为「槽位键名」，LlmClient 内 `proxy_.resolveModelSlot(model)` 取实际 id；AgentLoop 原样传 `task.modelSlot`。上例第三用例据此断言；第一用例 FakeProxy.resolved 返回值才是进 body 的 model——把第一用例的断言改为 `"\"model\":\"claude-sonnet-5-5\""` 来自 resolved 而非形参（形参传 "default"）。）

- [ ] **Step 2: 确认失败** → **Step 3: 实现** `llm_client.cpp`：

```cpp
// 逻辑序（spec §1 步骤 3）：
// 1. !proxy_.isProxyRunning() → fail(ValidationError, "代理未启，请先在 Claude 页启动代理")
// 2. auto id = proxy_.resolveModelSlot(model); !id → fail(ValidationError, "无法解析模型槽位 ...")
// 3. 构造 body：json::makeObject
//    set("model", id)、set("max_tokens", makeInt(4096))（kLlmMaxTokens，spec 留白→计划定 4096）
//    set("messages", 解析各 message.contentJson 后组数组)（contentJson 本身是数组原文，逐条 parse 后 push 进 makeArray）
//    toolsJson != "[]" 时 set("tools", parse(toolsJson))；systemPrompt 非空 set("system", makeString)
//    stringify → req.body；req.method="POST"；req.url="http://127.0.0.1:"+std::to_string(proxy_.proxyPort())+"/v1/messages"
//    req.headers["content-type"]="application/json"；req.timeoutMs=60000
// 4. http_ ? http_(req) : platform::http::send(req)；失败原样透传 Result
// 5. statusCode!=200 → fail(HttpError, "上游 HTTP " + code + ": " + body 前 200 字节)
// 6. parse(body) → stop_reason/content(整数组 stringify)/usage.input_tokens/output_tokens → LlmResponse
```

（AgentLoop 的 Task 4 代码无需改：model 形参一直传的就是 task.modelSlot。）
- [ ] **Step 4: 跑绿**（3/3 + 全量）
- [ ] **Step 5: Commit** `git commit -m "feat(agent): LlmClient 经本地代理发 /v1/messages（预检+槽位解析+mock 注入缝）"`

### Task 8: TodoTools（6 个 ITool）

**Files:**
- Create: `src/module_todo/todo_tools.h`、`src/module_todo/todo_tools.cpp`
- Modify: `src/module_todo/CMakeLists.txt`、`src/module_todo/todo_module.cpp`（onStart 注册 / onStop 注销——找到 TodoService 构造处旁边挂 `std::unique_ptr<TodoTools>`）
- Test: `tests/test_modules/test_todo_tools.cpp` + CMakeLists 加行

**Interfaces:**
- Consumes: `TodoService`（`todo_service.h` 既有方法签名：`listColumns/listAllTasks/createTask(columnId,title)/updateTask(const Task&)/deleteTask/moveTask/completeTask/search`；`Task` 字段以该头为准）
- Produces: `class TodoTools { explicit TodoTools(TodoService& svc); void registerAll(); void unregisterAll(); }`——内部持 6 个 `shared_ptr<ITool>`：`todo.list / todo.create / todo.update / todo.move / todo.complete / todo.delete`（最后一个 `isDestructive()==true`）。

- [ ] **Step 1: 写失败测试**——`test_support::TestEnv env("todotools","t", /*registerAppDb=*/true)` 构造真实 TodoService（**照抄 `test_todo_service.cpp` 既有 TestEnv 构造段**），覆盖：
  - `todo.list` 空库返回 `"[]"` 或 `{"tasks":[]}` 形态、种 3 个任务后返回含 title；
  - `todo.create` 传 `{"title":"x"}` → TodoService 里多一条（listAllTasks 验证），column 缺省取第一列（listColumns()[0]）；
  - `todo.complete` 后该任务 completed 标记翻转（按 Task 字段断言）；
  - `todo.delete` 的 `isDestructive()==true`；
  - 非法参数（空 title）→ `Result<ToolResult>` ok 但 `isError==true`、content 含错误说明（**工具不 fail Result，错误回喂 LLM**——spec §10 M1 统一）。
  每个工具一个用例，schema 文本抽查 `input_schema` 含必填键。
- [ ] **Step 2: 确认失败** → **Step 3: 实现**：每个工具 = 一个小 class（`TodoListTool` 等，同文件），schema 用原始字符串字面量 `R"({"name":"todo.list","description":"列出全部待办","input_schema":{"type":"object","properties":{"completed":{"type":"boolean"}}}})"` 风格；execute 内 `platform::json::parse(args.json)` 取参 → 调 service → 结果组 `{"tasks":[...]}` stringify。`registerAll/unregisterAll` 调 `core::ToolRegistry::instance()`。
- [ ] **Step 4: 跑绿 + 全量** → **Step 5: Commit** `git commit -m "feat(agent): TodoTools 6 工具（todo 模块注册）"`

### Task 9: MarkdownTools M1（md.search / md.read）

**Files:** Create `src/module_markdown/markdown_tools.{h,cpp}`（同 Task 8 模式）；Modify `markdown_module.cpp`（onStart/ onStop 注册注销——MarkdownService 构造处旁）；Test `tests/test_modules/test_markdown_tools.cpp`。

- [ ] **Step 1: 失败测试**——用 `test_support::MdHeadlessEnv`（笔记目录+库+appDatabase 既有脚手架，照抄 `test_markdown_service.cpp` 构造段）种 2 个笔记文件：
  - `md.search` `{"query":"关键词"}` → 返回命中（`MarkdownService::search` 返回 `vector<NoteMeta>`，字段见 `markdown_service.h`）；
  - `md.read` `{"path":"notes/a.md"}` → content 含文件正文；不存在的 path → `isError==true` 回喂。
- [ ] **Step 2: 红 → Step 3: 实现 → Step 4: 绿** → **Step 5: Commit** `git commit -m "feat(agent): MarkdownTools M1（md.search/md.read）"`

### Task 10: AppStatsTool（module_agent 自实现）

**Files:** Create `src/module_agent/app_stats_tool.{h,cpp}`；Modify module_agent CMakeLists；Test `tests/test_modules/test_agent_tools.cpp`（与 Task 12 的 AgentModule 测试共用文件）。

- [ ] **Step 1: 失败测试**：`TestDb` 种数据（直接 SQL 插 todo_tasks 2 行、markdown_index 1 行、agent_tasks 1 行）→ `app.stats` execute 返回 JSON 含 `"todos":2`、`"notes":1`、`"agentTasks":1`。
- [ ] **Step 2: 红 → Step 3: 实现**：直查 `SELECT COUNT(*)`（列名注意 `SqlRow` 取 `values[0]`）；Produces: `std::shared_ptr<ITool> makeAppStatsTool(core::data::SqliteDb& db)`。
- [ ] **Step 4: 绿** → **Step 5: Commit** `git commit -m "feat(agent): AppStatsTool 只读统计"`

### Task 11: 装配层接线 + 契约测试

**Files:**
- Modify: `src/core/module_interface.h`（`PageId::Agent` 加在 About 前 + `pageIdToString` 加 `case PageId::Agent: return "Agent";`）
- Modify: `src/app_registry.h`（`extern "C" ::core::Module* create_agent_module();` + `kModuleRegistry` Agent 行插在 Settings 与 About 之间：`{::core::PageId::Agent, module::settings::kModuleIdAgent, "智能体", "\xEF\x8B\x9B", &create_agent_module}`——图标码点若不在 Font Awesome 7 Free-Solid 内会渲染空缺，取 `assets/Font Awesome 7 Free-Solid-900.otf` 已含的机器人码点 U+F544（`eui::utf8(0xF544)` 的 UTF-8 字节 `\xEF\x85\x84` 由实现者用 eui::utf8 核对生成，**以 test_module_icons 既有用例的校验通过为准**）
- Modify: `src/module_settings/settings_config.h`（`inline constexpr const char* kModuleIdAgent = "agent";`）
- Modify: `src/main.cpp`（工厂声明区加 `create_agent_module` 声明若在 main；`getRouter()` 持有区加 `static std::unique_ptr<::core::Module> agent(create_agent_module());` + `moduleForPage` switch 加 `case ::core::PageId::Agent: return agent.get();`）
- Modify: `src/CMakeLists.txt`（若 Task 3 未接 app 链接，此处确认 `devdesk_agent` 已链）
- Modify: `scripts/check-module-isolation.sh`（照 todo/markdown 的检查方式给 module_agent 加条目）
- Test Modify: `tests/test_core/test_router.cpp`（`core_pageid_order`：kEnumOrder 语义改 7 元素，断言 `agent=5`、`about=6`，标题同步）、`tests/test_modules/test_app_assembly.cpp`（`assembly_registry_table_shape`：size 6→7、kEnumOrder 插 Agent、`kModuleRegistry[5].gatingKey==kModuleIdAgent`、`[6].gatingKey==nullptr`；`assembly_build_gated_entries_real_table`：全开 gated 6→7 条、kEnumOrder 插 Agent——以两用例实际断言现状为准逐项扩展）、`tests/test_modules/test_module_icons.cpp`（若有图标码点校验，照既有模式加 Agent 行）

- [ ] **Step 1: 先改三个契约测试**（红）→ **Step 2: 跑，确认三处契约测试失败**
- [ ] **Step 3: 按上表实现全部接线**（注意：`create_agent_module` 尚不存在 → Task 12 会建；本任务先建**空壳** `src/module_agent/agent_module.{h,cpp}`：`AgentModule : core::Module` 返回 `PageId::Agent`、displayName "智能体"、compose 画占位 `ui.text("agent.placeholder").text("M1 接线中").build()`、onStart/onStop 空——Task 12 填实）

```cpp
// src/module_agent/agent_module.h（M1 空壳）
#pragma once
#include "core/module_interface.h"
namespace module::agent {
class AgentModule : public ::core::Module {
public:
    ::core::PageId pageId() const override { return ::core::PageId::Agent; }
    std::string displayName() const override { return "智能体"; }
    std::string shortName() const override { return "智能体"; }
    std::string iconGlyph() const override;   // eui::utf8(0xF544)，cpp 内实现
    void compose(::eui::Ui& ui, const ::eui::Screen& screen) override;
};
} // namespace module::agent
```

- [ ] **Step 4: 全量构建 + 测试绿**（契约测试转绿 + 无回归）+ `bash scripts/check-module-isolation.sh` 通过
- [ ] **Step 5: Commit** `git commit -m "feat(agent): 装配接线（PageId::Agent/registry/settings key/契约测试）"`

### Task 12: AgentModule 实体（teardown 顺序 + 手动触发 + 最小 UI）

**Files:**
- Modify: `src/module_agent/agent_module.{h,cpp}`（Task 11 空壳填实）
- Test: `tests/test_modules/test_agent_tools.cpp` 追加（AgentModule 生命周期用例）

**Interfaces:**
- Consumes: AgentStore / AgentLoop / LlmClient / ToolRegistry / makeAppStatsTool / `IClaudeProxyService`（main.cpp 已注册的 `GlobalClaudeProxyService` 适配器——通过 `app_assembly` 既有注入渠道拿；若该渠道不含它，在 `assembleAndStart` 的依赖注入段照 `IClaudeProxyService` 既有注册模式补）
- Produces: `extern "C" ::core::Module* create_agent_module();`（`agent_module.cpp` 底部，照其他模块工厂样式）

- [ ] **Step 1: 写失败测试**（HeadlessFrame 无头驱动，照 `test_claude_ui.cpp` 模式）：
  - `agent_module_registers_own_tool`：onStart 后 `ToolRegistry::instance().find("app.stats")` ok；onStop 后 fail；
  - `agent_module_onstop_cancels_and_joins`：构造慢 FakeTool（execute sleep 300ms）+ mock endpoint 起一个 run → 立即 onStop → 断言 run 线程 join 完成（onStop 返回后工具不再被调用）、run 落库 `cancelled`，且 unregister 发生在 join 之后（用时间戳或调用计数断言）。
- [ ] **Step 2: 红 → Step 3: 实现**——核心结构（完整）：

```cpp
// agent_module.h 成员（compose 之外的生命周期骨架）
class AgentModule : public ::core::Module {
    // ...Task 11 的覆写保持...
    void onStart() override;
    void onStop() override;
    // deps 由 main.cpp 装配段注入（构造参数或 setter；与既有模块同风格）
    struct Deps {
        core::data::SqliteDb* db = nullptr;
        core::IClaudeProxyService* proxy = nullptr;
    };
    explicit AgentModule(Deps d);
private:
    Deps deps_;
    std::unique_ptr<AgentStore> store_;
    std::unique_ptr<LlmClient> llm_;
    std::unique_ptr<AgentLoop> loop_;
    std::shared_ptr<core::ITool> statsTool_;
    struct ActiveRun { int64_t taskId; std::thread thread; std::atomic<bool> cancel{false}; };
    std::mutex runsMu_;
    std::vector<std::unique_ptr<ActiveRun>> runs_;
};
// onStart：sweepStaleRuns() → store_/llm_/loop_ 构造 → statsTool_ 注册
// onStop（spec §3 顺序，结构保证勿改）：
//   1) { lock; for r: r->cancel = true; }
//   2) for r: r->thread.join()（run 是同步循环，cancel 下轮迭代前退出）
//   3) runs_.clear()
//   4) ToolRegistry::instance().unregisterTool("app.stats")
// 手动启动（compose 的 onClick 回调）：
//   { lock; 若该 task 已有在跑 run → 忽略（同 task 不并发，spec §10）；
//     new ActiveRun{taskId}; emplace; }
//   r->thread = std::thread([this, r = r.get(), task = taskRecord] {
//       AgentLoop 与 store_ 的生命周期由 onStop 的 join 保证；
//       AgentTask t{...}; t.maxIterations = task.maxIterations;
//       t.allowDestructive = task.allowDestructive; t.modelSlot = task.modelSlot;
//       loop_->run(t, r->cancel);   // Result 落库在 run 内完成
//   });
```

最小 UI（compose）：`ui.column` 内——标题行 `ui.text("agent.title").text("智能体").build()`；任务列表每行 `ui.row`：任务名 text + 「启动」 `.onClick([this, id]{ startTask(id); })` + 最近 run 状态 text；新建表单：goal 输入框**照抄 `src/module_claude_proxy/claude_module.cpp` 表单编辑器的既有输入原语**（该文件是本项目表单文本输入的既定模式；M1 只需 goal 一个文本输入 + 默认槽位 "default" + max_iterations 默认 10）；run 详情：点击任务行展开 `listMessages` 最近 run 的 role+content 前 80 字符行。元素 id 一律 `agent.*` 前缀（可测试定位）。

元素 id 契约表（Task 13 的 UI 测试逐字钉这些 id，实现必须一致）：

| 元素 | id | 说明 |
|---|---|---|
| 页面根 | `agent.page` | 最外层 column，宽 = 逻辑宽 |
| 标题 | `agent.title` | text == "智能体" |
| 新建表单 goal 输入 | `agent.form.goal` | 文本输入（表单编辑器既有输入原语） |
| 新建提交按钮 | `agent.form.create` | button |
| 任务列表容器 | `agent.tasklist` | 每任务一行 |
| 任务行 | `agent.task.<taskId>` | row，点击展开/收起详情 |
| 任务名 | `agent.task.<taskId>.title` | text |
| 启动按钮 | `agent.task.<taskId>.run` | button，text == "启动" |
| 最近 run 状态 | `agent.task.<taskId>.status` | text，run 完成后含 "成功"/"失败"/"已取消" |
| run 详情容器 | `agent.task.<taskId>.detail` | 展开时才存在 |
| 消息行 | `agent.task.<taskId>.msg.<i>` | text，i 从 0 起，role+content 前 80 字符 |

`<taskId>` = AgentStore 主键的十进制字面量（如 `agent.task.3.run`）。按钮 id 经 `components::button(ui, "<id>")` 第二参传入（照 `claude_ui_list.cpp:616` 模式），文本元素经 `ui.text("<id>")`（照 `claude_ui_list.cpp:273` 模式）。

- [ ] **Step 4: 绿 + 全量** → **Step 5: Commit** `git commit -m "feat(agent): AgentModule 实体（teardown 顺序 + 手动触发 + 最小 UI）"`

### Task 13: 智能体页 UI 测试（无头几何断言 + probe）

**Files:**
- Create: `tests/test_modules/test_agent_ui.cpp`（**必须**登记进 `tests/CMakeLists.txt` 的 `add_executable(devdesk_tests ...)` 列表，漏登 configure 期 FATAL_ERROR）
- Create: `tests/probes/agent_probes.cpp`（GLOB 自动注册，ctest label `probe`，无需手工登记）
- Modify: `tests/CMakeLists.txt` probe 共用 `target_link_libraries` 列表加 `devdesk_agent`（一行；会使全部既有 probe 重链，属预期成本）

**Interfaces:**
- Consumes: Task 12 的元素 id 契约表（见 Task 12 末尾）、`module::agent::AgentModule(Deps{db, proxy})` 直接构造（fake 依赖注入，照 test_agent_tools.cpp 中 AgentModule 生命周期用例的构造方式；不需要 `create_agent_module()` 工厂）、既有桩 `FakeLlmEndpoint` / `FakeTool`（`test_agent_loop.cpp` / `test_agent_tools.cpp`，以实际存在者为准）
- Produces: UI 契约钉子——元素 id 改名、结构删除、文案改动即红

**背景事实（来自 probe-recipe.md 仓库调研，勿凭想象写 API）:**
- probe 需真窗口（`glfwInit`），无窗口 probe 无先例 → probe 是本机验收关卡；无头用例才是常规门禁
- probe 侧点击照抄 `claude_probes.cpp:169-182`：`core::queuePointerButton` press+release **成对**排队，下一帧 `composeFrame()` 才被消费；`Element` **无** `onTap` 字段
- probe **无**几何断言 helper：手搓 `check()` + `nearEq`（照 `about_probe.cpp:60,94-99`）
- probe 收尾顺序照抄 `about_probe.cpp:449-453`：`mod->onStop()` → `runtime.shutdown(false)` → `core::releaseInputQueue(window)` → `core::window::destroyWindow(window)` → `probe::terminateWindowBackend()`
- 无头侧 `eui::Ui` **无** `tap()`：点击 = 直接调 `el->onPress(ev, bounds)` + `el->onRelease(ev, bounds)`（照 `test_markdown.cpp:4420-4447`）；文本输入 = `el->onTextInput(core::TextInputEvent{text})`；裸 `Ui` 路线必须 `ui.layout(w, h)` 之后 frame 才有值（照 `test_ui_modern.cpp:1108-1117`）
- 侧边栏断言先例：`test_app_sidebar.cpp`（真 `app::compose` + 桩 `Router` + `RecordingModule`）

- [ ] **Step 1: 写失败无头测试**（红），五个用例，元素 id 逐字钉 Task 12 契约表：

```cpp
// tests/test_modules/test_agent_ui.cpp
// 公共：module::agent::AgentModule mod(Deps{&env.db.db(), &fakeProxy}) 直接构造
//（fakeProxy 照 test_agent_tools.cpp 既有 IClaudeProxyService 桩）；裸 eui::Ui +
// ui.begin("agent_ui") + mod->compose(ui, screen) + ui.end() + ui.layout(1600, 1000)。
//
// 1) agent_ui_compose_skeleton：空库 compose——
//    find("agent.page") 非空且 frame.width == 1600；
//    find("agent.title") 非空且 text == "智能体"；
//    find("agent.form.goal") 与 find("agent.form.create") 非空；
//    空库无任务行：find("agent.task.1") 为空。
// 2) agent_ui_create_task_via_form：对 agent.form.goal 逐字符
//    onTextInput("测试目标") → onPress/onRelease agent.form.create →
//    AgentStore::listTasks 恰 1 条且 goal=="测试目标"；re-compose 后
//    find("agent.task.<newId>.title") 非空。
// 3) agent_ui_run_button_triggers_loop：种 1 条任务；fakeProxy 返回 end_turn
//    且零工具调用 → onPress/onRelease agent.task.<id>.run → 有界轮询
//    （≤5s、10ms 步进；禁长 sleep）AgentStore::listRuns 顶部 status=="completed"；
//    re-compose 后 find("agent.task.<id>.status") 文本含 "成功"。
// 4) agent_ui_run_detail_expands：接 3) —— onPress/onRelease agent.task.<id>
//    （任务行本身）→ re-compose 后 find("agent.task.<id>.detail") 非空，
//    find("agent.task.<id>.msg.0") 文本含 role 前缀且长度 ≤ 80。
// 5) agent_ui_sidebar_registers_agent：照 test_app_sidebar.cpp 模式（真
//    app::compose + 桩 Router），Router 注册 AgentModule 实体（Deps 注入 fake），
//    断言侧边栏 compose 出现 "智能体" 项。
```

- [ ] **Step 2: 跑，确认 5 个用例红** → **Step 3: 核对钉子**——本任务只写测试不改实现；若钉子与 Task 12 实现不符，说明 Task 12 违反 id 契约，**改实现不改测试** → **Step 4: 绿**
- [ ] **Step 5: 写 probe**（`tests/probes/agent_probes.cpp`，骨架照 `.superpowers/sdd/2026-09-30-agent-m1-plan/probe-recipe.md` §3）：
  - `test_support::TestEnv` 建库（registerAsAppDatabase=true）→ `AgentModule` 直接构造（Deps 注入 fake）→ onStart → `probe::composeModuleFrame` 循环
  - 断言 `agent.page` 存在且 `frame.width` == 窗口逻辑宽（`nearEq` 0.5）
  - 断言 `agent.title` / `agent.form.goal` / `agent.form.create` 存在
  - 种 1 条任务 → composeFrame → `agent.task.1.run` 存在且 frame 完全落在 `agent.task.1` 行内（手搓包含判断）
  - 点击 `agent.task.1.run` 中心（queuePointerButton press+release 成对）→ composeFrame → 断言 run 落库（fakeProxy 桩返回 end_turn）
  - 收尾照 `about_probe.cpp:449-453` 顺序
- [ ] **Step 6: 跑 probe**：`(cd build && ctest -R agent_probes --output-on-failure)` 绿（本机 GUI session 内）
- [ ] **Step 7: 全量绿**：`devdesk_tests` 全量 + `(cd build && ctest -L probe --output-on-failure)` 全套 + `bash scripts/check-module-isolation.sh`
- [ ] **Step 8: Commit** `git commit -m "test(agent): 智能体页 UI 测试（无头几何断言 + probe）"`

### Task 14: 端到端验收 + 收尾

- [ ] **Step 1: 全量构建 + 测试**

```bash
cmake --build build && ./build/tests/devdesk_tests 2>&1 | grep -E "Total:|Passed:|Failed:|Skipped:"
```
Expected: Failed: 0。

- [ ] **Step 2: 模块隔离守卫** `bash scripts/check-module-isolation.sh` 通过。
- [ ] **Step 3: 真机手动验收**（可选项 = spec §11 集成行，需真 API key）：
  `bash scripts/build-test-run.sh --no-run` 后 `bash scripts/build-test-run.sh --no-test` 启动 app → 设置页开启「智能体」模块 → Claude 页确认代理已启 → 智能体页新建任务（goal：「列出全部待办并把第一条标题改为已完成测试」）→ 启动 → run 详情看到 todo.list → todo.update 调用链与最终回复。
- [ ] **Step 4: 检查 `git status --porcelain` 无 NUL 等垃圾 → 收尾提交（如有零散）→ 汇报 Total/Passed 基线对比。**

## Self-Review 记录

- **Spec 覆盖**：§2 抽象（T1/T4/T6/T7）、§3 组件与 teardown（T4/T12）、§4 协议（T7）、§5 手动触发/状态机（T5/T12）、§6 v12 三表（T2/T3）、§7 M1 工具 9 个（T8/T9/T10）、§8 最小 UI（T12 实现 + T13 UI 测试钉契约）、§9 装配清单全项（T11）、§10 控制（T5 各分支 + T3 sweep）、§11 mock 策略（各测试）、§14 M1 全行、§15 纪律（Global Constraints）。M2 范围（Scheduler/cron/其余 8 工具）明确不在本计划。
- **占位符扫描**：无 TBD/TODO；两处「照抄既有文件 X 的 Y 段」是对既有仓库代码的精确引用（非占位）；agent_store 非示例方法以「SQL+列序表」全量给定。
- **类型一致性**：`AgentTask.modelSlot`→`send(model=槽位键)`→`LlmClient::resolveModelSlot`（Task 7 定案）链路一致；`isDestructive()` 在 T4 声明、T5/T8 消费；`makeAppStatsTool` T10 定义 T12 消费。
- **Review Focus 落钉**：5/5 已在对应任务测试步骤中。
