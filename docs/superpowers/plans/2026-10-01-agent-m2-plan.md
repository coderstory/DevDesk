# 智能体 M2（调度 + 工具补全 + 资源上限）实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** agent M2 = spec §14 二期切分全量：`Scheduler`（cron 定时触发 + 常驻 worker 队列）、其余 8 个内置工具（md.create / md.list_tags / md.delete + ClaudeTools 4 个 + app.read_settings）、单 run 资源硬上限（墙钟 30 分钟 + 200K token）、agent 页 UI 补全（任务编辑 / cron 校验 / 启用禁用 / 常驻入队 / run 详情完整展示）。

**Architecture:** M1 已交付 `module_agent`（AgentLoop / LlmClient / AgentStore / AppStatsTool / 最小 UI，见 agent_module.h 头注释）。M2 增量：(1) 资源上限落在 AgentLoop 循环检查点（与既有 cancel/迭代检查同位，agent_loop.cpp:176-186）；(2) `Scheduler` 为 module_agent 内独立文件（spec §15），持有 tick 轮询线程 + 常驻 worker 线程 + 自己的 run 线程表；「同 task 不并发」原子闸**单点落在 `AgentLoop::run`**（agent_loop.cpp:126 换调 `AgentStore::startRunExclusive`），dispatchRun 为薄 spawn 包装、不预插行——手动与 cron 两路共用同一闸行；(3) 工具按模块归属落各自模块（MarkdownTools 扩展 3 个、ClaudeTools 新文件 4 个、AppTools 新文件 1 个），注册模式照 MarkdownTools::registerAll 先例；(4) UI 在 agent_module.cpp compose 内扩展，元素 id 契约表见 Task 10/11。

**Tech Stack:** C++20 / CMake / Ninja；自制迷你 doctest（REGISTER_TEST + DOCTEST_CHECK/REQUIRE，fail-fast——CHECK 失败即 return false，RAII 清理放栈对象析构）；platform::json；SqliteDb（db_.mutex() 锁内 prepare→bind→step 模式）。

**Spec:** `docs/superpowers/specs/2026-09-30-agent-design.md`（§5.1 cron 语法 / §5.2 资源上限与状态语义 / §7 工具清单 / §10 线程模型 / §14 M2 切分行）。M1 已合并（agent_loop / store / module / app.stats / md.search / md.read 均在 dev）。

## Global Constraints

- **资源硬上限（spec §5.2 逐字）**：单 run 墙钟 = `max_iterations × 请求超时`（请求超时默认 60s），**硬上限 30 分钟**；token 累计（input+output）**硬上限 200K**；达任一上限 → `status='limited'` + error 说明。`max_iterations` 硬上限 50 已由 M1 clamp（agent_loop.cpp:23）；单条 tool_result 32KB 截断已由 M1 实现（agent_loop.cpp:29）——均不重做。
- **cron 语法（spec §5.1 逐字）**：标准 5 字段 `分 时 日 月 周`；支持 `*` `,` `-` `/`；周字段 0-6（0=周日）；**不支持** Quartz 扩展（`L`/`W`/`#`）与英文名称；时区 = 本地时间；**错过的触发不补跑**；同分钟防重（lastFiredMinute）；表达式原文存 `agent_tasks.trigger_config`（JSON `{"cron":"…"}`）。
- **不抛异常**：错误一律 `core::Result<T>`；Scheduler/run 线程内不得让异常逃逸。
- **模块隔离铁律**：module_agent 只 include core 头（tool.h / llm_endpoint.h / claude_proxy_service.h）+ platform；各工具集归各自模块（`MarkdownTools` 归 module_markdown、`ClaudeTools` 归 module_claude_proxy、`AppTools` 归 module_settings）；改完跑 `bash scripts/check-module-isolation.sh` 必须绿。
- **破坏性授权（spec §5.3）**：`md.delete` / `claude.switch_profile` 覆写 `isDestructive()=true`，AgentLoop 在 `allowDestructive=0` 时直接终止 run（M1 已实现判定，agent_loop.cpp:283——M2 工具只覆写标志，不动 loop）。
- **teardown 顺序契约（spec §3，勿改）**：cancel 全部在跑 run → join → 停 tick/常驻 worker → 最后 unregisterTool。既有 AgentModule::onStop（agent_module.cpp:152-184）顺序在此基础上把 `Scheduler::stopAll()`（**判空调用**——onStart 早退路径 scheduler_ 可为 null）插在 join 之后、unregister 之前；stopAll **内部细化**（M2 审计修订）：stopFlag 置位 + join residentThread_ 先行（先灭再派发窗口），再 cancel/join 全部 run——契约意图（run 不活过收尾、工具最后注销）不变。
- **ToolRegistry 既有语义**（src/core/tool.cpp）：同名同指针注册 = no-op ok；同名不同指针 = fail。重入幂等守卫（started_）已就位，不动。
- **TDD**：每任务先失败测试后实现；测试名/断言按本计划，不为绿灯弱化。
- **构建**：一律 `bash scripts/build-test-run.sh`（禁手敲 cmake）；定向 `./build/tests/devdesk_tests <子串>`；**用例总数以运行输出 `Total:` 为准，不在文档写死**——执行 Task 1 前先跑一次全量记录当前基线（Total/Passed/Failed），Task 12 收尾对比零回归。
- **提交**：原子提交、显式 pathspec、禁 `git add -A`、提交前 `git status --porcelain`（仓库根 NUL 出现即删）。
- 本计划在 **dev 主检出**直接执行（agent M1 同款，非 worktree 新分支）。

## Review Focus（五类最易咬人的输入）

1. **同 task 双跑竞态**（手动「启动」与 cron tick / 常驻派发交叉）：两套 run 线程表各自判重挡不住跨表并发。钉子：Task 2 `startRunExclusive` 原子拒重用例 + Task 4 `scheduler_tick_skips_running_task` 用例 + Task 4 跨路径用例（同任务 手动 module 路径 × cron scheduler 路径交叠 → 第二路撞闸拒绝：两套 run 线程表合计 2 个在跑条目、DB 恰 1 行 running，非双插旧案的 3）。
2. **资源上限判定位置与顺序**：token/墙钟检查若放在工具执行后或 cancel 检查前，会出现超限继续跑 / 取消被吞。钉子：Task 1 token cap 与 wall-clock 两用例（cancel 检查先行、检查点在 loop 顶部）。
3. **Scheduler teardown 顺序**（stopAll 与 module onStop 嵌套）：常驻 run 在跑时 onStop 若先注销工具 = UAF；tick 线程未停 = 退出后触发派发；常驻线程若晚于在跑 run 收尾 = 收尾窗口里还能 pop 出新 job 逃出 teardown。钉子：Task 5 `scheduler_stopall_cancels_inflight_resident_run` + Task 6 `agent_module_onstop_stops_scheduler_runs`。
4. **md.delete / claude.switch_profile 破坏性面**：isDestructive 漏覆写（未授权也执行）或越界路径绕过 resolve 校验。钉子：Task 7 `md_delete_is_destructive_and_rejects_escape` + Task 8 switch_profile 未授权终止复跑 AgentLoop 既有授权用例。
5. **claude 工具线程安全**（agent 工作线程 vs UI 线程并发触 ClaudeService）：setActiveProfile 含 settings.json 接管同步（文件 + Keychain + warnings_ 内存态），无锁并发 = 数据竞争。钉子：Task 8 `setActiveProfile` 服务内锁实现 + claude 工具直接调用其余纯读方法（claude_service.h:178 无锁先例）用例。

---

### Task 1: AgentLoop 单 run 资源上限（墙钟 30 分钟 + token 200K）

**Files:**
- Modify: `src/module_agent/agent_loop.h`（ctor 加时钟注入形参）+ `src/module_agent/agent_loop.cpp`（两个检查点）
- Test: `tests/test_modules/test_agent_loop.cpp` 追加

**Interfaces:**
- Consumes: 既有 run 循环检查点结构（agent_loop.cpp:176-186：cancel → 迭代上限）；`inTotal/outTotal` 累计（:199-200）。
- Produces: `AgentLoop::AgentLoop(llm, tools, store, NowMsFn nowMs = {})`——`NowMsFn = std::function<int64_t()>`（steady 毫秒；默认 = `std::steady_clock`）。墙钟预算 = `min(effectiveMax × 60000ms, 1800000ms)`；循环顶部 cancel 检查之后加墙钟检查；token 200K 检查在 `keepAssistant`（agent_loop.cpp:202）**之后**——超限轮的 assistant 消息先持久化再判停（spec §10 可审计）。两处超限均 `status='limited'` + error，不动既有状态语义。

- [ ] **Step 1: 写失败测试**（test_agent_loop.cpp 追加；Fixture / resp / setupTask 用文件内既有形态，:146-167）：

```cpp
// ===== M2 Task 1：资源上限 =====

// spec §5.2：token 累计（input+output）硬上限 200K。单轮响应即破限也必须停
//（检查点在 keepAssistant 落库之后、下一轮工具执行之前——破限轮的 assistant
// 消息与用量照实落库（spec §10 审计），落库完立即判停，不再发第二轮）。
REGISTER_TEST(agent_loop_token_cap_limits_run,
              "agent_loop: token 累计超 200K → limited + error 说明", {
    Fixture fx;
    auto taskId = fx.store.createTask("t", "烧token", "manual", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());
    AgentTask task; task.id = taskId.value(); task.goal = "烧token"; task.modelSlot = "default";
    core::LlmResponse big = resp(R"([{"type":"text","text":"big"}])", "tool_use");
    big.inputTokens = 150000; big.outputTokens = 60000;    // 累计 21 万 > 200K
    core::LlmResponse next = resp(R"([{"type":"text","text":"more"}])", "end_turn");
    next.inputTokens = 10; next.outputTokens = 5;
    fx.llm.script = {big, next};   // 第二条必须永远发不出去
    std::atomic<bool> cancel{false};
    auto runId = fx.loop.run(task, cancel);
    DOCTEST_REQUIRE(runId.isOk());
    DOCTEST_REQUIRE_EQ(fx.llm.sentMessages.size(), 1u);    // 破限后不再发第二轮
    AgentRunRow run;
    DOCTEST_REQUIRE(onlyRun(fx, task, run));
    DOCTEST_CHECK_EQ(run.status, std::string("limited"));
    DOCTEST_REQUIRE(run.error.has_value());
    DOCTEST_CHECK(run.error->find("200K") != std::string::npos);
    DOCTEST_CHECK_EQ(run.inputTokens.value_or(0), 150000); // 用量照实落库
    DOCTEST_CHECK_EQ(run.outputTokens.value_or(0), 60000);
    return true;
})

// spec §5.2：单 run 墙钟 = max_iterations × 请求超时(60s)，硬上限 30 分钟。
// 注入假时钟：每次 send 前推进 10 分钟 → 第三轮前累计 30 分钟破硬上限
//（effectiveMax=10 使迭代上限 10×60s=600s 预算更紧——预算取 min，同样被钉）。
REGISTER_TEST(agent_loop_wall_clock_cap_limits_run,
              "agent_loop: 墙钟超预算 → limited（时钟注入零等待）", {
    Fixture fx;
    auto taskId = fx.store.createTask("t", "磨洋工", "manual", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());
    AgentTask task; task.id = taskId.value(); task.goal = "磨洋工"; task.modelSlot = "default";
    int64_t fakeNowMs = 0;
    AgentLoop loop{fx.llm, fx.tools, fx.store,
                   [&fakeNowMs] { return fakeNowMs; }};   // 注入时钟（新形参）
    fx.llm.onSendHook = [&fakeNowMs] { fakeNowMs += 10 * 60 * 1000; };  // 每轮 +10min
    core::LlmResponse tu = resp(R"([{"type":"text","text":"继续"}])", "tool_use");
    tu.inputTokens = 1; tu.outputTokens = 1;
    fx.llm.script = {tu, tu, tu, tu};   // 4 条远小于迭代上限 10
    std::atomic<bool> cancel{false};
    auto runId = loop.run(task, cancel);
    DOCTEST_REQUIRE(runId.isOk());
    AgentRunRow run;
    DOCTEST_REQUIRE(onlyRun(fx, task, run));
    DOCTEST_CHECK_EQ(run.status, std::string("limited"));
    DOCTEST_REQUIRE(run.error.has_value());
    DOCTEST_CHECK(run.error->find("墙钟") != std::string::npos);
    return true;
})
```

