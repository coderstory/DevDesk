# MCP 与智能体模块改进实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在实地核对后的缺口清单上，先落地最高风险项——MCP 破坏性工具服务端闸 + 在途并发上限，并给出其余缺口可独立立项的分批路线。

**Architecture:** 破坏性语义在 core 接缝（`IMcpTool`）上新增默认 false 的只读属性，由镜像适配器透传源 `ITool::isDestructive()`；协议层在 `tools/call` 入口按「允许开关 + 工具属性」先拒后不调；在途并发用进程级原子计数，配额随 worker 生命周期释放（含超时 detach 路径）。开关持久化进 `SettingsStore`，经 app 层函数指针桥（既有 `mcp_service_bridge` 模式）读写，模块间零互引。

**Tech Stack:** C++20 / CMake / 自制迷你 doctest（`REGISTER_TEST` + `DOCTEST_*`）/ EUI-NEO DSL / SQLite app_config / cpp-httplib（MCP 传输层）。

**Spec:**
- 原始审计：桌面 `新建 文本文档 (2).txt`（2026-11-26，MCP 与智能体模块审计报告）
- 权威设计：`docs/superpowers/specs/2026-10-26-mcp-server-skeleton-design.md`（§14.2 已知风险）、`docs/superpowers/specs/2026-09-30-agent-design.md`、`docs/superpowers/design/2026-11-19-mcp-agent-tools-ui.md`
- 本计划已按下节「实地核对」修正原报告，执行者以本计划为准。

## Global Constraints

- 构建/测试唯一入口：`bash scripts/build-test-run.sh --dev --filter <名字>`；全量 `doctest+probe+守卫链` 只在收口跑一次。禁止手敲 cmake。
- 不抛 C++ 异常，错误统一 `Result<T>`；新代码不得引入 `#ifdef _WIN32/__APPLE__`（平台差异拆文件）。
- `module_*` 之间禁止互相依赖；`core` 不依赖 `platform`、不调 EUI-NEO 运行时；模块层只经 app 层函数指针桥跨模块。
- 测试是自制迷你 doctest：仅 `REGISTER_TEST` / `DOCTEST_CHECK(_EQ/_NE/_TRUE/_FALSE/_MESSAGE)` / `DOCTEST_REQUIRE*` / `DOCTEST_SKIP`，**无 `CHECK_LE`**；bool 用例体 `return true;` 收尾。
- UI 改动必须真窗口 probe 几何断言 + 截图（`--screenshot` 进程内捕获）才算完成；单测 mock 绿不算收口。
- 每个任务先红后绿：测试先写、先跑红，再实现；长任务每环节 `wip:` 临时提交，收尾 squash 成单个 `feat:`/`fix:`。
- 报告为只读审计，未入池。执行第一步先按 devflow 把「A-1+A-2」入池建卡，再动代码。

## Review Focus

- MCP 客户端在开关关闭时调 `fs_write`/`bash_run`：必须得到 `isError:true` + 可操作中文原因，工具一次都不执行。
- Claude Code 并发发多个 `tools/call`：超出上限的请求必须立刻收到 `-32603`（不排队、不占线程），且槽位在完成后归零。
- 工具挂死触发 600s 总闸 detach：槽位只可在 worker 真正退出时释放；若在请求线程释放，上限形同虚设。
- 服务改端口/重启：开关状态必须延续，不能重启后悄悄回到「允许」。
- 开关写盘失败（库忙/只读）：开关必须回弹并在行内红字说明，不能显示已开启。

---

## 0. 实地核对结论（对原报告的修正）

核对基线：本机 `dev @ eba4b37c`（领先 `origin/dev` 39 提交、落后 0；`git submodule update --remote` 后 EUI-NEO 在 fork `main` 最新）。

| 原报告结论 | 实地结果 | 证据 |
|---|---|---|
| 基线 dev @ 6790eb35 | **本仓无此提交**（`git rev-parse` 失败）。报告应产自另一台机/另一条线，行号有漂移 | `git log --all` 无此对象 |
| 池台账 4 条脱节（#405/#423/#399/#478） | **Windows 池同号异义**：`docs/pool/pool.db` 里 399=记事本预览空白（已验收）、405=todo 首帧（已验收）、423=波末验证批（已否决）、478=EUI readOnly（已验收）。报告所指的 Mac 池条目在本库的对应物是 **#279/#394/#170/#416，全部已验收关闭** | `needs` 按 id 直查 + 标题关键词反查 |
| 模块体量 5475 / 14443 行 | 量级一致，有提交演进漂移：本机 `module_mcp` 5446 行、`module_agent` 14589 行（.cpp+.h） | `glob` 行数统计 |
| A-1 破坏性工具无服务端闸 | **属实且未缓解**：`tools/call` 直接 `tool->execute(argumentsJson)`，无任何 `isDestructive` 判定 | `src/module_mcp/mcp_protocol.cpp:300-382`；`agent_loop.cpp:314` 有闸，MCP 没有 |
| A-2 无在途并发上限 | **属实**：每次 call 起一条线程，仅超时 detach 计数，无全局配额 | `mcp_protocol.cpp:25-28,317-362`（注释自承「后续裁决」） |
| A-3 `~/.claude.json` 读改写无锁 | **属实**：读→改→备份→原子写，无锁、无 mtime 校验 | `src/module_mcp/mcp_config_common.cpp:16-47` |
| B-1 LLM 零重试 | **属实**：单次 `http::send` 失败即返回 fail，无重试/退避 | `direct_llm_client.cpp:289-302`；`agent_loop.cpp:225-229` |
| B-2 历史无保留策略 | **属实**：仅 `deleteTask` 级联清理三表 | `agent_store.cpp:254-300` |
| B-3 无按任务工具白名单 | **属实**：每轮全量 `tools_.allSchemasJson()` | `agent_loop.cpp:190` |
| B-4 运行记录只取 20 条无分页 | **属实** | `agent_ui_shared.h:24` |
| C-1 MCP 工具网格无搜索 | **属实**：页内「搜索」字样 0 处，仅 scrollView | `mcp_page_module.cpp` 全文计数 |
| C-2 MCP 无调用日志 | **属实**：全仓无 callCount/invokeCount/lastCall | `mcp_protocol.cpp` 仅超时 LOG_WARN |
| C-3 卡片 chip 硬编码 | **属实**：「内置」「服务」两 chip 对每卡无条件渲染 | `mcp_page_module.cpp:628-657` |
| 43 工具（39 镜像 + 4 直注） | **属实**，probe 有硬断言 | `tests/probes/mcp_e2e_probe.cpp:252` |
| 7 件破坏性工具 | **属实**：`isDestructive` 覆写 7 处（agent_builtin 4 / claude_tools 1 / markdown 1 / todo 1） | `git grep isDestructive` |