- [ ] **Step 2: 红**（AgentLoop 无第 4 形参 → 编译失败即 RED 证据；token 用例补上形参后断言 limited 失败为行为红）
- [ ] **Step 3: 实现**：

```cpp
// agent_loop.h：include <functional>；ctor 与成员
    using NowMsFn = std::function<int64_t()>;   // steady 毫秒；默认 = steady_clock
    AgentLoop(core::LlmEndpoint& llm, core::ToolRegistry& tools, AgentStore& store,
              NowMsFn nowMs = {});
    ...
    NowMsFn nowMs_;
```

```cpp
// agent_loop.cpp 匿名命名空间追加（kMaxIterationsHardCap 旁）：
// spec §5.2（M2 强制，原 M1 实况标注「刻意留空」由此接棒）：
constexpr int64_t kTokenHardCap = 200000;              // token 累计硬上限
constexpr int64_t kWallClockHardCapMs = 30 * 60 * 1000; // 单 run 墙钟硬上限 30 分钟
constexpr int64_t kRequestTimeoutMs = 60 * 1000;        // 请求超时默认（spec §5.2 表）
int64_t defaultNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
```

ctor 尾缀 `nowMs_(std::move(nowMs))`。`run()` 内 `effectiveMax` 计算后：

```cpp
    const int64_t startMs = nowMs_ ? nowMs_() : defaultNowMs();
    const int64_t wallBudgetMs =
        std::min<int64_t>(static_cast<int64_t>(effectiveMax) * kRequestTimeoutMs,
                          kWallClockHardCapMs);
```

循环顶部 cancel 检查之后（迭代上限检查之前）：

```cpp
        if ((nowMs_ ? nowMs_() : defaultNowMs()) - startMs >= wallBudgetMs) {
            status = "limited";
            error = "达到墙钟上限 " + std::to_string(wallBudgetMs / 1000) + "s";
            break;
        }
```

`keepAssistant(...)`（agent_loop.cpp:202，超限轮 assistant 消息已持久化）之后：

```cpp
        if (inTotal + outTotal > kTokenHardCap) {
            status = "limited";
            error = "token 累计超上限 200K（input " + std::to_string(inTotal) +
                    " + output " + std::to_string(outTotal) + "）";
            break;
        }
```

（include `<chrono>`；墙钟判定注释 agent_loop.cpp:187-190 的「刻意留空」段同步改写为指向本实现。）

- [ ] **Step 4: 绿**（定向 `./build/tests/devdesk_tests agent_loop` 全绿，既有 8 分支零回归）+ 全量
- [ ] **Step 5: Commit** `git commit -m "feat(agent): 单 run 墙钟 30 分钟 + token 200K 硬上限（时钟注入）" -- src/module_agent/agent_loop.h src/module_agent/agent_loop.cpp tests/test_modules/test_agent_loop.cpp`

### Task 2: AgentStore M2 扩展（原子互斥起跑 + 任务编辑/启停）

**Files:**
- Modify: `src/module_agent/agent_store.h` + `src/module_agent/agent_store.cpp`
- Modify: `src/module_agent/agent_loop.h`（`taskFromRow` 自由函数声明）+ `agent_loop.cpp`（实现 + 起跑换单闸 `startRunExclusive`）
- Test: `tests/test_modules/test_agent_store.cpp` 追加

**Interfaces:**
- Produces（全部走既有 `db_.mutex()` 锁内 prepare→bind→step 模式）:
  - `Result<int64_t> startRunExclusive(int64_t taskId)`——锁内先查 `SELECT 1 FROM agent_runs WHERE task_id=? AND status='running' LIMIT 1`，命中 → fail(`ErrorCode::AlreadyExists`, "任务已在运行")；否则 INSERT running 行（同 startRun）。**同一把锁内查+插 = 原子**：手动触发与 Scheduler 派发都经 AgentStore 同一 SqliteDb，互斥由 mutex 保证（Review Focus 1）。
  - `Result<void> updateTask(int64_t id, const std::string& name, const std::string& goal, const std::string& triggerType, const std::string& triggerConfig, const std::string& modelSlot, int maxIterations, bool allowDestructive)`——UPDATE 七列 + updated_at；**enabled 不在此列**（归 setTaskEnabled）。
  - `Result<void> setTaskEnabled(int64_t id, bool enabled)`。
  - `Result<AgentTaskRow> getTask(int64_t id)`——不存在 → fail(`ErrorCode::NotFound`)。
  - `createTask` 尾部加默认形参 `const std::string& triggerConfig = ""`（既有 6 参调用点零改动，仍绑空串）。
  - `AgentTask taskFromRow(const AgentTaskRow&)`（agent_loop.h 声明，module/scheduler/tests 三方共用，消灭 agent_module.cpp:243-249 的手抄映射）。
  - **单闸落点 = `AgentLoop::run`**（agent_loop.cpp:126）：`store_.startRun(task.id)` → `store_.startRunExclusive(task.id)`。**全仓唯一的「同 task 不并发」闸点**——手动路径（AgentModule 直调 loop_）与 Scheduler（Task 4 dispatchRun）都只经这一处；撞闸 → `run()` 原样上抛 fail(`AlreadyExists`)、不落行不发请求。签名与「返回 runId」语义不变（agent_loop.h:34 `Result<int64_t> run(const AgentTask&, std::atomic<bool>&)`）。

- [ ] **Step 1: 写失败测试**（test_agent_store.cpp 追加，照文件内既有 TestDb + AgentStore 直用形态）：

```cpp
REGISTER_TEST(agent_store_start_run_exclusive_rejects_concurrent,
              "agent_store: startRunExclusive 已有 running run → fail(AlreadyExists)", {
    test_support::TestDb tdb("agentstore", "exclusive");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("t", "g", "schedule", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());
    auto first = store.startRunExclusive(taskId.value());
    DOCTEST_REQUIRE(first.isOk());
    auto second = store.startRunExclusive(taskId.value());   // 并发第二次
    DOCTEST_REQUIRE(second.isFail());
    DOCTEST_CHECK(second.errorCode() == core::ErrorCode::AlreadyExists);
    // 终态后可再次起跑
    DOCTEST_REQUIRE(store.finishRun(first.value(), "completed", 1, std::nullopt,
                                    std::nullopt, std::nullopt).isOk());
    DOCTEST_CHECK(store.startRunExclusive(taskId.value()).isOk());
    return true;
})

REGISTER_TEST(agent_store_update_enable_get_roundtrip,
              "agent_store: updateTask/setTaskEnabled/getTask 往返 + createTask 带触发配置", {
    test_support::TestDb tdb("agentstore", "update");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("旧名", "旧目标", "manual", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());
    // updateTask 全列覆盖 + triggerConfig 落库
    DOCTEST_REQUIRE(store.updateTask(taskId.value(), "整理逾期", "新目标", "schedule",
                                     R"({"cron":"0 9 * * 1-5"})", "sonnet", 20, true)
                        .isOk());
    auto got = store.getTask(taskId.value());
    DOCTEST_REQUIRE(got.isOk());
    DOCTEST_CHECK_EQ(got.value().name, std::string("整理逾期"));
    DOCTEST_CHECK_EQ(got.value().triggerType, std::string("schedule"));
    DOCTEST_CHECK_EQ(got.value().triggerConfig, std::string(R"({"cron":"0 9 * * 1-5"})"));
    DOCTEST_CHECK_EQ(got.value().modelSlot, std::string("sonnet"));
    DOCTEST_CHECK_EQ(got.value().maxIterations, 20);
    DOCTEST_CHECK(got.value().allowDestructive);
    DOCTEST_CHECK(got.value().enabled);          // updateTask 不碰 enabled
    // setTaskEnabled 翻转 + getTask 回读；不存在 id → NotFound
    DOCTEST_REQUIRE(store.setTaskEnabled(taskId.value(), false).isOk());
    auto off = store.getTask(taskId.value());
    DOCTEST_REQUIRE(off.isOk());
    DOCTEST_CHECK_FALSE(off.value().enabled);
    DOCTEST_CHECK(store.getTask(99999).isFail());
    return true;
})
```

- [ ] **Step 2: 红**（新方法不存在 → 编译失败）→ **Step 3: 实现**（agent_store.cpp：startRunExclusive = 锁内 SELECT 存在性 + 复用 startRun 的 INSERT 体；updateTask/setTaskEnabled/getTask 照 listTasks 的 rowInt/rowStr 行取值模式；taskFromRow 逐字段搬运 id/name/goal/modelSlot/maxIterations/allowDestructive；agent_loop.cpp:126 换调 `startRunExclusive`——单闸落点，注释写明「手动与 Scheduler 两路共用此处」）
- [ ] **Step 4: 绿**（定向 `devdesk_tests agent_store`）+ 全量
- [ ] **Step 5: Commit** `git commit -m "feat(agent): AgentStore 原子互斥起跑 + 任务编辑/启停/单查" -- src/module_agent/agent_store.h src/module_agent/agent_store.cpp src/module_agent/agent_loop.h src/module_agent/agent_loop.cpp tests/test_modules/test_agent_store.cpp`

### Task 3: cron 纯函数（parse + matches，表驱动）

**Files:**
- Create: `src/module_agent/cron.h` + `src/module_agent/cron.cpp`
- Modify: `src/module_agent/CMakeLists.txt`（add_library 加 cron.cpp）
- Test: `tests/test_modules/test_agent_cron.cpp` 新建 + `tests/CMakeLists.txt` 登记进 `add_executable(devdesk_tests ...)`（test_agent_store.cpp 行旁；漏登记会被 CMake 自检断言拦下）

**Interfaces:**
- Produces:

```cpp
// src/module_agent/cron.h —— spec §5.1 R4 语法：5 字段「分 时 日 月 周」，
// 支持 * , - /；周 0-6（0=周日）；不支持 Quartz 扩展（L/W/#）与英文名称。
// 纯函数、无 IO 无时钟：parse 在任务保存时校验，matches 在 tick 时判定。
#pragma once
#include <array>
#include <string>
#include "core/result.h"

namespace module::agent {
struct CronExpr {
    std::array<bool, 60> minutes{};   // [0..59]
    std::array<bool, 24> hours{};     // [0..23]
    std::array<bool, 32> doms{};      // [1..31]，[0] 恒 false
    std::array<bool, 13> months{};    // [1..12]，[0] 恒 false
    std::array<bool, 8> dows{};       // [0..6]，0=周日
    bool domRestricted = false;       // 日字段非 *
    bool dowRestricted = false;       // 周字段非 *
};

// 非法（字段数≠5 / 越界 / 空字段 / 逆序区间 / 步长 0 / 非数字 / L W # / 英文）→ fail(ValidationError)
core::Result<CronExpr> parseCron(const std::string& expr);

// 纯匹配：只读 tm 的 tm_min/tm_hour/tm_mday/tm_mon/tm_wday（tm_wday 由调用方
// 经 core::time::localTime 填充，不从日期推导）。日/周同时受限时按 vixie-cron
// 标准取「或」：命中任一即当日命中；仅一方受限则该方必须命中。
bool cronMatches(const CronExpr& c, const std::tm& local);
} // namespace module::agent
```

- [ ] **Step 1: 写失败测试**（新文件，表驱动）：

```cpp
#include "doctest.h"
#include "module_agent/cron.h"
#include <ctime>

namespace {
std::tm tmOf(int min, int hour, int mday, int mon1to12, int wday) {
    std::tm t{};
    t.tm_min = min; t.tm_hour = hour; t.tm_mday = mday;
    t.tm_mon = mon1to12 - 1; t.tm_wday = wday;   // 0=周日
    return t;
}
bool fires(const std::string& expr, std::tm t) {
    auto c = module::agent::parseCron(expr);
    return c.isOk() && module::agent::cronMatches(c.value(), t);
}
} // namespace

REGISTER_TEST(cron_parse_and_match_table,
              "cron: parse/matches 表驱动（语法域 × 匹配域 × 拒绝域）", {
    // ---- 命中 ----
    DOCTEST_CHECK(fires("*/5 * * * *",  tmOf(0, 9, 5, 10, 3)));
    DOCTEST_CHECK(fires("*/5 * * * *",  tmOf(55, 23, 28, 2, 5)));
    DOCTEST_CHECK(!fires("*/5 * * * *", tmOf(3, 9, 5, 10, 3)));
    DOCTEST_CHECK(fires("0 9 * * 1-5",  tmOf(0, 9, 6, 10, 1)));   // 周一 09:00
    DOCTEST_CHECK(!fires("0 9 * * 1-5", tmOf(0, 9, 6, 10, 6)));   // 周六不中
    DOCTEST_CHECK(fires("30 14 1 * *",  tmOf(30, 14, 1, 7, 0)));  // 每月 1 日
    DOCTEST_CHECK(fires("5,15,45 * * * *", tmOf(15, 0, 2, 1, 2)));
    DOCTEST_CHECK(fires("10-30/10 * * * *", tmOf(20, 0, 2, 1, 2)));
    DOCTEST_CHECK(!fires("10-30/10 * * * *", tmOf(25, 0, 2, 1, 2))); // 步长跳过
    DOCTEST_CHECK(fires("0 12 * * 0", tmOf(0, 12, 7, 10, 0)));    // 周日=0
    // 日/周双受限 → vixie「或」（1 号或周日）
    DOCTEST_CHECK(fires("0 0 1 * 0", tmOf(0, 0, 15, 10, 0)));     // 周日非 1 号
    DOCTEST_CHECK(fires("0 0 1 * 0", tmOf(0, 0, 1, 10, 3)));      // 1 号非周日
    DOCTEST_CHECK(!fires("0 0 1 * 0", tmOf(0, 0, 15, 10, 3)));    // 两者皆非
    // 仅日受限 / 仅周受限 → 交集语义
    DOCTEST_CHECK(!fires("0 0 1 * *", tmOf(0, 0, 15, 10, 0)));
    DOCTEST_CHECK(!fires("0 0 * * 0", tmOf(0, 0, 15, 10, 3)));
    // ---- 拒绝 ----
    for (const char* bad : {"61 * * * *", "* 25 * * *", "* * 32 * *", "* * * 13 *",
                            "* * * * 7", "0 9 * *", "0 9 * * * *", "*/0 * * * *",
                            "9-5 * * * *", "MON * * * *", "L * * * *", "0 9,w * * *",
                            "", "  ", "* * * * *#2"}) {
        DOCTEST_CHECK_MESSAGE(module::agent::parseCron(bad).isFail(),
                              "应拒绝: " + std::string(bad));
    }
    return true;
})
```

- [ ] **Step 2: 红**（头不存在 → 编译失败）→ **Step 3: 实现**（cron.cpp：按 5 空格切分（连续空格视为一个分隔，trim 首尾）；单字段解析器 `parseField(text, lo, hi, out[], restricted)`——`*` → 全 true；`a-b/c`、`a/c`（a=lo 起）、单值、逗号列表逐段递归；数字严格校验（`std::from_chars`，拒绝空串/尾随字符）；`/` 步长 ≥1；区间 a≤b 否则拒；越界拒；非数字/字母拒）。`cronMatches`：month && hour && minute 恒交集；日/周按 `domRestricted && dowRestricted` 时取或、否则两者都须命中）
- [ ] **Step 4: 绿**（定向 `devdesk_tests cron`）+ 全量
- [ ] **Step 5: Commit** `git commit -m "feat(agent): cron 5 字段纯函数解析与匹配（表驱动）" -- src/module_agent/cron.h src/module_agent/cron.cpp src/module_agent/CMakeLists.txt tests/test_modules/test_agent_cron.cpp tests/CMakeLists.txt`

### Task 4: Scheduler 定时触发（tickNow + 派发 + 同分钟防重 + tick 轮询线程）

**Files:**
- Create: `src/module_agent/scheduler.h` + `src/module_agent/scheduler.cpp`（本任务先落定时触发一半；常驻队列 Task 5 补）
- Modify: `src/module_agent/CMakeLists.txt`（加 scheduler.cpp）
- Test: `tests/test_modules/test_agent_scheduler.cpp` 新建 + `tests/CMakeLists.txt` 登记

**Interfaces:**
- Consumes: Task 2 **单闸**（`AgentLoop::run` 内已换调 `startRunExclusive`——dispatchRun **不自己起跑、不预插行**，只 spawn 线程、落行交给 loop）/ `taskFromRow`、Task 3 `parseCron`/`cronMatches`、`core::time::localTime`（src/core/time.h:11，填 tm_wday）、AgentLoop（Task 1 后含资源上限）。
- Produces:

```cpp
// src/module_agent/scheduler.h（骨架；常驻成员 Task 5 加）
#pragma once
#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <vector>
#include "module_agent/agent_loop.h"
#include "module_agent/agent_store.h"

namespace module::agent {
class AgentLoop;

class Scheduler {
public:
    // now：unix 秒时钟缝（spec §5.1「时钟注入：构造注入 std::function<int64_t()>，
    // 测试零等待」）。nullptr = 真实系统时间。
    using NowFn = std::function<int64_t()>;
    Scheduler(AgentStore& store, AgentLoop& loop, NowFn now = {});
    ~Scheduler();                       // 兜底 stopAll（幂等）
    void start();                       // 起 tick 轮询线程（Task 5 再起常驻线程）
    void stopAll();                     // 停派发线程（tick/常驻）→ cancel 全部在跑 run → join（幂等）
    // 对每个 enabled 且 trigger_type='schedule' 的任务：解析 trigger_config 的
    // cron → matches(本地 tm) → 命中且 lastFiredMinute_[id] != 本分钟 → 派发。
    // 公开供测试直调（零等待）；tick 线程每 200ms 实时醒来探一次分钟翻转。
    core::Result<void> tickNow();
    size_t activeRunCount();            // 测试观察面：runs_ 容器条目数（含已收尾未 reap 的）
private:
    struct ActiveRun {
        int64_t taskId = 0;
        std::thread thread;
        std::atomic<bool> cancel{false};
        std::atomic<bool> done{false};
        std::atomic<int64_t> runId{0};  // loop_.run 返回；0 = 未起（撞闸/起跑失败）
    };
    core::Result<void> dispatchRun(int64_t taskId);  // 薄 spawn 包装：闸在 AgentLoop::run 内
    void reapDone();
    AgentStore& store_;
    AgentLoop& loop_;
    NowFn now_;
    std::atomic<bool> stopFlag_{false};
    std::thread tickThread_;
    std::mutex mu_;
    std::map<int64_t, int64_t> lastFiredMinute_;    // taskId → 上次触发的分钟序（now/60）
    std::vector<std::unique_ptr<ActiveRun>> runs_;  // 主线程写/读（stopAll 在主线程）
};
} // namespace module::agent
```

dispatchRun 核心体（**薄 spawn 包装**：不预插 run 行、不自己查闸——闸在 `AgentLoop::run`；run 线程 lambda 与 agent_module.cpp:240-259 同构）：

```cpp
core::Result<void> Scheduler::dispatchRun(int64_t taskId) {
    auto row = store_.getTask(taskId);
    if (row.isFail()) return core::Result<void>::fail(row.errorCode(), row.errorMessage());
    AgentTask task = taskFromRow(row.value());
    auto run = std::make_unique<ActiveRun>();
    run->taskId = taskId;
    ActiveRun* raw = run.get();
    runs_.push_back(std::move(run));
    try {
        raw->thread = std::thread([this, raw, task] {
            auto r = loop_.run(task, raw->cancel);   // 单闸 + 落行都在 loop 内
            if (r.isFail()) {
                // 撞闸（他路同 task 在跑，ErrorCode::AlreadyExists）或起跑失败：
                // 本路从未落行，无悬挂 running 要收尾，只记 WARN（Review Focus 1）
                LOG_WARN("Scheduler: run 未起 task=" + std::to_string(task.id) +
                         ": " + r.errorMessage());
            } else {
                raw->runId.store(r.value());         // runId 取自 loop_.run 的返回值
            }
            raw->done.store(true);
        });
    } catch (const std::system_error& e) {
        // 线程创建失败：闸尚未被调用（在 loop 内）→ 库中无行可收尾，只摘除占位
        for (auto it = runs_.begin(); it != runs_.end(); ++it)
            if (it->get() == raw) { runs_.erase(it); break; }
        return core::Result<void>::fail(core::ErrorCode::Internal, e.what());
    }
    return core::Result<void>::ok();
}
```

tickNow 核心：

```cpp
core::Result<void> Scheduler::tickNow() {
    auto tasks = store_.listTasks();
    if (tasks.isFail()) return tasks;   // Result<void> 化由调用侧 ErrorMessage 透传
    const int64_t nowSec = now_ ? now_() : static_cast<int64_t>(std::time(nullptr));
    const int64_t minute = nowSec / 60;
    std::tm local{};
    core::time::localTime(nowSec, &local);   // tm_wday 同步填充（time.h:11）
    for (const auto& t : tasks.value()) {
        if (!t.enabled || t.triggerType != "schedule") continue;
        auto cfg = platform::json::parse(t.triggerConfig);
        std::string cronText;
        if (cfg.isOk() && cfg.value().isObject())
            if (auto v = cfg.value().asObject().get("cron"); v.isString()) cronText = v.asString();
        if (cronText.empty()) continue;              // 配置缺失/坏 → 静默跳过不派发
        auto expr = parseCron(cronText);
        if (expr.isFail()) continue;                 // 保存期已校验；运行期坏配置不炸 tick
        if (!cronMatches(expr.value(), local)) continue;
        {   // 同分钟防重（spec §5.1 lastFiredMinute）
            std::lock_guard<std::mutex> lock(mu_);
            if (lastFiredMinute_[t.id] == minute) continue;
            lastFiredMinute_[t.id] = minute;
        }
        auto fired = dispatchRun(t.id);
        if (fired.isFail()) LOG_WARN("Scheduler: 派发失败 task=" + std::to_string(t.id) + ": " + fired.errorMessage());
    }
    return core::Result<void>::ok();
}
```

start()/stopAll()：start 起 `tickThread_`（循环：实时 sleep 200ms → stopFlag 检查 → 分钟序与 lastSeen 不同则 `tickNow()` 并记 lastSeen——注入时钟测试靠直接调 tickNow，线程只是生产驱动）；stopAll = stopFlag 置位 → join tickThread_ →（Task 5 后：**stopFlag 置位 + notify + join `residentThread_` 先行**）→ cancel 全部 ActiveRun → join → 清 runs_（**顺序契约 spec §3 步骤 1-3**；「先灭派发源、再收在跑 run」的理由见 Task 5）。`reapDone()`（摘除已 done 条目）在 `tickNow()` 入口与 `stopAll()` 收尾各调一次——`activeRunCount()` 只读条目数、不负责摘。幂等：stopFlag_ 交换守卫。

- [ ] **Step 1: 写失败测试**（新文件；MockLlm / ToolReg 照 test_agent_loop.cpp:22-45,129-144 自带副本或 include 共享桩——以最小改动为准，允许文件内重定义；**文件内 MockLlm 的 send 加 `std::mutex`**——常驻并行派发下两个 run 会并发 send，共享桩的 script/游标/sentMessages 必须自保。文件内辅助：`SlowToolForSched`（睡 300ms 的记账工具）/ `FastToolForSched`（立即返回）/ `ToolRegGuard` / `countStatus(rows, status)`）：