报告未写、实地补充的三条缓和/前提：

1. **MCP 默认关闭**：`mcp` 是默认关闭模块，A-1 风险仅在用户主动开启服务后存在（`settings_config.cpp:134`）。
2. **会话门禁存在**：非 `initialize` 请求缺 `Mcp-Session-Id` 一律 400，客户端必须先握手（`mcp_protocol.cpp:175-177`）。这挡不住恶意客户端，但把风险从「任意网页」收窄到「已握手的本机客户端」。
3. **原有的 `abandonedToolWorkers()` 观测面可直接复用/扩展**，不必另造指标。

---

## 1. 改进路线

| 批次 | 项目 | 级别 | 工作量 | 为什么先做/后做 |
|---|---|---|---|---|
| **第一批** | A-1 破坏性工具闸 + A-2 在途并发上限 | P0/P1 | S+M（约 370 行 + 260 行测试） | 唯一「数据可能被误删/误改」的入口；同文件域，一次改动闭环 |
| **第二批** | A-3 配置写 mtime 校验；B-1 LLM 重试/退避 | P1 | S+S（40 + 60 行） | 都是低成本高收益的稳定性修复 |
| **第三批** | C-2 MCP 调用日志；B-2 历史保留策略 | P1/P2 | M+M | 可观测性与长期体积，需先定保留参数 |
| **暂缓** | B-3 工具白名单；B-4 run 分页；C-1 网格搜索；C-3 来源 chip | P2/P3 | 各自 M/S | 43 件规模下痛感未到；数据面先观察 |
| **需决策** | D-1 对话式入口；D-2 MCP client | L/L+ | 先出 PRD | 产品形态级空白，不是补缺 |

> 笔记 / 待办两份报告的实地复核与第一批小修见 `docs/superpowers/plans/2026-11-26-markdown-todo-audit-review.md`。结论：本机 Windows 池无需核销动作；待办 A-1/A-4 是紧随本批之后最值得做的两个 S 级修复。

**需要老板拍板的 3 件事**

1. **A-1 粒度**：总开关（推荐）还是按 7 件逐个授权？推荐总开关——按粒度会变成第二套开关矩阵，违反减法原则。
2. **默认值**：推荐默认关；开启后 toast 文案明示「MCP 客户端可写文件/执行命令」。已装 Claude Code 的用户升级后会先被拒一次，这是有意为之。
3. **D-1 / D-2**：是否各出一页 PRD 立项？（D-2 即「智能体调用外部 MCP server」，价值面比「把自己 43 个工具开放出去」更宽。）

**第一批五维预算**

| 维度 | 预算 |
|---|---|
| 功能代码量 | ≤ 400 行（生产）；测试 ≤ 300 行 |
| 注释量 | ≤ 50 行，只写 why/契约，不复读 what |
| 执行周期 | 2–3 个工作日（含 probe 与真机走查） |
| 内存 | 常驻 +0（2 个 atomic + 1 个 bool）；峰值线程数从无界收敛到 ≤ 8 |
| CPU | 每 call 两次原子读写，< 1µs；每帧开关读取一次函数指针，可忽略 |
| 二进制 | +约 4KB |

---

## 2. 第一批实施计划：破坏性工具闸 + 在途并发上限

**文件结构**

| 文件 | 职责 |
|---|---|
| `src/core/mcp_tool.h` | 新增 `IMcpTool::isDestructive()` 默认 false（接缝） |
| `src/module_mcp/mcp_tool_mirror.{h,cpp}` | `AgentToolAdapter` 透传源工具破坏性 |
| `src/module_mcp/mcp_protocol.{h,cpp}` | 开关状态、拒绝分支、并发配额与槽位释放、观测计数 |
| `src/module_mcp/mcp_service.{h,cpp}` | 持有开关期望值，start 时注入 protocol |
| `src/module_settings/settings_config.{h,cpp}` | 新键 `mcpAllowDestructive` 默认 false |
| `src/mcp_service_bridge.{h,cpp}` | 开关 get/set 函数指针缝（沿用既有模式） |
| `src/main.cpp` | 实现桥：读/存 SettingsStore + 转发 service |
| `src/module_mcp/mcp_page_module.{h,cpp}` | 页头第二行「破坏性工具」开关（复用记忆行样式） |
| `tests/test_modules/test_mcp_protocol.cpp` | 闸 + 并发用例 |
| `tests/test_modules/test_mcp_tool_mirror.cpp` | 适配层透传用例 |
| `tests/test_modules/test_mcp_settings.cpp` | 键数 27 + 桥默认桩用例 |
| `tests/test_modules/test_mcp_page.cpp` | 新行几何/交互用例 |
| `tests/probes/mcp_e2e_probe.cpp` | 真 HTTP 往返：拒 → 开 → 放行 |

### Task 1: core 接缝与镜像透传

**Files:**
- Modify: `src/core/mcp_tool.h:25-34`
- Modify: `src/module_mcp/mcp_tool_mirror.h:20-40`
- Modify: `src/module_mcp/mcp_tool_mirror.cpp:92-97`
- Test: `tests/test_modules/test_mcp_tool_mirror.cpp`

**Interfaces:**
- Produces: `bool core::mcp::IMcpTool::isDestructive() const`（默认 `false`）
- Produces: `bool module::mcp::AgentToolAdapter::isDestructive() const`（转发 `source_->isDestructive()`）

- [ ] **Step 1: 写失败用例**

在 `test_mcp_tool_mirror.cpp` 的匿名 namespace 后追加（`FakeITool` 已支持 `destructive` 构造参数）：

```cpp
REGISTER_TEST(mcp_tool_mirror_destructive_flag,
              "mcp tool mirror: 适配层 isDestructive 透传源工具（破坏性 true / 只读 false）", {
    auto danger = std::make_shared<FakeITool>("mirror.danger", kGoodSchema, true);
    auto safe = std::make_shared<FakeITool>("mirror.safe", kGoodSchema, false);
    module::mcp::AgentToolAdapter dangerAdapter(danger);
    module::mcp::AgentToolAdapter safeAdapter(safe);
    DOCTEST_REQUIRE(dangerAdapter.valid());
    DOCTEST_REQUIRE(safeAdapter.valid());
    DOCTEST_CHECK_TRUE(dangerAdapter.isDestructive());
    DOCTEST_CHECK_FALSE(safeAdapter.isDestructive());
    return true;
})
```

- [ ] **Step 2: 跑红**

Run: `bash scripts/build-test-run.sh --dev --filter mcp_tool_mirror_destructive_flag`
Expected: 编译失败（`isDestructive` 不是 `IMcpTool` 成员），即红。

- [ ] **Step 3: 加接缝与透传**

`core/mcp_tool.h` 在 `execute` 前加：

```cpp
    // 有不可逆副作用的工具覆写返回 true；AgentLoop 走 allowDestructive，
    // MCP 协议层走页头开关——两侧都靠它做「先拒后不调」。
    virtual bool isDestructive() const { return false; }
```

`mcp_tool_mirror.h` 在 `AgentToolAdapter` 类内加声明，`mcp_tool_mirror.cpp` 在 `annotationsSuffix()` 前加：

```cpp
bool AgentToolAdapter::isDestructive() const { return source_->isDestructive(); }
```

- [ ] **Step 4: 跑绿**

Run: `bash scripts/build-test-run.sh --dev --filter mcp_tool_mirror`
Expected: 全部 PASS（含既有 annotations 用例）。

- [ ] **Step 5: 提交**

```bash
git add src/core/mcp_tool.h src/module_mcp/mcp_tool_mirror.h src/module_mcp/mcp_tool_mirror.cpp tests/test_modules/test_mcp_tool_mirror.cpp
git commit -m "wip: mcp 接缝加 isDestructive 并由镜像适配器透传"
```

### Task 2: 协议层闸（默认拒）

**Files:**
- Modify: `src/module_mcp/mcp_protocol.h:23-40`（`McpProtocol` 公有段）
- Modify: `src/module_mcp/mcp_protocol.cpp`（`tools/call` 分支 + 匿名 namespace 的结果拼装）
- Test: `tests/test_modules/test_mcp_protocol.cpp`

**Interfaces:**
- Produces: `void module::mcp::McpProtocol::setAllowDestructive(bool allow)`；`bool allowDestructive() const`
- 拒绝语义：HTTP 200 + JSON-RPC `result`，其中 `isError:true`（工具级错误，不是 `-32603`）。
- 头文件需补 `#include <atomic>`（成员用 `std::atomic<bool>`）。

- [ ] **Step 1: 扩展测试桩并写失败用例**

`StubTool` 构造加第 4 个默认参数并覆写属性：

```cpp
    StubTool(std::string name, std::string text, bool isError = false,
             bool destructive = false)
        : name_(std::move(name)), text_(std::move(text)),
          isError_(isError), destructive_(destructive) {}
    bool isDestructive() const override { return destructive_; }
    // private: bool destructive_ = false;
```

追加三条用例：

```cpp
REGISTER_TEST(mcp_protocol_destructive_gate_blocks_by_default,
              "mcp protocol: 破坏性工具默认拒——isError=true、工具零执行、文案含未授权", {
    Fixture fixture;
    auto tool = std::make_shared<StubTool>("danger_tool", "should-not-run", false, true);
    DOCTEST_REQUIRE(fixture.tools.registerTool(tool).isOk());
    const std::string sid = fixture.openSession();
    std::string ignored;
    auto outcome = fixture.protocol.handlePost(
        R"({"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"danger_tool","arguments":{}}})",
        sid, "", ignored);
    DOCTEST_REQUIRE_EQ(outcome.status, 200);
    const auto root = parseBody(outcome.body);
    DOCTEST_REQUIRE(root.isObject());
    const auto result = root.asObject().get("result");
    DOCTEST_REQUIRE(result.isObject());
    DOCTEST_CHECK_TRUE(boolField(result.asObject(), "isError"));
    const auto content = result.asObject().get("content");
    DOCTEST_REQUIRE(content.isArray());
    const auto first = content.asArray().at(0);
    DOCTEST_REQUIRE(first.isObject());
    DOCTEST_CHECK(strField(first.asObject(), "text").find("未授权") != std::string::npos);
    DOCTEST_CHECK_EQ(tool->calls, 0);
    return true;
})

REGISTER_TEST(mcp_protocol_destructive_gate_allows_when_enabled,
              "mcp protocol: 开关打开后破坏性工具正常执行", {
    Fixture fixture;
    auto tool = std::make_shared<StubTool>("danger_tool", "ran", false, true);
    DOCTEST_REQUIRE(fixture.tools.registerTool(tool).isOk());
    fixture.protocol.setAllowDestructive(true);
    const std::string sid = fixture.openSession();
    std::string ignored;
    auto outcome = fixture.protocol.handlePost(
        R"({"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"danger_tool","arguments":{}}})",
        sid, "", ignored);
    DOCTEST_REQUIRE_EQ(outcome.status, 200);
    DOCTEST_CHECK_EQ(tool->calls, 1);
    DOCTEST_CHECK(outcome.body.find("ran") != std::string::npos);
    return true;
})

REGISTER_TEST(mcp_protocol_readonly_tool_unaffected_by_gate,
              "mcp protocol: 只读工具在开关关闭时照常执行", {
    Fixture fixture;
    auto tool = std::make_shared<StubTool>("safe_tool", "ok");
    DOCTEST_REQUIRE(fixture.tools.registerTool(tool).isOk());
    const std::string sid = fixture.openSession();
    std::string ignored;
    auto outcome = fixture.protocol.handlePost(
        R"({"jsonrpc":"2.0","id":11,"method":"tools/call","params":{"name":"safe_tool","arguments":{}}})",
        sid, "", ignored);
    DOCTEST_REQUIRE_EQ(outcome.status, 200);
    DOCTEST_CHECK_EQ(tool->calls, 1);
    DOCTEST_CHECK(outcome.body.find("isError") == std::string::npos);
    return true;
})
```