```cpp
REGISTER_TEST(scheduler_tick_fires_matching_task_once_per_minute,
              "scheduler: enabled+schedule+cron 命中 → 派发一次；同分钟不重复；下一分钟再触发", {
    test_support::TestDb tdb("agentsched", "tick");
    module::agent::AgentStore store(tdb.handle());
    MockLlm llm; llm.script = { endTurnResp() };            // 立即 end_turn
    module::agent::AgentLoop loop(llm, core::ToolRegistry::instance(), store);
    int64_t nowSec = 3600 * 24;                              // 任意整基点
    module::agent::Scheduler sched(store, loop, [&nowSec] { return nowSec; });
    auto taskId = store.createTask("每分钟", "心跳", "schedule", "default", 5, false,
                                   R"({"cron":"* * * * *"})");   // triggerConfig = 末位形参
    DOCTEST_REQUIRE(taskId.isOk());
    DOCTEST_REQUIRE(sched.tickNow().isOk());
    for (int i = 0; i < 500 && sched.activeRunCount() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    DOCTEST_REQUIRE_EQ(sched.activeRunCount(), 1u);
    sched.stopAll();                                         // join，run 落 completed
    auto runs = store.listRuns(taskId.value(), 10);
    DOCTEST_REQUIRE(runs.isOk());
    DOCTEST_REQUIRE_EQ(runs.value().size(), 1u);
    DOCTEST_CHECK_EQ(runs.value()[0].status, std::string("completed"));
    // 同分钟：不重复派发
    DOCTEST_REQUIRE(sched.tickNow().isOk());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    DOCTEST_CHECK_EQ(sched.activeRunCount(), 0u);
    DOCTEST_CHECK(store.listRuns(taskId.value(), 10).value().size() == 1u);
    // 下一分钟：再触发
    nowSec += 61;
    DOCTEST_REQUIRE(sched.tickNow().isOk());
    for (int i = 0; i < 500 && sched.activeRunCount() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    DOCTEST_CHECK_EQ(sched.activeRunCount(), 1u);
    sched.stopAll();
    return true;
})

REGISTER_TEST(scheduler_tick_skips_disabled_manual_and_bad_cron,
              "scheduler: disabled/manual/坏 cron 配置 → 不派发不崩", {
    test_support::TestDb tdb("agentsched", "skip");
    module::agent::AgentStore store(tdb.handle());
    MockLlm llm; llm.script = { endTurnResp() };
    module::agent::AgentLoop loop(llm, core::ToolRegistry::instance(), store);
    module::agent::Scheduler sched(store, loop, [] { return int64_t(3600 * 24); });
    auto dis = store.createTask("禁用", "g", "schedule", "default", 5, false, R"({"cron":"* * * * *"})");
    DOCTEST_REQUIRE(dis.isOk());
    DOCTEST_REQUIRE(store.setTaskEnabled(dis.value(), false).isOk());
    auto man = store.createTask("手动", "g", "manual", "default", 5, false, R"({"cron":"* * * * *"})");
    DOCTEST_REQUIRE(man.isOk());
    auto bad = store.createTask("坏配置", "g", "schedule", "default", 5, false, R"({"cron":"BROKEN"})");
    DOCTEST_REQUIRE(bad.isOk());
    auto noCfg = store.createTask("无配置", "g", "schedule", "default", 5, false);
    DOCTEST_REQUIRE(noCfg.isOk());
    DOCTEST_REQUIRE(sched.tickNow().isOk());                 // 不抛不崩
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    DOCTEST_CHECK_EQ(sched.activeRunCount(), 0u);
    for (int64_t id : {dis.value(), man.value(), bad.value(), noCfg.value()})
        DOCTEST_CHECK(store.listRuns(id, 10).value().empty());
    sched.stopAll();
    return true;
})

REGISTER_TEST(scheduler_tick_skips_running_task,
              "scheduler: 任务在跑时下一分钟 tick → startRunExclusive 拒绝，不双跑", {
    test_support::TestDb tdb("agentsched", "concurrent");
    module::agent::AgentStore store(tdb.handle());
    MockLlm llm;
    core::ToolRegistry::instance().unregisterTool("test.tick");
    SlowToolForSched tool;   // 文件内定义：execute 睡 300ms 的 test.tick 工具
    ToolRegGuard guard{core::ToolRegistry::instance(), &tool};
    llm.script = { toolUseResp("test.tick"), endTurnResp() };
    module::agent::AgentLoop loop(llm, core::ToolRegistry::instance(), store);
    int64_t nowSec = 3600 * 24;
    module::agent::Scheduler sched(store, loop, [&nowSec] { return nowSec; });
    auto taskId = store.createTask("慢任务", "g", "schedule", "default", 5, false, R"({"cron":"* * * * *"})");
    DOCTEST_REQUIRE(taskId.isOk());
    DOCTEST_REQUIRE(sched.tickNow().isOk());
    for (int i = 0; i < 500 && tool.calls.load() < 1; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    DOCTEST_REQUIRE(tool.calls.load() == 1);                 // 第一跑在途（睡 300ms）
    nowSec += 61;                                            // 下一分钟
    DOCTEST_REQUIRE(sched.tickNow().isOk());                 // 起线程 → loop_.run 撞闸
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    // 被拒那一路不落行 → DB 仍只有 1 行 running（sched.runs_ 里会多一条已收尾的被拒
    // 条目，由下次 tick / stopAll 的 reapDone() 摘除——activeRunCount 是条目数、
    // 不是「在跑数」，别拿它当跑数断言）
    auto mid = store.listRuns(taskId.value(), 10);
    DOCTEST_REQUIRE(mid.isOk());
    DOCTEST_CHECK_EQ(countStatus(mid.value(), "running"), 1u);
    DOCTEST_CHECK_EQ(tool.calls.load(), 1);                  // 未起第二跑
    sched.stopAll();                                         // cancel → run 落 cancelled
    auto runs = store.listRuns(taskId.value(), 10);
    DOCTEST_REQUIRE_EQ(runs.value().size(), 1u);
    DOCTEST_CHECK_EQ(runs.value()[0].status, std::string("cancelled"));
    return true;
})

// Review Focus 1 跨路径钉子：手动路径（AgentModule 同款：独立线程直调 loop_.run）×
// cron 路径（tickNow → dispatchRun）同任务交叠。单闸在 AgentLoop::run 内，第二路
// 撞闸即拒（run 线程内 LOG_WARN，无行可落）：两套 run 线程表合计 2 个在跑条目，
// DB 恰 1 行 running——旧「两路各自预插」案会落 2 行 running、同一任务双跑。
REGISTER_TEST(scheduler_cross_path_single_gate_no_double_run,
              "scheduler: 手动 × cron 同任务交叠 → 单闸拒第二路（DB 恰 1 行 running）", {
    test_support::TestDb tdb("agentsched", "crossgate");
    module::agent::AgentStore store(tdb.handle());
    MockLlm llm;
    core::ToolRegistry::instance().unregisterTool("test.cross");
    SlowToolForSched tool;   // 睡 300ms（Task 4 定义复用）：把第一跑钉在闸内
    ToolRegGuard guard{core::ToolRegistry::instance(), &tool};
    llm.script = { toolUseResp("test.cross"), endTurnResp() };
    module::agent::AgentLoop loop(llm, core::ToolRegistry::instance(), store);
    int64_t nowSec = 3600 * 24;
    module::agent::Scheduler sched(store, loop, [&nowSec] { return nowSec; });
    auto taskId = store.createTask("交叠", "g", "schedule", "default", 5, false,
                                   R"({"cron":"* * * * *"})");
    DOCTEST_REQUIRE(taskId.isOk());
    // 路径 A：手动（AgentModule 的 run 线程同款——独立线程直调 loop_.run）
    auto rowA = store.getTask(taskId.value());
    DOCTEST_REQUIRE(rowA.isOk());
    AgentTask manualTask = module::agent::taskFromRow(rowA.value());
    std::atomic<bool> manualCancel{false};
    std::thread manual([&] { (void)loop.run(manualTask, manualCancel); });
    for (int i = 0; i < 500 && tool.calls.load() < 1; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    DOCTEST_REQUIRE(tool.calls.load() == 1);                 // A 在跑（闸被占）
    // 路径 B：cron tick 派发同任务 → 起线程 → loop_.run 撞闸（WARN 在 run 线程内）
    DOCTEST_REQUIRE(sched.tickNow().isOk());
    for (int i = 0; i < 500 && sched.activeRunCount() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    DOCTEST_CHECK_EQ(sched.activeRunCount(), 1u);            // B 条目在（线程已起）
    DOCTEST_CHECK_EQ(tool.calls.load(), 1);                  // 第二路从未执行
    auto mid = store.listRuns(taskId.value(), 10);
    DOCTEST_REQUIRE(mid.isOk());
    DOCTEST_CHECK_EQ(countStatus(mid.value(), "running"), 1u);   // DB 恰 1 行 running
    manualCancel.store(true);                                // A 收尾（在跑的 run 落 cancelled）
    manual.join();
    sched.stopAll();
    auto after = store.listRuns(taskId.value(), 10);
    DOCTEST_REQUIRE(after.isOk());
    DOCTEST_REQUIRE_EQ(after.value().size(), 1u);            // 第二路从未落行
    DOCTEST_CHECK_EQ(after.value()[0].status, std::string("cancelled"));
    return true;
})
```

（`countStatus(rows, "running")` = 文件内小 helper（`<algorithm>` count_if），与 `SlowToolForSched` / `ToolRegGuard` 同段落定义；撞闸那一路的 LOG_WARN 无日志捕获设施可断言，故以其可观察后果（第二路未落行 + 未执行工具）钉住。本用例自持 `manual` 线程：按文件 fail-fast 口径配一个栈上 RAII join 守卫（析构里 `cancel.store(true)` + `join()`），断言提前 return 时不留 joinable thread（否则栈对象析构即 `std::terminate`）。）

- [ ] **Step 2: 红**（scheduler.h 不存在 → 编译失败）→ **Step 3: 实现**（如上 Interfaces 两段 + start/stopAll；include core/time.h、core/logging.h、platform/json.h）
- [ ] **Step 4: 绿**（定向 `devdesk_tests scheduler`）+ 全量
- [ ] **Step 5: Commit** `git commit -m "feat(agent): Scheduler cron 定时触发（tickNow + 同分钟防重 + 原子互斥派发）" -- src/module_agent/scheduler.h src/module_agent/scheduler.cpp src/module_agent/CMakeLists.txt tests/test_modules/test_agent_scheduler.cpp tests/CMakeLists.txt`

### Task 5: Scheduler 常驻 worker 队列 + stopAll 全量收口

**Files:**
- Modify: `src/module_agent/scheduler.h` + `src/module_agent/scheduler.cpp`
- Test: `tests/test_modules/test_agent_scheduler.cpp` 追加

**Interfaces:**
- Consumes: Task 4 dispatchRun / stopAll 骨架。
- Produces: `void enqueueResident(int64_t taskId)`（手动入队——spec §5.1：常驻本阶段只跑手动入队子任务）；私有 `residentThread_` + `residentQueue_`（`std::deque<int64_t>`，mu_ 保护）+ `cv_`。常驻线程循环：cv 等待 → stopFlag 或队列空则续等/退出 → pop taskId → `dispatchRun`（pop 即 spawn、不等上一跑结束；同 task 连发两单撞单闸由 `AgentLoop::run` 拒绝，不阻塞队列）。**stopAll 扩展顺序（M2 审计修订）**：① stopFlag 置位 + notify + join `residentThread_` **先行**（先灭派发源、再收在跑 run——反序会让常驻线程在收尾窗口里再 pop 一个 job、spawn 出新 run 逃出 teardown）；② 再 cancel 全部在跑 run（含常驻在跑的）→ join 全部；③ 清 runs_。派发复用 dispatchRun ⇒ 常驻 run 同样受**单闸**（`AgentLoop::run` 内）与 AgentLoop 资源上限约束（spec §5.1「常驻任务每轮 run 仍是单次 AgentLoop」）。