- [ ] **Step 2: 跑红**

Run: `bash scripts/build-test-run.sh --dev --filter mcp_protocol_destructive`
Expected: 编译失败（`setAllowDestructive` 不存在）。

- [ ] **Step 3: 实现闸**

`mcp_protocol.h` 公有段加：

```cpp
    // 页头「破坏性工具」开关的实时值；默认 false = 破坏性工具一律拒。
    // 原子：UI 线程写，HTTP worker 读。
    void setAllowDestructive(bool allow);
    bool allowDestructive() const;
```

私有成员加 `std::atomic<bool> allowDestructive_{false};`。

`mcp_protocol.cpp` 匿名 namespace 抽出结果拼装（现有 372-375 行逻辑原样搬入），供正常与拒绝两处复用：

```cpp
std::string toolResultJson(const core::mcp::ToolResult& tr) {
    std::string content = "{\"content\":[{\"type\":\"text\",\"text\":" +
                          quoteString(tr.text) + "}]";
    if (tr.isError) content += ",\"isError\":true";
    content += "}";
    return content;
}
```

在 `found` 校验通过、`const std::shared_ptr<...> tool = found.value();` 之后、创建 promise/线程之前插入：

```cpp
        if (tool->isDestructive() &&
            !allowDestructive_.load(std::memory_order_relaxed)) {
            // 工具级拒绝：让客户端看到「可以开，但现在没开」，而不是协议错误
            const core::mcp::ToolResult refused{
                true, "破坏性工具 " + toolName +
                          " 未授权：MCP 页「破坏性工具」开关为关闭状态"};
            auto refusedJson = parseFragment(toolResultJson(refused));
            if (refusedJson.isFail() || !refusedJson.value().isObject()) {
                return HttpOutcome{200, errorResponseText(id, -32603,
                                                          "拒绝结果构造失败"),
                                   false};
            }
            return HttpOutcome{200,
                               successResponseText(id, refusedJson.value()), false};
        }
```

尾部正常路径改用 `parseFragment(toolResultJson(toolResult))`，删除就地拼接。公有实现：

```cpp
void McpProtocol::setAllowDestructive(bool allow) {
    allowDestructive_.store(allow, std::memory_order_relaxed);
}
bool McpProtocol::allowDestructive() const {
    return allowDestructive_.load(std::memory_order_relaxed);
}
```

- [ ] **Step 4: 跑绿**

Run: `bash scripts/build-test-run.sh --dev --filter mcp_protocol`
Expected: 新老用例全 PASS。

- [ ] **Step 5: 提交**

```bash
git commit -m "wip: mcp tools/call 破坏性工具服务端闸（默认拒）"
```

### Task 3: 在途并发上限与槽位生命周期

**Files:**
- Modify: `src/module_mcp/mcp_protocol.h:40-48`
- Modify: `src/module_mcp/mcp_protocol.cpp:25-72, 300-362`
- Test: `tests/test_modules/test_mcp_protocol.cpp`

**Interfaces:**
- Produces: `int module::mcp::inflightToolCalls()`（与 `abandonedToolWorkers()` 同风格，供测试/probe）
- Env：`MCP_MAX_CONCURRENT_CALLS`（正整数；非法/≤0 回退 8）
- 超限响应：`-32603`，message 含「并发已达上限」。

- [ ] **Step 1: 写失败用例**

在测试文件加 `#include <future>` 与阻塞桩：

```cpp
// 进 execute 后阻塞，直到 release()；用于确定性占住并发槽位
class BlockingTool : public core::mcp::IMcpTool {
public:
    std::string name() const override { return "block_tool"; }
    std::string description() const override { return "blocks until released"; }
    std::string inputSchemaJson() const override {
        return R"({"type":"object","properties":{}})";
    }
    core::Result<core::mcp::ToolResult> execute(const std::string&) override {
        ++calls;
        entered_.set_value();
        gate_.wait();
        return core::Result<core::mcp::ToolResult>::ok(
            core::mcp::ToolResult{false, "released"});
    }
    bool waitEntered() {
        return entered_.get_future().wait_for(std::chrono::seconds(2)) ==
               std::future_status::ready;
    }
    void release() { release_.set_value(); }
    std::atomic<int> calls{0};

private:
    std::promise<void> entered_;
    std::promise<void> release_;
    std::shared_future<void> gate_ = release_.get_future();
};
```

```cpp
REGISTER_TEST(mcp_protocol_concurrency_cap_rejects_and_recovers,
              "mcp protocol: 并发上限=1 时第二个 call 立刻 -32603，释放后计数归零", {
    test_support::ScopedEnv env("MCP_MAX_CONCURRENT_CALLS", "1");
    Fixture fixture;
    auto tool = std::make_shared<BlockingTool>();
    DOCTEST_REQUIRE(fixture.tools.registerTool(tool).isOk());
    const std::string sid = fixture.openSession();
    const std::string call =
        R"({"jsonrpc":"2.0","id":12,"method":"tools/call","params":{"name":"block_tool","arguments":{}}})";

    std::string ignoredFirst;
    std::string ignoredSecond;
    std::thread first(
        [&] { fixture.protocol.handlePost(call, sid, "", ignoredFirst); });
    DOCTEST_REQUIRE(tool->waitEntered());
    DOCTEST_CHECK_EQ(module::mcp::inflightToolCalls(), 1);

    auto second = fixture.protocol.handlePost(call, sid, "", ignoredSecond);
    DOCTEST_CHECK_EQ(errorCodeOf(second.body), -32603);
    DOCTEST_CHECK(second.body.find("并发已达上限") != std::string::npos);
    DOCTEST_CHECK_EQ(tool->calls.load(), 1);  // 第二个没有执行

    tool->release();
    first.join();
    DOCTEST_CHECK_EQ(module::mcp::inflightToolCalls(), 0);
    DOCTEST_CHECK_EQ(tool->calls.load(), 1);
    return true;
})
```

- [ ] **Step 2: 跑红**

Run: `bash scripts/build-test-run.sh --dev --filter mcp_protocol_concurrency`
Expected: 编译失败（`inflightToolCalls` 不存在）。

- [ ] **Step 3: 实现配额**

`mcp_protocol.cpp` 匿名 namespace 顶部：

```cpp
constexpr int kDefaultMaxConcurrentToolCalls = 8;
std::atomic<int> g_inflightToolCalls{0};

// 上限覆盖：env MCP_MAX_CONCURRENT_CALLS；非法/≤0 回退默认
int maxConcurrentToolCalls() {
    const char* raw = std::getenv("MCP_MAX_CONCURRENT_CALLS");
    if (raw == nullptr || *raw == '\0') return kDefaultMaxConcurrentToolCalls;
    char* parseEnd = nullptr;
    const long parsed = std::strtol(raw, &parseEnd, 10);
    if (parseEnd == raw || *parseEnd != '\0' || parsed <= 0 || parsed > 1024) {
        return kDefaultMaxConcurrentToolCalls;
    }
    return static_cast<int>(parsed);
}

bool tryAcquireToolCallSlot(int limit) {
    int current = g_inflightToolCalls.load(std::memory_order_relaxed);
    while (current < limit) {
        if (g_inflightToolCalls.compare_exchange_weak(
                current, current + 1, std::memory_order_acquire,
                std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

// 槽位随 worker 生命周期释放：超时 detach 后仍由 worker 析构归还，
// 不能在请求线程归还（否则上限对 detach 路径失效）。
struct ToolCallSlotRelease {
    ~ToolCallSlotRelease() {
        g_inflightToolCalls.fetch_sub(1, std::memory_order_relaxed);
    }
};
```

`inflightToolCalls()` 放进公有命名空间（同 `abandonedToolWorkers()`），并在头文件声明。

`tools/call` 在闸之后：

```cpp
        if (!tryAcquireToolCallSlot(maxConcurrentToolCalls())) {
            return HttpOutcome{200,
                               errorResponseText(id, -32603,
                                                 "并发已达上限: " + toolName),
                               false};
        }
        auto slotRelease = std::make_shared<ToolCallSlotRelease>();
```

worker lambda 捕获 `slotRelease`；线程创建失败分支显式 `slotRelease.reset();` 再返回错误。超时 detach 路径**不要** reset（worker 会归还）。

- [ ] **Step 4: 跑绿 + detach 归零用例**

再补一条超时 detach 用例（Review Focus 第 3 条必须有测试钉住）：

```cpp
REGISTER_TEST(mcp_protocol_concurrency_slot_held_until_detached_worker_exits,
              "mcp protocol: 超时 detach 后槽位仍被占，worker 真正退出才归零", {
    test_support::ScopedEnv env("MCP_TOOL_TIMEOUT_SECONDS", "1");
    Fixture fixture;
    auto tool = std::make_shared<BlockingTool>();
    DOCTEST_REQUIRE(fixture.tools.registerTool(tool).isOk());
    const std::string sid = fixture.openSession();
    std::string ignored;
    auto outcome = fixture.protocol.handlePost(
        R"({"jsonrpc":"2.0","id":13,"method":"tools/call","params":{"name":"block_tool","arguments":{}}})",
        sid, "", ignored);
    DOCTEST_CHECK_EQ(errorCodeOf(outcome.body), -32603);
    DOCTEST_CHECK_EQ(module::mcp::inflightToolCalls(), 1);  // detach 不归还
    tool->release();
    // worker 已脱离，只能轮询到归零（上限 3s，避免偶发慢机假红）
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (module::mcp::inflightToolCalls() != 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    DOCTEST_CHECK_EQ(module::mcp::inflightToolCalls(), 0);
    return true;
})
```

Run: `bash scripts/build-test-run.sh --dev --filter mcp_protocol`
Expected: 全部 PASS；`mcp_protocol_tool_timeout_...` 既有两条仍绿。

- [ ] **Step 5: 提交**

```bash
git commit -m "wip: mcp tools/call 在途并发上限（默认 8）与槽位生命周期"
```

### Task 4: 开关持久化与 app 桥

**Files:**
- Modify: `src/module_settings/settings_config.h:110-170`
- Modify: `src/module_settings/settings_config.cpp:58-70, 520-570, 740-760`
- Modify: `src/mcp_service_bridge.h:14-49`
- Modify: `src/mcp_service_bridge.cpp`
- Modify: `src/main.cpp:300-330, 630-645`
- Test: `tests/test_modules/test_mcp_settings.cpp:198-281`
- Test: `tests/test_modules/test_mcp_service.cpp`（新增：重启后开关延续）

**Interfaces:**
- `SettingsConfig::mcpAllowDestructive`（bool，默认 false），键名 `mcpAllowDestructive`
- `app::mcp::allowDestructive() -> bool`；`app::mcp::setAllowDestructive(bool) -> core::Result<void>`
- 默认桩：getter 返回 false；setter fail「MCP 服务未装配」

- [ ] **Step 1: 写失败用例**

`test_mcp_settings.cpp` 键数断言从 26 改 27，并加往返断言：

```cpp
    cfg.mcpAllowDestructive = true;
    ...
    DOCTEST_CHECK_TRUE(loaded.mcpAllowDestructive);
    DOCTEST_CHECK_EQ(rows.value().size(), static_cast<size_t>(27));
```

新桥用例：