- [ ] **Step 1: 写失败测试**：

```cpp
REGISTER_TEST(scheduler_resident_queue_runs_enqueued_jobs,
              "scheduler: 常驻队列两个不同任务各起一跑（确定 2 行 run）；空队列不空转", {
    test_support::TestDb tdb("agentsched", "resident");
    module::agent::AgentStore store(tdb.handle());
    MockLlm llm;   // 文件内副本：send 带 std::mutex（并行派发下两 run 并发 send）
    core::ToolRegistry::instance().unregisterTool("test.res");
    FastToolForSched tool;   // 立即返回（文件内定义）
    ToolRegGuard guard{core::ToolRegistry::instance(), &tool};
    llm.script = { toolUseResp("test.res"), endTurnResp(),
                   toolUseResp("test.res"), endTurnResp() };
    module::agent::AgentLoop loop(llm, core::ToolRegistry::instance(), store);
    module::agent::Scheduler sched(store, loop, {});         // 生产时钟即可
    sched.start();                                           // 起常驻线程
    // 两个**不同** taskId：闸不互相拒 → 跑数确定（同 task 连发的闸语义见下一用例）。
    // 常驻线程 pop 即 spawn、不等上一跑结束，两跑响应的游标交错顺序不定 → 只断言
    // 「每任务恰 1 跑 + 终态 completed」，不断言工具调用次数。
    auto a = store.createTask("常驻A", "g", "resident", "default", 5, false);
    auto b = store.createTask("常驻B", "g", "resident", "default", 5, false);
    DOCTEST_REQUIRE(a.isOk());
    DOCTEST_REQUIRE(b.isOk());
    sched.enqueueResident(a.value());
    sched.enqueueResident(b.value());
    for (int i = 0; i < 1000; ++i) {                         // 有界等两跑落终态
        auto ra = store.listRuns(a.value(), 10);
        auto rb = store.listRuns(b.value(), 10);
        if (ra.isOk() && rb.isOk() && ra.value().size() == 1 && rb.value().size() == 1 &&
            ra.value()[0].status != "running" && rb.value()[0].status != "running") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    auto ra = store.listRuns(a.value(), 10);
    auto rb = store.listRuns(b.value(), 10);
    DOCTEST_REQUIRE(ra.isOk());
    DOCTEST_REQUIRE(rb.isOk());
    DOCTEST_REQUIRE_EQ(ra.value().size(), 1u);               // 每任务恰 1 跑（无第三跑）
    DOCTEST_REQUIRE_EQ(rb.value().size(), 1u);
    DOCTEST_CHECK_EQ(ra.value()[0].status, std::string("completed"));
    DOCTEST_CHECK_EQ(rb.value()[0].status, std::string("completed"));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));   // 空队列不空转
    DOCTEST_CHECK_EQ(store.listRuns(a.value(), 10).value().size(), 1u);
    DOCTEST_CHECK_EQ(store.listRuns(b.value(), 10).value().size(), 1u);
    sched.stopAll();
    return true;
})

REGISTER_TEST(scheduler_resident_same_task_twice_single_gate,
              "scheduler: 同 task 连发两单 → 单闸只放一跑（1 行 run；第二单 WARN 后收尾）", {
    test_support::TestDb tdb("agentsched", "residentdup");
    module::agent::AgentStore store(tdb.handle());
    MockLlm llm;
    core::ToolRegistry::instance().unregisterTool("test.slow");
    SlowToolForSched tool;   // 睡 300ms：第一跑钉在闸内，第二单必撞闸
    ToolRegGuard guard{core::ToolRegistry::instance(), &tool};
    llm.script = { toolUseResp("test.slow"), endTurnResp() };
    module::agent::AgentLoop loop(llm, core::ToolRegistry::instance(), store);
    module::agent::Scheduler sched(store, loop, {});
    sched.start();
    auto taskId = store.createTask("常驻重发", "g", "resident", "default", 5, false);
    DOCTEST_REQUIRE(taskId.isOk());
    sched.enqueueResident(taskId.value());
    sched.enqueueResident(taskId.value());                   // 同 task 连发两单
    for (int i = 0; i < 1000; ++i) {                         // 有界等唯一一跑落终态
        auto runs = store.listRuns(taskId.value(), 10);
        if (runs.isOk() && runs.value().size() == 1 &&
            runs.value()[0].status != "running") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    auto runs = store.listRuns(taskId.value(), 10);
    DOCTEST_REQUIRE(runs.isOk());
    DOCTEST_REQUIRE_EQ(runs.value().size(), 1u);             // 第二单从未落行
    DOCTEST_CHECK_EQ(runs.value()[0].status, std::string("completed"));
    DOCTEST_CHECK_EQ(tool.calls.load(), 1);                  // 单闸只放一跑（第二单未执行）
    sched.stopAll();
    return true;
})

REGISTER_TEST(scheduler_stopall_cancels_inflight_resident_run,
              "scheduler: stopAll 取消在跑的常驻 run 并 join（teardown 契约）", {
    test_support::TestDb tdb("agentsched", "stop");
    module::agent::AgentStore store(tdb.handle());
    MockLlm llm;
    core::ToolRegistry::instance().unregisterTool("test.slow");
    SlowToolForSched tool;                                   // 睡 300ms（Task 4 定义复用）
    ToolRegGuard guard{core::ToolRegistry::instance(), &tool};
    llm.script = { toolUseResp("test.slow") };
    module::agent::AgentLoop loop(llm, core::ToolRegistry::instance(), store);
    module::agent::Scheduler sched(store, loop, {});
    sched.start();
    auto taskId = store.createTask("常驻慢", "g", "resident", "default", 5, false);
    DOCTEST_REQUIRE(taskId.isOk());
    sched.enqueueResident(taskId.value());
    for (int i = 0; i < 500 && tool.calls.load() < 1; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    DOCTEST_REQUIRE(tool.calls.load() == 1);                 // 在跑（睡 300ms 中）
    sched.stopAll();                                         // 阻塞到 cancel+join 完成
    auto runs = store.listRuns(taskId.value(), 10);
    DOCTEST_REQUIRE_EQ(runs.value().size(), 1u);
    DOCTEST_CHECK_EQ(runs.value()[0].status, std::string("cancelled"));
    DOCTEST_CHECK_EQ(sched.activeRunCount(), 0u);
    return true;
})
```

（撞闸用例（本任务 `scheduler_resident_same_task_twice_single_gate` 与 Task 4 `scheduler_cross_path_single_gate_no_double_run`）里第二路的 LOG_WARN 由撞闸的 run 线程打印——测试框架无日志捕获设施，故以其可观察后果（未落行 + 工具未执行）钉住；确定性来自 300ms 慢工具把第一跑钉在闸内，两单 pop 间隔远小于此。）

- [ ] **Step 2: 红**（enqueueResident 不存在 → 编译失败）→ **Step 3: 实现**（常驻线程 + 队列 + stopAll 扩展（**先停常驻线程、再 cancel/join 全部 run**——先灭派发源，收尾窗口才不会被新 job 逃逸）；cv wait 谓词含 stopFlag_；队列元素出队后照常 dispatchRun——同 task 连发两单时第二单在 run 线程内撞单闸，LOG_WARN 后收尾（无行可落），不阻塞队列）
- [ ] **Step 4: 绿**（定向 `devdesk_tests scheduler`，Task 4 用例零回归）+ 全量
- [ ] **Step 5: Commit** `git commit -m "feat(agent): Scheduler 常驻 worker 队列 + stopAll 全量收口" -- src/module_agent/scheduler.h src/module_agent/scheduler.cpp tests/test_modules/test_agent_scheduler.cpp`

### Task 6: AgentModule 装配 Scheduler（生命周期契约接长）

**Files:**
- Modify: `src/module_agent/agent_module.h`（Deps 加时钟缝、成员加 scheduler_）+ `src/module_agent/agent_module.cpp`
- Test: `tests/test_modules/test_agent_tools.cpp` 追加

**Interfaces:**
- Consumes: Task 4/5 Scheduler、既有 onStart/onStop 契约（agent_module.cpp:103-184）。
- Produces: `AgentModule::Deps` 加 `Scheduler::NowFn schedulerNow;`（nullptr = 生产真实时钟；测试注入假时钟驱动 tick）。onStart：loop_ 创建成功后 `scheduler_ = std::make_unique<Scheduler>(*store_, *loop_, deps_.schedulerNow); scheduler_->start();`。onStop：既有「全取消 → 全 join → 清容器」之后、`unregisterTool("app.stats")` **之前**插入**判空**调用 `if (scheduler_) scheduler_->stopAll();`（spec §3 步骤 3「停常驻 worker 线程」的落位；db 为 null 时 onStart 早退、scheduler_ 从未创建——不判空即空指针崩）。成员声明序：`scheduler_` 放在 `loop_` / `llm_` / `store_` **之后**（逆向析构：先亡 scheduler_，再轮到它引用的 loop_/store_）；释放序 `loop_.reset()` 在 `scheduler_.reset()` 之前（Scheduler 持 AgentLoop&，stopAll 已 join 其线程，reset 安全）。compose 的常驻任务「入队」按钮由 Task 10 接线，本任务不碰 UI。

- [ ] **Step 1: 写失败测试**（test_agent_tools.cpp 追加；FakeProxy/ToolGuard/SlowTool 文件内已有，:103-156）：

```cpp
REGISTER_TEST(agent_module_onstop_stops_scheduler_runs,
              "agent_module: onStop 收口 Scheduler 在跑 run（cancel→join→停线程→注销工具）", {
    test_support::TestDb tdb("agenttools", "schedstop");
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};
    core::ToolRegistry::instance().unregisterTool("app.stats");   // 起点干净
    core::ToolRegistry::instance().unregisterTool("test.slow");
    auto slow = std::make_shared<SlowTool>();
    SlowTool* tool = slow.get();
    DOCTEST_REQUIRE(core::ToolRegistry::instance().registerTool(slow).isOk());
    ToolGuard slowGuard{"test.slow"};

    module::agent::AgentStore store(tdb.handle());
    auto created = store.createTask("定时慢", "调用 test.slow 一次", "schedule", "default", 10, false,
                                    R"({"cron":"* * * * *"})");
    DOCTEST_REQUIRE(created.isOk());
    const int64_t taskId = created.value();

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.proxy = &proxy;
    std::atomic<int64_t> fakeNow{3600 * 24};
    deps.schedulerNow = [&fakeNow] { return fakeNow.load(); };    // 假时钟注入
    int httpCalls = 0;
    deps.http = [&httpCalls](const platform::http::Request&) {
        const int n = ++httpCalls;
        platform::http::Response res;
        res.statusCode = 200;
        res.body = R"({"id":"m1","stop_reason":"tool_use","content":[)"
                   R"({"type":"tool_use","id":"tu1","name":"test.slow","input":{}}],)"
                   R"("usage":{"input_tokens":10,"output_tokens":5}})";
        return core::Result<platform::http::Response>::ok(res);
    };
    std::unique_ptr<::core::Module> mod(new module::agent::AgentModule(deps));
    mod->onStart();
    fakeNow.store(3600 * 24 + 60);      // 推进一分钟 → tick 线程 ≤200ms 内探到翻转
    for (int i = 0; i < 500 && tool->calls.load() < 1; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    DOCTEST_REQUIRE_MESSAGE(tool->calls.load() == 1,
                            "tick 线程 5s 内未派发 cron run（装配缺失？）");

    mod->onStop();                       // 阻塞到 cancel+join+停线程完成
    DOCTEST_CHECK(core::ToolRegistry::instance().find("app.stats").isFail());
    DOCTEST_CHECK_EQ(tool->calls.load(), 1);                  // onStop 后不再被调
    auto runs = store.listRuns(taskId, 10);
    DOCTEST_REQUIRE(runs.isOk());
    DOCTEST_REQUIRE_EQ(runs.value().size(), 1u);
    DOCTEST_CHECK_EQ(runs.value()[0].status, std::string("cancelled"));
    return true;
})
```