```cpp
// 测试文件顶部（与 mcpRecStart 同区）：自由函数 + 全局状态
bool g_allowDestructive = false;
bool allowDestructiveRec() { return g_allowDestructive; }
core::Result<void> setAllowDestructiveRec(bool on) {
    g_allowDestructive = on;
    return core::Result<void>::ok();
}
struct DestructiveHandlerGuard {
    DestructiveHandlerGuard() {
        app::mcp::setDestructiveHandlers(&allowDestructiveRec,
                                         &setAllowDestructiveRec);
    }
    ~DestructiveHandlerGuard() {
        app::mcp::setDestructiveHandlers(nullptr, nullptr);
    }
};

REGISTER_TEST(mcp_bridge_allow_destructive_default_and_roundtrip,
              "mcp: 破坏性开关桥默认 false/fail-closed；注入后读写路由", {
    app::mcp::setDestructiveHandlers(nullptr, nullptr);  // 出厂态显式化
    DOCTEST_CHECK_FALSE(app::mcp::allowDestructive());
    DOCTEST_CHECK(app::mcp::setAllowDestructive(true).isFail());
    {
        DestructiveHandlerGuard guard;
        DOCTEST_REQUIRE(app::mcp::setAllowDestructive(true).isOk());
        DOCTEST_CHECK_TRUE(app::mcp::allowDestructive());
    }
    DOCTEST_CHECK_FALSE(app::mcp::allowDestructive());  // 桩复位后回默认
    return true;
})
```

- [ ] **Step 2: 跑红**

Run: `bash scripts/build-test-run.sh --dev --filter mcp_bridge_allow_destructive`
Expected: 编译失败。

- [ ] **Step 3: 实现**

`settings_config.h`：加 `bool mcpAllowDestructive = false;`，注释「MCP 页「破坏性工具」开关；默认关 = 服务端一律拒」。

`settings_config.cpp`：加键 `constexpr const char* kKeyMcpAllowDestructive = "mcpAllowDestructive";`；load 段照 `mdAssocEnabled` 写法：

```cpp
    if (auto raw = readRow(kKeyMcpAllowDestructive); raw.isOk()) {
        auto v = parseRow(configKey(kKeyMcpAllowDestructive), raw.value());
        if (v.isOk() && v.value().isBool()) {
            cfg.mcpAllowDestructive = v.value().asBool();
        }
    }
```

save 的 rows 表加 `{kKeyMcpAllowDestructive, platform::json::makeBool(cfg.mcpAllowDestructive)}`；同步 `save` 注释里的键数（现注释停在 24、实际断言 26，本轮统一改 27）。

`mcp_service_bridge.h/.cpp`：**不改** 既有 `setHandlers` 四参签名，另开一对槽（避免所有调用点改签名）：

```cpp
using AllowDestructiveHandler = bool (*)();
using SetAllowDestructiveHandler = core::Result<void> (*)(bool);
void setDestructiveHandlers(AllowDestructiveHandler get,
                            SetAllowDestructiveHandler set);
bool allowDestructive();
core::Result<void> setAllowDestructive(bool on);
```

默认桩：getter `return false;`；setter `return Result<void>::fail(Internal, "MCP 服务未装配");`。补两条 `static_assert` 签名哨兵。

`main.cpp`：

```cpp
bool mcpAllowDestructiveImpl() {
    return module::mcp::globalMcpService().allowDestructive();
}
core::Result<void> mcpSetAllowDestructiveImpl(bool on) {
    module::settings::SettingsStore store(::core::appDatabase());
    auto cfg = store.load();
    cfg.mcpAllowDestructive = on;
    auto saved = store.save(cfg);
    if (saved.isFail()) return saved;
    module::mcp::globalMcpService().setAllowDestructive(on);
    return core::Result<void>::ok();
}
```

装配段 `app::mcp::setDestructiveHandlers(&mcpAllowDestructiveImpl, &mcpSetAllowDestructiveImpl);`。
`mcpStartImpl(port)` 里先 `SettingsStore store(::core::appDatabase()); globalMcpService().setAllowDestructive(store.load().mcpAllowDestructive);` 再 `start(port)`——模块开关/改端口的启动都经此桥（`app_assembly.cpp:388`）。

冷启动**直连**路径（`main.cpp:636` 的 `globalMcpService().start(...)`）同样要在启动前用 `gatingConfigSnapshot().mcpAllowDestructive` 赋值；否则冷启动会退回默认关。两处入口都改，才算口径一致。

`mcp_service.h/.cpp`：加 `std::atomic<bool> allowDestructive_{false};`（`mcp_service.h` 补 `#include <atomic>`）+ `setAllowDestructive/allowDestructive`；`start()` 构造 `McpProtocol` 后立刻 `protocol->setAllowDestructive(allowDestructive_.load())`。

- [ ] **Step 4: 跑绿**

Run: `bash scripts/build-test-run.sh --dev --filter mcp_settings`
Run: `bash scripts/build-test-run.sh --dev --filter mcp_bridge`
Expected: PASS。

重启延续用例（`test_mcp_service.cpp`，Review Focus 第 4 条）：

```cpp
REGISTER_TEST(mcp_service_allow_destructive_survives_restart,
              "mcp service: setAllowDestructive 在 stop/restart 后延续（不被重置为允许）", {
    module::mcp::McpService service(core::mcp::globalMcpTools());
    service.setAllowDestructive(true);
    DOCTEST_CHECK_TRUE(service.allowDestructive());
    // 未启动时 stop 是 no-op；启动→停止→再启动走同一条成员注入路径
    DOCTEST_REQUIRE(service.start(0).isOk());
    const int port = service.port();
    service.stop();
    DOCTEST_CHECK_TRUE(service.allowDestructive());
    DOCTEST_REQUIRE(service.start(port > 0 ? 0 : 0).isOk());
    DOCTEST_CHECK_TRUE(service.allowDestructive());
    service.stop();
    return true;
})
```

Run: `bash scripts/build-test-run.sh --dev --filter mcp_service`
Expected: PASS。

- [ ] **Step 5: 提交**

```bash
git commit -m "wip: 破坏性开关持久化（SettingsConfig 第 27 键）+ mcp 桥读写"
```

### Task 5: 页头开关行

**Files:**
- Modify: `src/module_mcp/mcp_page_module.h:85-100,149-152`
- Modify: `src/module_mcp/mcp_page_module.cpp:283-288, 413-415, 699-779`
- Test: `tests/test_modules/test_mcp_page.cpp:843-960`

**Interfaces:**
- 新元素 id 前缀：`mcp.module.destructive`（标题/说明/开关 `.toggle.hit` / `.toggle.track`）
- 复用记忆行样式；行高 64、宽度 = contentW

- [ ] **Step 1: 写失败用例**

照 `mcp_page_memory_module_row` 克隆一份到其后（测试文件已有 `MemoryBridgeGuard` 同款桩模式）：

```cpp
REGISTER_TEST(mcp_page_destructive_row,
              "mcp_page: 破坏性工具行 64 高/标题说明/开关；桥失败红字回弹；桥成功点亮", {
    core::mcp::McpToolRegistry reg;
    McpPageModule mod(reg);
    const eui::Screen screen = makeScreen();
    {
        eui::Ui ui;
        composeFrame(mod, ui, screen, "mcp_drow_1");
        const auto* row = ui.find("mcp.module.destructive");
        DOCTEST_REQUIRE(row != nullptr);
        DOCTEST_CHECK_EQ(row->height.value, 64.0f);
        const auto* title = ui.find("mcp.module.destructive.title");
        DOCTEST_REQUIRE(title != nullptr);
        DOCTEST_CHECK_EQ(title->text, std::string("破坏性工具"));
        const auto* toggle = ui.find("mcp.module.destructive.toggle.hit");
        DOCTEST_REQUIRE(toggle != nullptr);
        DOCTEST_REQUIRE(toggle->onClick != nullptr);
        const auto* track = ui.find("mcp.module.destructive.toggle.track");
        DOCTEST_REQUIRE(track != nullptr);
        DOCTEST_CHECK_FALSE(trackIsPrimary(track));  // 默认桩 = 关
    }
    {
        eui::Ui ui;  // 桥未装配：点击失败 → 红字 + 回弹
        composeFrame(mod, ui, screen, "mcp_drow_2");
        ui.find("mcp.module.destructive.toggle.hit")->onClick();
        composeFrame(mod, ui, screen, "mcp_drow_3");
        const auto* desc = ui.find("mcp.module.destructive.desc");
        DOCTEST_REQUIRE(desc != nullptr);
        DOCTEST_CHECK(desc->text.find("保存失败") != std::string::npos);
    }
    {   // 成功桩：先在测试文件加 DestructiveBridgeGuard 三件套（照 MemoryBridgeGuard）
        DestructiveBridgeGuard guard;
        g_allowDestructive = false;
        eui::Ui ui;
        composeFrame(mod, ui, screen, "mcp_drow_4");
        ui.find("mcp.module.destructive.toggle.hit")->onClick();
        DOCTEST_CHECK_TRUE(g_allowDestructive);  // onChange(true) 真到达桥
        composeFrame(mod, ui, screen, "mcp_drow_5");
        const auto* track = ui.find("mcp.module.destructive.toggle.track");
        DOCTEST_REQUIRE(track != nullptr);
        DOCTEST_CHECK_TRUE(trackIsPrimary(track));  // 点亮
    }
    return true;
})
```

同文件（`MemoryBridgeGuard` 下方）加桩：

```cpp
bool g_allowDestructive = false;
bool allowDestructiveRec() { return g_allowDestructive; }
core::Result<void> setAllowDestructiveRec(bool on) {
    g_allowDestructive = on;
    return core::Result<void>::ok();
}
struct DestructiveBridgeGuard {
    DestructiveBridgeGuard() {
        app::mcp::setDestructiveHandlers(&allowDestructiveRec,
                                         &setAllowDestructiveRec);
    }
    ~DestructiveBridgeGuard() {
        app::mcp::setDestructiveHandlers(nullptr, nullptr);
    }
    DestructiveBridgeGuard(const DestructiveBridgeGuard&) = delete;
    DestructiveBridgeGuard& operator=(const DestructiveBridgeGuard&) = delete;
};
```

- [ ] **Step 2: 跑红**

Run: `bash scripts/build-test-run.sh --dev --filter mcp_page_destructive_row`
Expected: FAIL（找不到 `mcp.module.destructive`）。

- [ ] **Step 3: 抽行助手 + 加行 + 改固定区高度**

`mcp_page_module.h` 私有段把 `drawMemoryModuleRow` 改为通用助手（保留原函数签名以避免大改）并新增（需补 `#include <functional>`）：

```cpp
    void drawToolSwitchRow(eui::Ui& ui, float contentW,
                           const core::theme::Tokens& t,
                           const components::theme::ThemeColorTokens& comp,
                           const char* idPrefix, const char* title,
                           const char* desc, bool checked,
                           std::string& errorSlot,
                           const std::function<core::Result<void>(bool)>& onToggle);
    std::string destructiveSaveError_;
```

`drawMemoryModuleRow` 变成 8 行转发（idPrefix `"mcp.module.memory"`、`app::memory_mcp::status().switchOn`、`memorySaveError_`、`app::memory_mcp::setEnabled`）；新增 `drawDestructiveModuleRow` 转发（idPrefix `"mcp.module.destructive"`、`app::mcp::allowDestructive()`、`destructiveSaveError_`、`app::mcp::setAllowDestructive`）。

`.cpp` 原 699-779 行的绘制体搬进助手，所有 `id + ".xxx"` 拼法保持不变（既有记忆用例 id 不动）。

固定区高度（原 287-288 行）：

```cpp
    const float headerH = headCardH + core::theme::scaled(64.0f) * 2.0f +
                          core::theme::scaled(26.0f) + t.sizes.spacingLarge * 4.0f;
```

compose 里在记忆行后追加 `drawDestructiveModuleRow(ui, contentW, t, comp);`。