- [ ] **Step 2: 红**（Deps 无 schedulerNow → 编译失败；补形参后用例卡「5s 未派发」为行为红）→ **Step 3: 实现**（如 Interfaces；onStart 的 scheduler 装配置于 statsTool_ 注册之前、llm_/loop_ 创建分支内）
- [ ] **Step 3b: 手抄映射除重**（agent_module.cpp:243-249 那六行 `t.id=…t.allowDestructive=…` 字段搬运删掉，改 `AgentTask t = taskFromRow(taskRecord);`——taskFromRow（Task 2）是 store 行 → AgentTask 的唯一映射，run 线程里不留第二份会漂移的副本；scheduler / tests 同用它）
- [ ] **Step 4: 绿**（定向 `devdesk_tests agent_module`，既有 onstop_cancels_and_joins / onstart_reentry_idempotent 零回归）+ 全量
- [ ] **Step 5: Commit** `git commit -m "feat(agent): AgentModule 装配 Scheduler（onStart 起 / onStop 契约顺序收口）" -- src/module_agent/agent_module.h src/module_agent/agent_module.cpp tests/test_modules/test_agent_tools.cpp`

### Task 7: MarkdownTools M2 三工具（md.create / md.list_tags / md.delete）

**Files:**
- Modify: `src/module_markdown/markdown_tools.cpp`（三工具类 + ctor 登记；头文件不动——tools_ 容器已泛化）
- Test: `tests/test_modules/test_markdown_tools.cpp` 追加

**Interfaces:**
- Consumes: `MarkdownService::write(relPath, content)`（markdown_service.cpp:690，建父目录+索引同步）、`read(relPath)`（存在性探测）、`listAllTags()`（TagCount{name,noteCount}，markdown_service.h:35-38,103）、`remove(relPath)`（:783，越界路径经 resolve 拒绝）。
- Produces: 三工具按 md.search/md.read 同文件同类形态追加进 `MarkdownTools::MarkdownTools` ctor：
  - `md.create` `{path, content}`（写但可逆）：`svc_.read(path)` isOk → toolError("笔记已存在，拒绝覆盖（如需重建先 md.delete）")；否则 write → toolOk(`{"path":"…","created":true}`)。
  - `md.list_tags` `{}`（只读）：listAllTags → toolOk(`{"tags":[{"name":…,"noteCount":N},…]}`，数组用逐元素 stringify 拼接先例 markdown_tools.cpp:108-116)。
  - `md.delete` `{path}`：**`isDestructive() const override { return true; }`**（spec §7 破坏性列）；execute = remove → toolOk(`{"path":…,"deleted":true}`)；remove fail → toolError(原错误消息)。

- [ ] **Step 1: 写失败测试**（test_markdown_tools.cpp 追加，ToolEnv / runTool / seedNotes 文件内已有，:28-70）：

```cpp
REGISTER_TEST(md_create_list_tags_delete_roundtrip,
              "md_tools: md.create 建笔记（拒绝覆盖）、md.list_tags 统计、md.delete 破坏性删除", {
    ToolEnv env("m2create");
    env.tools->registerAll();
    // create：成功 + 重复拒绝
    auto made = runTool("md.create", R"({"path":"notes/m2.md","content":"# M2\n正文"})");
    DOCTEST_REQUIRE(made.isOk());
    DOCTEST_CHECK_FALSE(made.value().isError);
    auto dup = runTool("md.create", R"({"path":"notes/m2.md","content":"再写"})");
    DOCTEST_REQUIRE(dup.isOk());
    DOCTEST_CHECK(dup.value().isError);                      // 已存在 → isError 回喂
    auto readBack = runTool("md.read", R"({"path":"notes/m2.md"})");
    DOCTEST_REQUIRE(readBack.isOk());
    DOCTEST_CHECK(readBack.value().contentJson.find("正文") != std::string::npos);
    // list_tags：给 m2.md 打 tag 后统计（经 service setTags）
    DOCTEST_REQUIRE(env.svc->setTags(std::filesystem::path("notes/m2.md"),
                                     {"alpha"}).isOk());
    auto tags = runTool("md.list_tags", "{}");
    DOCTEST_REQUIRE(tags.isOk());
    DOCTEST_CHECK_FALSE(tags.value().isError);
    DOCTEST_CHECK(tags.value().contentJson.find("\"name\":\"alpha\"") != std::string::npos);
    // delete：破坏性标志 + 删除生效 + 越界路径拒绝
    auto delTool = core::ToolRegistry::instance().find("md.delete");
    DOCTEST_REQUIRE(delTool.isOk());
    DOCTEST_CHECK(delTool.value()->isDestructive());
    auto gone = runTool("md.delete", R"({"path":"notes/m2.md"})");
    DOCTEST_REQUIRE(gone.isOk());
    DOCTEST_CHECK_FALSE(gone.value().isError);
    auto after = env.svc->read(std::filesystem::path("notes/m2.md"));
    DOCTEST_CHECK(after.isFail());                           // 磁盘已删
    auto escape = runTool("md.delete", R"({"path":"../../etc/passwd"})");
    DOCTEST_REQUIRE(escape.isOk());
    DOCTEST_CHECK(escape.value().isError);                   // resolve 拒绝越界
    return true;
})
```

- [ ] **Step 2: 红**（find("md.create") NotFound → runTool fail）→ **Step 3: 实现**（三工具类照 MdSearchTool 形态；md.create 的存在性探测复用 svc_.read；解析复用 parseArgs/strField）
- [ ] **Step 4: 绿**（定向 `devdesk_tests md_`，既有 search/read 用例零回归）+ 全量 + `bash scripts/check-module-isolation.sh`
- [ ] **Step 5: Commit** `git commit -m "feat(markdown): md.create/md.list_tags/md.delete 智能体工具" -- src/module_markdown/markdown_tools.cpp tests/test_modules/test_markdown_tools.cpp`

### Task 8: ClaudeTools 四工具 + setActiveProfile 服务内锁

**Files:**
- Create: `src/module_claude_proxy/claude_tools.h` + `src/module_claude_proxy/claude_tools.cpp`
- Modify: `src/module_claude_proxy/claude_service.h`（加 `std::mutex switchMutex_;` 成员）+ `claude_service.cpp`（setActiveProfile 加锁）
- Modify: `src/module_claude_proxy/claude_module.cpp`（onStart 注册 / onStop 注销）+ `CMakeLists.txt`（加 claude_tools.cpp）
- Test: `tests/test_modules/test_claude_tools.cpp` 新建 + `tests/CMakeLists.txt` 登记

**Interfaces:**
- Consumes: `ClaudeService::listProfiles()`（claude_service.h:61）、`getActiveProfile()`（:126）、`setActiveProfile(id)`（:245，**破坏性**——切激活 + 代理运行中同步接管 settings.json）、`getProfile(id)`/`loadApiKey(id)`（:64,:122，testConnection 前装 key）、`testConnection(profile)`（:184）、`recentCalls(limit)`（:195）。
- Produces: `class ClaudeTools { explicit ClaudeTools(ClaudeService& svc); void registerAll(); void unregisterAll(); }`（MarkdownTools 同型）。四工具：
  - `claude.list_profiles` `{}`（只读）：`{"profiles":[{"id":…,"name":…,"active":bool}]}`，active 由 getActiveProfile 的 id 比对。
  - `claude.switch_profile` `{id}`：`isDestructive()=true`；id 非 int → toolError；setActiveProfile(id) fail → toolError(原消息)。
  - `claude.test_connection` `{id?}`（只读，spec §5.3「只读（发测试请求）」）：id 缺省 → getActiveProfile；getProfile + loadApiKey 填 `Profile::apiKeyPlain` → testConnection → toolOk(`{"statusCode":N,"ok":bool}`)；任一步 fail → toolError。
  - `claude.recent_logs` `{limit?}`（只读）：limit 默认 20、clamp [1,100]；recentCalls → `{"logs":[{"id":…,"statusCode":…,"latencyMs":…,"error":…,"startedAt":…}]}`。
- 线程安全落点（spec §10 M3 plan 裁定）：**仅 setActiveProfile 加服务内锁**——方法体首行 `std::lock_guard<std::mutex> lock(switchMutex_);`；其余三个读方法按 claude_service.h:178 既有裁定（纯读随 SqliteDb recursive_mutex 串行化）直调零改动；testConnection 为本地副本上的纯计算 + 网络，无共享内存态。

- [ ] **Step 1: 写失败测试**（新文件；ClaudeService 构造照 test_claude_service.cpp:139 形态 `ClaudeService svc(tdb.handle(), kTestKeychainService)`，需要 Keychain 桩则照 :510 FakeKeychain / :592 ScopedFakeKeychain 复用）：

```cpp
REGISTER_TEST(claude_tools_register_and_list_and_switch,
              "claude_tools: 注册四工具；list_profiles 活跃标记；switch_profile 破坏性 + 生效", {
    test_support::TestDb tdb("claudetools", "main");
    module::claude::ClaudeService svc(tdb.handle(), kTestKeychainService);
    auto p1 = svc.createProfile("A", "https://api.anthropic.com", "sk-test-a",
                                "claude-sonnet-5-5", /*activateIfNoneActive=*/true,
                                /*sharedConfigId=*/0);
    DOCTEST_REQUIRE(p1.isOk());
    module::claude::ClaudeTools tools(svc);
    tools.registerAll();
    for (const char* n : {"claude.list_profiles", "claude.switch_profile",
                          "claude.test_connection", "claude.recent_logs"})
        DOCTEST_CHECK(core::ToolRegistry::instance().find(n).isOk());
    auto listed = runToolName("claude.list_profiles", "{}");
    DOCTEST_REQUIRE(listed.isOk());
    DOCTEST_CHECK(listed.value().contentJson.find("\"active\":true") != std::string::npos);
    auto p2 = svc.createProfile("B", "https://api.anthropic.com", "sk-test-b",
                                "claude-sonnet-5-5", /*activateIfNoneActive=*/false,
                                /*sharedConfigId=*/0);
    DOCTEST_REQUIRE(p2.isOk());
    auto sw = core::ToolRegistry::instance().find("claude.switch_profile");
    DOCTEST_REQUIRE(sw.isOk());
    DOCTEST_CHECK(sw.value()->isDestructive());
    auto switched = runToolName("claude.switch_profile",
                                R"({"id":)" + std::to_string(p2.value().id) + "}");
    DOCTEST_REQUIRE(switched.isOk());
    DOCTEST_CHECK_FALSE(switched.value().isError);
    auto active = svc.getActiveProfile();
    DOCTEST_REQUIRE(active.isOk());
    DOCTEST_CHECK_EQ(active.value().id, p2.value().id);      // 真的切了
    tools.unregisterAll();
    DOCTEST_CHECK(core::ToolRegistry::instance().find("claude.list_profiles").isFail());
    return true;
})
```

（`runToolName` = find→execute 小 helper 照 test_markdown_tools.cpp:44 形态文件内定义；createProfile 实参已按 claude_service.h:83 真实签名对齐——5th `bool activateIfNoneActive`、6th `int64_t sharedConfigId`，authField/dialectOverride/fallbackProfileId/extraSettingsJson 留默认；p1 建时置活跃，list_profiles 的 `"active":true` 才有得比。）
- [ ] **Step 2: 红**（claude_tools.h 不存在 → 编译失败）→ **Step 3: 实现**（claude_tools.{h,cpp} 四工具 + claude_module.cpp onStart 尾段 `claudeTools_ = std::make_unique<ClaudeTools>(*svc); claudeTools_->registerAll();`、onStop 首段 `claudeTools_->unregisterAll(); claudeTools_.reset();`（**先于** stopProxy）+ 成员声明 + CMake 登记源文件）
- [ ] **Step 4: 实现 setActiveProfile 锁**（claude_service.h 成员区加 `std::mutex switchMutex_;  // spec §10 M3：agent 工作线程与 UI 线程并发切换互斥`；claude_service.cpp:245 方法体首行加 lock_guard）+ 定向 `devdesk_tests claude_tools` 绿
- [ ] **Step 5: 绿** + 全量 + 隔离守卫
- [ ] **Step 6: Commit** `git commit -m "feat(claude): 智能体四工具 + setActiveProfile 服务内互斥锁" -- src/module_claude_proxy/claude_tools.h src/module_claude_proxy/claude_tools.cpp src/module_claude_proxy/claude_service.h src/module_claude_proxy/claude_service.cpp src/module_claude_proxy/claude_module.cpp src/module_claude_proxy/CMakeLists.txt tests/test_modules/test_claude_tools.cpp tests/CMakeLists.txt`