- [ ] **Step 4: 跑绿**

Run: `bash scripts/build-test-run.sh --dev --filter mcp_page`
Expected: 记忆行、宿主行、卡片、弹窗等全部既有用例仍 PASS；新增用例 PASS。

- [ ] **Step 5: 真窗口 probe**

在 `tests/probes/mcp_card_343_probe.cpp` 的页头检查段追加：`mcp.module.destructive` 存在、`height == 64`、开关 track 右缘对齐记忆行 track 右缘；跑 `(cd build && ctest -L probe -R mcp_card)` 并确认截图产物落 `evidence/`。

- [ ] **Step 6: 提交**

```bash
git commit -m "wip: MCP 页头「破坏性工具」开关行（复用记忆行样式）"
```

### Task 6: E2E 真往返 + 收口

**Files:**
- Modify: `tests/probes/mcp_e2e_probe.cpp:240-320`
- Modify: `docs/superpowers/specs/2026-10-26-mcp-server-skeleton-design.md:736-750`
- Modify: `docs/USER_GUIDE.md`（MCP 小节）

**Interfaces:**
- 探针用 `module::mcp::globalMcpService().setAllowDestructive(bool)` 做真 HTTP 往返；不触碰真实文件。

- [ ] **Step 1: 失败探针步骤**

在 43 工具断言之后（不能提前，否则计数变 44），注册一个仅探针使用的破坏性 `IMcpTool`（name `probe_destructive_tool`，`isDestructive()` 返回 true，`execute` 只累加计数并返回 ok），然后：

1. 开关关闭（默认）→ `tools/call probe_destructive_tool` → 断言 HTTP 200、`result.isError == true`、`calls == 0`。
2. `globalMcpService().setAllowDestructive(true)` → 再调 → 断言 `calls == 1`、无 `isError`。
3. 收尾 `setAllowDestructive(false)` 并注销该工具（保持 43 口径）。

- [ ] **Step 2: 跑红**

Run: `bash scripts/build-test-run.sh --dev --no-test` 后 `(cd build && ctest -L probe -R mcp_e2e)`
Expected: FAIL（`setAllowDestructive` 不存在 / 行为未实现）。

- [ ] **Step 3: 文档同步**

spec §14.2 第 6 条「无鉴权/本机信任」后补一句契约：破坏性工具另有服务端开关（默认关），本机信任不再等同于「用户知情授权」；USER_GUIDE 的 MCP 小节加开关位置与拒绝文案说明。

- [ ] **Step 4: 收口全量**

Run: `bash scripts/build-test-run.sh`（不带 `--dev`：全量 doctest + probe + 守卫链）
Expected: `Total:` 全绿、probe 全绿、模块隔离守卫 0。

- [ ] **Step 5: squash 提交**

```bash
git add -A
git commit -m "feat: MCP 破坏性工具服务端闸 + 在途并发上限（默认关/默认 8）"
```

---

## 3. 后续批次（另立计划，已给落点与测试缝）

| 项目 | 落点 | 验收缝 |
|---|---|---|
| **A-3** 配置写 mtime 校验 | `mcp_config_common.cpp:16-47`：读前 `stat` 记 mtime，写前再 stat 比对，变化即 fail「配置文件正被修改，请稍后重试」（约 40 行） | 单测：改写前用假写入者 touch 文件 → `rewriteExisting` 返回 fail 且目标字节未变 |
| **B-1** LLM 重试/退避 | `direct_llm_client.h/.cpp`：加 `SleepFn` 注入缝（默认真 sleep），仅「网络失败 + 5xx」重试 2 次，延迟 1s/4s；4xx 不重试 | 假 `HttpFn` 连失败 3 次 + 记录 `SleepFn` 请求值 [1000,4000]；成功路径不 sleep |
| **C-2** MCP 调用日志 | 新 `module_mcp/mcp_call_log.{h,cpp}`：互斥保护环形 200 条（时间/工具/耗时/成败/拒绝原因），`globalMcpCallLog()`；协议层在结果返回处记录；页头加「调用记录」入口（约 150 行） | 单测：第 201 条覆盖最旧；拒绝/超时/detach 各有一条；页面弹窗读同一日志 |
| **B-2** 历史保留 | `agent_store.cpp`：`finishRun` 后异步裁剪「每任务最近 50 run + 全局 90 天」；先与 #387 小时备份联动评估体积（约 120 行） | 单测：造 51 run → 最旧一条级联删除；90 天边界用假时钟 |
| **B-3** 按任务工具白名单 | `agent_loop.cpp:190` 改为按 `task.tools` 过滤 schema；任务表单加多选，默认全选；落在 `trigger_config` 同源 JSON 列（约 200 行 + 迁移） | 单测：白名单 {fs.read} 时 `allSchemasJson` 只含该件；空集 = 全量（兼容旧数据） |
| **B-4** run 分页 | `agent_ui_shared.h:24` + `AgentStore::listRuns` 加 offset；详情「执行记录」加「加载更多」（约 80 行） | 单测：51 run 下首屏 20、第二页 20、末页 11，无重复无漏 |
| **C-1** MCP 网格搜索 | 复用 `agent_module_dialogs.cpp:1468` 的搜索组件（约 60 行） | 页面用例：输入 `fs` 只留名字/描述命中卡片；清空恢复全量 |
| **C-3** 来源 chip | `mcp_page_module.cpp:628-657`：直注 = 「内置」，镜像 = 来源模块名（adapter 暴露 source name 前缀）（约 40 行） | 页面用例：`devdesk_server_info` 显示内置；`fs_read` 显示「文件」类来源 |

---

## 4. 不重开的已知风险（来自 spec §14.2）

无鉴权/本机信任、会话 id 非密码学随机、DNS 重绑定不防护、响应体 2MiB 后才截断、`Server::stop()` 等在途工具、超时 detach 线程不可回收、无 resources/prompts/sampling、无 JSON-RPC batch、无 SSE。本计划的 A-1 只补「破坏性工具知情授权」一条，不改上述口径。