### Task 9: AppTools（app.read_settings，module_settings）

**Files:**
- Create: `src/module_settings/app_tools.h` + `src/module_settings/app_tools.cpp`
- Modify: `src/module_settings/settings_module.cpp`（onStart 注册 / onStop 注销）+ `CMakeLists.txt`（加 app_tools.cpp）
- Test: `tests/test_modules/test_settings.cpp` 追加（文件已有 SettingsStore 临时文件往返模式）

**Interfaces:**
- Consumes: `SettingsStore(path).load()`（settings_config.h:206-215，const 纯读——工具自持 SettingsStore 实例，不摸模块成员，agent 线程天然安全）、`core::paths::configFile()`（settings_module.cpp:186 同源）。
- Produces: `std::shared_ptr<core::ITool> makeAppReadSettingsTool(const std::filesystem::path& configPath);`——工具 `app.read_settings` `{}` 只读，execute = SettingsStore(configPath).load() → toolOk(`{"themeMode":…,"darkTheme":bool,"fontScalePercent":N,"editorFontSize":N,"logRetentionDays":N,"proxyPort":N,"windowEffect":"…","sidebarCollapsed":bool,"enabledModules":[…]}`)；读失败（磁盘异常）→ isError 回喂。settings_module.cpp onStart：store_ 创建后 `appReadSettingsTool_ = makeAppReadSettingsTool(::core::paths::configFile()); ToolRegistry::instance().registerTool(...)`（失败 LOG_ERROR 照 agent statsTool_ 模式）；onStop 注销 + reset。

- [ ] **Step 1: 写失败测试**（test_settings.cpp 追加，用临时配置文件路径）：

```cpp
REGISTER_TEST(app_read_settings_tool_roundtrip,
              "app_tools: app.read_settings 读回持久化配置（只读非破坏）", {
    const auto path = test_support::tempConfigFile(kCfgPrefix, "appread");   // 本文件既有 helper（test_settings.cpp:276 同款）
    module::settings::SettingsStore store(path);
    module::settings::SettingsConfig cfg;                    // 默认值 + 改两处
    cfg.fontScalePercent = 110;
    cfg.proxyPort = 9999;
    DOCTEST_REQUIRE(store.save(cfg).isOk());
    auto tool = module::settings::makeAppReadSettingsTool(path);
    DOCTEST_CHECK_EQ(tool->name(), std::string("app.read_settings"));
    DOCTEST_CHECK_FALSE(tool->isDestructive());
    auto r = tool->execute(core::ToolArgs{"{}"});
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_FALSE(r.value().isError);
    auto parsed = platform::json::parse(r.value().contentJson);
    DOCTEST_REQUIRE(parsed.isOk());
    auto obj = parsed.value().asObject();
    DOCTEST_CHECK_EQ(obj.get("fontScalePercent").asInt(), 110);
    DOCTEST_CHECK_EQ(obj.get("proxyPort").asInt(), 9999);
    DOCTEST_CHECK(obj.get("themeMode").isString());
    return true;
})
```

- [ ] **Step 2: 红**（工厂不存在 → 编译失败）→ **Step 3: 实现**（app_tools.{h,cpp}：settings_config.h + core/tool.h + platform/json.h；错误消息组装照 markdown_tools.cpp:30-41 toolError 模式；settings_module 接线 + CMake）
- [ ] **Step 4: 绿**（定向 `devdesk_tests app_read_settings`）+ 全量 + `bash scripts/check-module-isolation.sh`（settings_module 新 include 只有其模块内头 + core，守卫 B 项必须仍绿）
- [ ] **Step 5: Commit** `git commit -m "feat(settings): app.read_settings 智能体工具（自持 SettingsStore 纯读）" -- src/module_settings/app_tools.h src/module_settings/app_tools.cpp src/module_settings/settings_module.cpp src/module_settings/CMakeLists.txt tests/test_modules/test_settings.cpp`

### Task 10: UI 任务编辑表单 + cron 校验 + 启用/禁用 + 常驻入队

**Files:**
- Create: `src/model_slots.h`（模型槽位 X 宏真源，header-only，src 根中立头——与 `src/module_pet_bridge.h` 同型）+ Modify: `src/module_claude_proxy/provider_config.h`（改为 include 它、删本地副本）+ `src/module_agent/agent_module.h`（编辑态成员）+ `src/module_agent/agent_module.cpp`
- Test: `tests/test_modules/test_agent_ui.cpp` 追加

**Interfaces:**
- Consumes: Task 2 updateTask/setTaskEnabled/getTask、Task 3 parseCron（保存期校验——spec §14 UI 行「cron 输入校验」）、Task 5 enqueueResident、`components::input`（M1 已用）/ `components::dropdown`（settings_ui_cards.cpp:137 用法：`.items(...) .selected(idx) .onChange(fn)`）/ `components::button`。
- Produces（元素 id 契约表，UI 测试逐字钉）：

| 元素 id | 出现条件 | 行为 |
|---|---|---|
| `agent.task.<id>.edit` | 任务行恒有 | 点击 → 打开该任务编辑表单 |
| `agent.task.<id>.toggle` | 任务行恒有 | 文案「禁用」/「启用」（按 enabled）→ setTaskEnabled 取反 |
| `agent.task.<id>.enqueue` | 仅 trigger_type=="resident" | 点击 → scheduler_->enqueueResident(id) |
| `agent.edit.form` 及子元素 | editingTaskId_ 非空 | `agent.edit.goal` / `.trigger`（下拉：手动/定时/常驻）/ `.cron`（trigger==定时才渲染）/ `.slot`（下拉）/ `.iter` / `.destructive` / `.save` / `.cancel` / `.error` |

- 模型槽位下拉键集：**真源迁到 src 根中立头 `src/model_slots.h`**（header-only）——把 provider_config.h:48-63 的 X 宏表 `DEVDESK_MODEL_SLOT_LIST(X)` 原样搬过去（**宏名与 6 参形态不变**），provider_config.h 改为 `#include "model_slots.h"` 并删掉本地那份；agent_module.cpp 同样 `#include "model_slots.h"`，在文件内展开键集：
  ```cpp
  #define DEVDESK_MODEL_SLOT_KEY(name, jsonKey, label, formLabel, envKey, nameKey) jsonKey,
  constexpr const char* kModelSlotKeys[] = { DEVDESK_MODEL_SLOT_LIST(DEVDESK_MODEL_SLOT_KEY) };
  #undef DEVDESK_MODEL_SLOT_KEY
  ```
  **一份真源、零漂移**（新增槽位只改 model_slots.h；不再有「两处同步」的复制件与漂移注释）。**零 CMake 改动**：src/ 已是 module_agent 与 module_claude_proxy 的 include 根（两模块 CMakeLists 均 `PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..`，provider_config.h 本就 `#include "core/result.h"` 走同一根），`#include "model_slots.h"` 直接命中；隔离守卫不受影响（src 根头不带兄弟模块斜杠前缀，C5 零命中）。
- 保存流程：trigger==定时 → `parseCron(cron)` 失败 → `agent.edit.error` 文案「cron 表达式非法：…」不落库；成功 → updateTask(...) + 退编辑态 + tasksDirty_。
- 编辑态成员：`std::optional<int64_t> editingTaskId_; std::string editGoal_, editCron_, editSlot_, editError_; int editTriggerIdx_ = 0, editIter_ = 10; bool editDestructive_ = false;`（打开时从 getTask 回填）。

- [ ] **Step 1: 写失败测试**（test_agent_ui.cpp 追加，照文件内 compose+find+onClick 形态与 FakeProxy 桩）：

```cpp
REGISTER_TEST(agent_ui_edit_form_and_cron_validation,
              "agent_ui: 编辑表单打开回填、非法 cron 拒存、合法 cron 落库、禁用切换、常驻入队按钮", {
    test_support::TestDb tdb("agentui", "edit");
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("旧任务", "旧目标", "manual", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());
    const std::string tid = std::to_string(taskId.value());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.proxy = &proxy;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Screen screen{1600.0f, 1000.0f};
    auto composeOnce = [&](eui::Ui& ui) { ui.begin("f"); mod.compose(ui, screen); ui.end(); };
    eui::Ui ui; composeOnce(ui);
    const auto* edit = ui.find("agent.task." + tid + ".edit");
    DOCTEST_REQUIRE(edit != nullptr);
    DOCTEST_REQUIRE(edit->onClick != nullptr);
    edit->onClick();
    eui::Ui ui2; composeOnce(ui2);
    DOCTEST_CHECK(ui2.find("agent.edit.form") != nullptr);
    DOCTEST_CHECK(ui2.find("agent.edit.goal") != nullptr);
    DOCTEST_CHECK(ui2.find("agent.edit.cron") == nullptr);   // manual 不渲染 cron 输入

    // 切触发类型到「定时」（下拉 onChange 回调直调）→ cron 输入出现；非法表达式拒存
    const auto* dd = ui2.find("agent.edit.trigger");
    DOCTEST_REQUIRE(dd != nullptr);
    // （按 compose 实现把 dropdown onChange 存到可直调的成员/捕获——以文件内
    //  FakeProxy 同款桩的接线为准；无法直调时经 ui2.find 的组件回调字段触发）
    callDropdownChange(ui2, "agent.edit.trigger", 1);        // helper：取回调直调 idx=1
    eui::Ui ui3; composeOnce(ui3);
    DOCTEST_CHECK(ui3.find("agent.edit.cron") != nullptr);
    setCronInput(ui3, "agent.edit.cron", "0 9 * * 1-5x");    // helper：写输入框草稿
    const auto* save = ui3.find("agent.edit.save");
    DOCTEST_REQUIRE(save != nullptr);
    save->onClick();
    eui::Ui ui4; composeOnce(ui4);
    const auto* err = ui4.find("agent.edit.error");
    DOCTEST_REQUIRE(err != nullptr);
    DOCTEST_CHECK(err->text.find("cron") != std::string::npos);
    auto untouched = store.getTask(taskId.value());
    DOCTEST_REQUIRE(untouched.isOk());
    DOCTEST_CHECK_EQ(untouched.value().goal, std::string("旧目标"));   // 未落库

    // 合法 cron → 落库 + 退编辑态
    setCronInput(ui4, "agent.edit.cron", "0 9 * * 1-5");
    ui4.find("agent.edit.save")->onClick();
    auto updated = store.getTask(taskId.value());
    DOCTEST_REQUIRE(updated.isOk());
    DOCTEST_CHECK_EQ(updated.value().triggerType, std::string("schedule"));
    DOCTEST_CHECK_EQ(updated.value().triggerConfig, std::string(R"({"cron":"0 9 * * 1-5"})"));
    // 启用/禁用切换
    eui::Ui ui5; composeOnce(ui5);
    const auto* toggle = ui5.find("agent.task." + tid + ".toggle");
    DOCTEST_REQUIRE(toggle != nullptr);
    DOCTEST_CHECK(toggle->text == "禁用");
    toggle->onClick();
    DOCTEST_CHECK_FALSE(store.getTask(taskId.value()).value().enabled);
    // 常驻任务才有入队按钮
    DOCTEST_CHECK(ui5.find("agent.task." + tid + ".enqueue") == nullptr);
    mod.onStop();
    return true;
})
```

（`callDropdownChange` / `setCronInput` 两个 helper 的可行形态：compose 把 dropdown 的 onChange 存入文件内可测的成员缝，或测试直接调模块的 `setEditTriggerIdx(int)` / `setEditCronDraft(std::string)` 私有→公有测试缝——实现时选侵入最小者，helper 落测试文件内；不引 EUI-NEO 真下拉事件机制。）
- [ ] **Step 2: 红**（edit 元素不存在 → REQUIRE 失败）→ **Step 3: 实现**（agent_module.cpp compose 任务行加 edit/toggle/enqueue 按钮 + 编辑表单分支；保存/取消/切换回调照 createTaskFromForm 模式；入队 onClick 内 `if (scheduler_) scheduler_->enqueueResident(id)`）
- [ ] **Step 3b: 任务详情只读展示**（详情区加一条只读信息：goal / cron 原文（schedule 任务）/ 模型槽位 / 迭代上限 / 破坏性标记；**与编辑表单同源渲染**——同一份 `AgentTaskRow`（`getTask`/`taskFromRow`）取字段，不另起第二套读法。run 列表/消息区在 Task 11 落位，本步只加这条信息行，不动 M1 的 detailMessages_ 区）
- [ ] **Step 4: 绿**（定向 `devdesk_tests agent_ui`，M1 契约用例零回归——注意 M1 的 `agent.task.<id>.run` 按钮与新增按钮共存不破坏 `.title`/`.status` 定位）+ 全量
- [ ] **Step 5: Commit** `git commit -m "feat(agent): 任务编辑表单 + cron 保存期校验 + 启用禁用 + 常驻入队 + 槽位真源中立头" -- src/model_slots.h src/module_claude_proxy/provider_config.h src/module_agent/agent_module.h src/module_agent/agent_module.cpp tests/test_modules/test_agent_ui.cpp`

### Task 11: UI run 详情完整展示 + probe 扩展

**Files:**
- Modify: `src/module_agent/agent_module.cpp`（展开详情区重写为 run 列表 + 选中 run 消息）
- Modify: `tests/probes/agent_probes.cpp`（新增 probe：编辑表单渲染 + run 列表几何）
- Test: `tests/test_modules/test_agent_ui.cpp` 追加

**Interfaces:**
- Consumes: `AgentStore::listRuns(taskId, 5)`（既有，含 status/iterations/inputTokens/outputTokens/error/startedAt/endedAt）、既有展开态 expandedTaskId_。
- Produces（元素 id 契约表）：

| 元素 id | 内容 |
|---|---|
| `agent.task.<id>.runlist` | 展开任务的最近 5 次 run 列表容器（空 run 时不渲染） |
| `agent.task.<id>.runlist.<i>` | 第 i 行：`status 中文化(参考 agent_module.cpp:37 runStatusText) / 耗时(endedAt-startedAt)s / 迭代 N / token in+out`（NULL token 显示「—」） |
| `agent.task.<id>.runlist.<i>.err` | error 非空时渲染错误说明行 |
| `agent.task.<id>.msg.<n>` | 选中 run 的消息序列（M1 既有 id 保留，消息源从「最近 run」改为「选中 run」） |

- 展开语义：点 runlist 行 → 选中该 run 加载其消息；默认选 runlist[0]（最近）。模块成员：`std::map<int64_t, std::vector<AgentRunRow>> taskRuns_; std::optional<int64_t> selectedRunId_;`（refreshData 装载）。

- [ ] **Step 1: 写失败测试**（test_agent_ui.cpp 追加）：

```cpp
REGISTER_TEST(agent_ui_run_detail_full_display,
              "agent_ui: 展开任务显示最近 5 次 run（状态/耗时/迭代/token/错误）并可选中查看消息", {
    test_support::TestDb tdb("agentui", "rundetail");
    FakeProxy proxy;
    ToolGuard statsGuard{"app.stats"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("详情", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());
    const std::string tid = std::to_string(taskId.value());
    // 种 2 次 run：一次 limited 带 error + token，一次 completed
    auto r1 = store.startRun(taskId.value());
    DOCTEST_REQUIRE(r1.isOk());
    DOCTEST_REQUIRE(store.appendMessage(r1.value(), 0, "user", R"([{"type":"text","text":"g"}])", std::nullopt).isOk());
    DOCTEST_REQUIRE(store.finishRun(r1.value(), "limited", 7, int64_t(190000), int64_t(15000),
                                    std::string("token 累计超上限 200K")).isOk());
    auto r2 = store.startRun(taskId.value());
    DOCTEST_REQUIRE(r2.isOk());
    DOCTEST_REQUIRE(store.finishRun(r2.value(), "completed", 1, std::nullopt, std::nullopt, std::nullopt).isOk());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.proxy = &proxy;
    module::agent::AgentModule mod(deps);
    mod.onStart();
    eui::Screen screen{1600.0f, 1000.0f};
    eui::Ui ui; ui.begin("f"); mod.compose(ui, screen); ui.end();
    ui.find("agent.task." + tid)->onClick();
    eui::Ui ui2; ui2.begin("f2"); mod.compose(ui2, screen); ui2.end();
    DOCTEST_REQUIRE(ui2.find("agent.task." + tid + ".runlist") != nullptr);
    const auto* row0 = ui2.find("agent.task." + tid + ".runlist.0");
    DOCTEST_REQUIRE(row0 != nullptr);                        // 最近一次在前
    DOCTEST_CHECK(row0->text.find("成功") != std::string::npos);   // completed 行
    const auto* row1 = ui2.find("agent.task." + tid + ".runlist.1");
    DOCTEST_REQUIRE(row1 != nullptr);
    DOCTEST_CHECK(row1->text.find("受限") != std::string::npos);   // limited 行
    DOCTEST_CHECK(row1->text.find("205000") != std::string::npos); // token 合计落进文案
    const auto* errRow = ui2.find("agent.task." + tid + ".runlist.1.err");
    DOCTEST_REQUIRE(errRow != nullptr);
    DOCTEST_CHECK(errRow->text.find("200K") != std::string::npos);
    // 点选 limited run → 消息区切换到该 run（r1 有 1 条 user 消息）
    row1->onClick();
    eui::Ui ui3; ui3.begin("f3"); mod.compose(ui3, screen); ui3.end();
    const auto* msg0 = ui3.find("agent.task." + tid + ".msg.0");
    DOCTEST_REQUIRE(msg0 != nullptr);
    DOCTEST_CHECK(msg0->text.find("user") != std::string::npos);
    mod.onStop();
    return true;
})
```

- [ ] **Step 2: 红**（runlist 不存在 → REQUIRE 失败）→ **Step 3: 实现**（compose 展开分支：detailMessages_ 区上方插 runlist 容器；refreshData 改装载 taskRuns_ + 默认选中最近 run；runStatusText/耗时/token 文案拼接全为既有数据字段）
- [ ] **Step 4: probe**（tests/probes/agent_probes.cpp 追加一枚 probe，照文件头注释 ①②③ 手搓 check() 口径）：种 schedule 任务 + 两次 run → composeFrame 后 `agent.task.<id>.edit` 存在、点击 edit → `agent.edit.form` 帧落在页面内容区（frameContains）、`agent.task.<id>.runlist` 行非零宽高；点击路径经 `core::queuePointerButton`（文件内既有 ③ 同款）；收尾照 about_probe.cpp:449-453。
- [ ] **Step 5: 跑**：定向 `devdesk_tests agent_ui` + `(cd build && ctest -R agent_probes --output-on-failure)` + 全量
- [ ] **Step 6: Commit** `git commit -m "feat(agent): run 详情完整展示（run 列表/token/错误）+ 真窗 probe" -- src/module_agent/agent_module.cpp tests/test_modules/test_agent_ui.cpp tests/probes/agent_probes.cpp`

### Task 12: 收尾（全量回归 + 守卫 + probe 套件 + 基线对比）

- [ ] **Step 1: 全量构建 + 测试**：`bash scripts/build-test-run.sh --no-run` → Failed: 0；`Total/Passed` 对比 Task 1 前记录的基线（新增用例数 = 本计划各测试文件追加数之和，只增不减）
- [ ] **Step 2: probe 套件**：`(cd build && ctest -L probe)` 全绿（含 agent_probes 既有 + Task 11 新增）
- [ ] **Step 3: 隔离守卫** `bash scripts/check-module-isolation.sh`（agent/markdown/settings/claude 四模块新 include 全零违规）
- [ ] **Step 4: 手动冒烟**（隔离 `DEVDESK_DATA_ROOT` 启动自己构建产物，**不碰 `/Applications/DevDesk.app`、禁 `pkill -f DevDesk`**）：设置页开 agent → 新建 schedule 任务（cron `*/2 * * * *`）→ 观察状态列到点变化 → 编辑任务改 cron（先填非法值看报错）→ 建常驻任务点入队 → run 详情看 token/错误列；启动代理前点「启动」→ run 落 failed「代理未启」（spec §4.2 预检既有路径在新触发下仍成立）
- [ ] **Step 5: 文档口径**：`agent_loop.cpp:187-190` 的「墙钟 M1 未实现」注释已在 Task 1 改写；**spec §5.2 同步一句**——`docs/superpowers/specs/2026-09-30-agent-design.md:394` 的「两硬上限由 M2 Scheduler 强制」改为「由 AgentLoop 检查点强制（Scheduler 等触发路径经 loop 组合传递全覆盖）」（检查点长在 AgentLoop 里，Scheduler 只是触发方之一；旧措辞会让人去 Scheduler 里找上限判定）；`docs/superpowers/plans/` 本计划文件勾选核对；`CLAUDE.md`/`TODO.md` 如有 agent M1 进度段落则补 M2 一行（grep 核实，无则不加）
- [ ] **Step 6: 汇报 Total/Passed 对比基线 + 遗留清单**

## Self-Review 记录

- **Spec 覆盖**：§14 M2 四行全落——调度（T3/T4/T5）、工具 8 个（T7 三个 + T8 四个 + T9 一个）、UI（T10/T11）、测试（cron 表驱动 T3 / Scheduler 注入时钟 T4 / 常驻队列 T5 / M2 各工具 T7-T9）；§5.2 资源上限（T1，M1 实况标注「由 M2 强制」接棒）；§5.1 cron 语法/同分钟防重/不补跑/时钟注入/存储格式（T3/T4）；§5.3 破坏性（T7 md.delete / T8 switch_profile 覆写 isDestructive，复用 M1 loop 判定）；§10 线程模型（T8 锁落点 plan 裁定 + T4/5/6 teardown 顺序）；§15 文件归属（每工具集独立文件、Scheduler 独立文件）。
- **占位符扫描**：T10 dropdown 回调测试缝「选侵入最小者」是显式授权的实现期核实点（锚点不足处已注明依据文件:行），非 TBD；其余全部有代码或精确命令（T8 `createProfile` 实参已按 claude_service.h:83 写死，不再是核实点）。
- **已知取舍（controller 裁决点）**：① 模型槽位键集**不再是复制件**——X 宏真源迁到 src 根中立头 `src/model_slots.h`（`DEVDESK_MODEL_SLOT_LIST` 宏名不变），provider_config.h 与 module_agent 共用一份、结构上无法漂移；隔离铁律仍不破（agent 不 include 兄弟模块头，include 的是 src 根中立头，与 module_pet_bridge.h 同型）；② 墙钟预算的「请求超时」因子取 spec 默认 60s 常量（用户可配请求超时不在 M2 spec 范围）；③ 日/周双受限取 vixie-cron「或」语义（标准 5 字段语义的一部分，T3 用例钉死）；④ 「同 task 不并发」是**单闸**——落点在 `AgentLoop::run`（agent_loop.cpp:126 换调 `store_.startRunExclusive`，签名/返回值语义不变），`Scheduler::dispatchRun` 只是薄 spawn 包装：不预插行、不自己查闸、runId 取自 `loop_.run` 的返回；手动与 cron 两路共用同一处闸，跨表双跑在结构上不可能（旧「两路各自预查再插」案才是竞态源）。
