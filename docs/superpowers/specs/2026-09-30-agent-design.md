# DevDesk 智能体子系统设计

> **项目代号**：DevDesk
> **类型**：新增子系统（后台自动化 agent + tool calling）
> **目标平台**：Windows 11（当前开发环境）、macOS 27+
> **作者**：纯 AI 开发（用户不熟悉 C++）
> **创建日期**：2026-09-30
> **性质**：设计 spec（R4 定稿，可作 writing-plans 输入）
> **参考**：`ignacionr/cppgpt`（对话历史管理模式参考，依赖不照搬）
> **修订**：2026-09-30 R2——落实自审 + skeptic 审计（H1/H2/M1-M5/L1-L4/A1/A2）；R3——落实第三轮独立审计（M1 const/M2 表名/M3 契约测试/M4 stopAll 顺序/L1-L3）；**R4（定稿）**——交付切分 M1/M2（§14）+ 关闭四项开放项：cron 语法（§5.1）、工具回调线程安全分层（§10）、HttpClient 注入缝与 anthropic-version 归属（§11）+ 工程纪律与工作区（§15）

---

## 0. 结论速览

| 组 | 问题 | 结论 |
|---|---|---|
| D-1 | 形态 | **后台自动化**（无对话窗）；触发：手动 + 定时（**事件触发降级 Phase 2**，core 无事件总线、硬上违反模块隔离，见 §13）；单次任务 + 常驻 worker 两种执行模型 |
| D-2 | 模型通道 | **经本地代理**（`127.0.0.1:<port>/v1/messages`）非直连；复用 profile 体系 + failover + settings.json 接管；agent 不自己解 key / 拼鉴权头 |
| D-3 | 工具协议 | **只支持 Anthropic tool_use**（content block：`tool_use` / `tool_result`）；不支持 OpenAI function_calling（YAGNI，后续可加） |
| D-4 | 跨模块调工具 | **core 新增 `ITool` + `ToolRegistry`**（依赖反转）；不扩展 `Module` 基类、不引入全局 service 单例 |
| D-5 | 依赖 | **纯 C++ 零新依赖**：用 `platform::http::HttpClient` + `platform::json` + `SqliteDb`；禁 CPR / nlohmann_json / Python / Node / MCP 子进程 |
| D-6 | 工具来源 | **仅内置工具**（todo / markdown / claude 配置 / 读应用状态）；不支持用户自定义、不接 MCP |
| D-7 | 迭代控制 | `max_iterations` 硬上限 + 用户可配；请求超时；cancel；破坏性操作经 `allow_destructive` 开关授权（=0 遇破坏性工具直接 fail run，不挂起确认，见 §5.3） |
| D-8 | LlmEndpoint 抽象 | core 定义 `LlmEndpoint` 接口，`LlmClient` 实现（走本地代理）；agent loop 依赖抽象，可 mock 测试。**扩展 `core::IClaudeProxyService` 加 `resolveModelSlot`** 解析激活 profile 的模型槽位（见 §2.2 H1） |
| D-9 | 交付切分 | **两期**：M1 核心链路端到端（手动触发 + 9 工具 + 最小 UI）；M2 调度（cron 定时 + 常驻）+ 其余 8 工具 + UI 补全（见 §14） |

---

## 1. 整体架构

DevDesk 现有四层（`main → module_* → core → platform → EUI-NEO`）不变。本子系统在 `module_*` 层新增一个 **`module_agent`**（Router 页面模块，管任务配置 / 历史 / 结果），在 `core` 层新增三个抽象（`ITool` / `ToolRegistry` / `LlmEndpoint`）作依赖反转中介；并**扩展既有 `core::IClaudeProxyService`** 加 `resolveModelSlot`（解析模型槽位）。现有模块（`module_todo` / `module_markdown` / `module_claude_proxy` / `module_settings`）各自实现 `ITool` 并在 `onStart` 注册到 `core::ToolRegistry`。

```
                 ┌──────────────────────────────┐
                 │          main.cpp            │
                 └───────────┬──────────────────┘
                             │ 装配（app_assembly）
         ┌───────────────────┼───────────────────┐
         │                   │                   │
         ▼                   ▼                   ▼
  ┌─────────────┐   ┌─────────────────┐   ┌─────────────┐
  │ module_todo  │   │  module_agent   │   │module_markdown│  ...其他模块
  │ 实现 TodoTool│   │  AgentLoop      │   │ 实现 MdTool │
  └──────┬──────┘   │  LlmClient      │   └──────┬──────┘
         │          │  Scheduler      │          │
         │ ITool    │  AgentModule    │  ITool   │
         │ 注册     └────────┬────────┘ 注册     │
         │                  │ 经 core 中介调     │
         ▼                  ▼                    ▼
  ┌──────────────────────────────────────────────────┐
  │  core (lib)                                       │
  │   ITool / ToolRegistry / LlmEndpoint（本设计新增）│
  │   IClaudeProxyService（扩展 resolveModelSlot）   │
  │   Result / SqliteDb / Router / Logging（既有）    │
  └──────────────────────┬───────────────────────────┘
                         │
                         ▼
  ┌──────────────────────────────────────────────────┐
  │  platform (lib)                                   │
  │   http_client（发 LLM 请求）/ json / keychain     │
  └──────────────────────┬───────────────────────────┘
                         │
                         ▼  本地 HTTP 代理（module_claude_proxy 既有）
                   真实上游（Anthropic / 兼容网关）
```

**数据流（一次 agent run）**：

1. 调度层触发任务 → `AgentLoop::run(task)` 在后台线程启动
2. `AgentLoop` 取任务 goal 作 system+user 首条消息，构造 Anthropic `/v1/messages` 请求（带 `tools` 定义）
   > **M1 实况标注（2026-10-01，final-review Fix 4）**：M1 无 system prompt（`AgentTask` 无该字段，`LlmEndpoint::send` 的 systemPrompt 形参传空串），goal 仅作 user 首条消息；system prompt 由 M2 引入。
3. `LlmClient::send()` 先经 `core::IClaudeProxyService` 预检代理状态（未跑 → run 标 failed + error「代理未启」）+ 取实际端口；经 `resolveModelSlot(task.modelSlot)` 取实际 model id 填进 body（解析失败 → run 标 failed，见 §2.2 H1）；再经 `platform::http::HttpClient` POST 到本地代理 `127.0.0.1:<port>/v1/messages`（代理已由 `module_claude_proxy` 起好，按激活 profile 转发到真实上游，带 failover）
4. 解析响应：若 `stop_reason == "tool_use"`，逐个 `tool_use` block → `core::ToolRegistry::find(name)` 取 `shared_ptr<ITool>`（keep-alive，防 unregister 悬垂）；**find NotFound（迭代间模块被拔）→ 构造 `tool_result {is_error: true, content: "工具 <name> 未找到，可能模块已禁用"}` 回喂 LLM，不中止 run**（L3a，让 LLM 自纠或换工具）→ `execute(args)` → 把 `tool_result` 拼进 messages → 回到步骤 3
5. 若 `stop_reason == "end_turn"`（或达 `max_iterations`）→ run 结束，落库 agent_runs / agent_messages
6. UI 经 `callLogVersion()` 同款版本号机制感知异步落库，刷新结果

---

## 2. core 新增抽象（D-4 / D-8）

core 新增三个抽象（分两个头文件）+ 扩展既有 `IClaudeProxyService`。**都不依赖 EUI-NEO / platform / 具体模块业务**，符合「core 不能依赖 EUI-NEO 或 platform」铁律。

### 2.1 `core/tool.h` — ITool + ToolRegistry

```cpp
// src/core/tool.h
// 智能体工具调用中介（依赖反转）：core 定义抽象，各 module_* 实现并注册，
// module_agent 经 ToolRegistry 按名调用。agent 不依赖任何具体模块，
// 具体模块也不依赖 agent —— 双方都只依赖 core。符合 spec §4.2 模块隔离铁律。
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/result.h"

namespace core {

// 工具参数：原始 JSON 串（Anthropic tool_use.input 的 JSON 文本）。
// 由 ITool 实现方自行用 platform::json 解析；core 不依赖 platform::json。
struct ToolArgs {
    std::string json;
};

struct ToolResult {
    bool isError = false;       // true = 回喂 LLM 时标 is_error（Anthropic tool_result）
    std::string contentJson;    // JSON 串（建议 {"content": ...} 或直接值）
};

// 工具接口：每个内置工具实现一个。name() 全局唯一（注册时冲突拒）。
// schemaJson() 返回 Anthropic tools 定义片段（name/description/input_schema），
// 由 AgentLoop 在请求里带上。execute() 同步执行，返回结果回喂 LLM。
// execute() 应返回 Result fail 而非抛异常（见 §10 工具异常语义）。
class ITool {
public:
    virtual ~ITool() = default;
    virtual std::string name() const = 0;           // 如 "todo.create"
    virtual std::string schemaJson() const = 0;     // Anthropic tool 定义 JSON
    virtual Result<ToolResult> execute(const ToolArgs& args) = 0;
};

// 注册表（进程级单例，同 ClaudeService::global() 模式）。module_* 在 onStart
// 调 register()，onStop 调 unregister()。AgentLoop 经 find() 取用。
// 线程安全：读写各一把锁（onStart/onStop 主线程写，AgentLoop 工作线程读）。
class ToolRegistry {
public:
    static ToolRegistry& instance();

    // 注册：name 冲突 → fail（ValidationError）。重复注册同一指针 = no-op。
    Result<void> registerTool(std::shared_ptr<ITool> tool);
    void unregisterTool(const std::string& name);

    // 查找：返回 shared_ptr（keep-alive）——模块运行时可插拔（设置页模块开关），
    // 裸指针在 find 与 execute 之间被 unregister 会 use-after-free。
    // 调用方持有 shared_ptr 期间 unregister 不悬垂。不存在 → fail（NotFound）。
    Result<std::shared_ptr<ITool>> find(const std::string& name) const;

    // 全部工具的 schemaJson（拼成 Anthropic tools 数组）
    std::string allSchemasJson() const;

private:
    ToolRegistry() = default;
    std::map<std::string, std::shared_ptr<ITool>> tools_;
    mutable std::mutex mutex_;  // 读写各一把锁（实现时定粒度，见 §10 线程模型）
};

} // namespace core
```

**为什么 core 加 ITool+ToolRegistry，而不是别的**（D-4）：

| 备选 | 否决理由 |
|---|---|
| 扩展 `Module` 基类加 `handleAction()` | 改 Module 基类影响全部 6 个现有模块（都要动）；Router 要加「按页找实例」接口；动作 RPC 语义和现有「导航+payload」异步语义混在一起。违反「优先扩展而非修改」（OCP）。 |
| 全局 service 单例（`TodoService::global()` 等） | 全局单例蔓延（每加一个可调模块加一个单例）；单例头放 core 等于 core 反向依赖 todo/markdown 业务模型（违反「core 不碰具体业务」）；单例头放 module 则 agent 依赖具体模块（违反模块隔离）。 |
| agent 直读写 SQLite 绕过业务模块 | 绕过 todo/markdown 的业务逻辑与状态管理，可能与模块运行态不一致（如 todo 模块内存态、markdown 索引）；数据模型漂移直接炸 agent。 |

ITool+ToolRegistry 是**依赖反转**：core 定义抽象契约，具体模块实现（依赖 core 头，允许——「module_* 可以依赖 core」），agent 经 registry 调（也只依赖 core 头）。agent 和 todo 之间**不存在直接依赖**，模块隔离铁律零违反。

### 2.2 `core/llm_endpoint.h` — LlmEndpoint + 扩展 IClaudeProxyService

```cpp
// src/core/llm_endpoint.h
// LLM 请求的抽象端点：agent loop 依赖此抽象，LlmClient（module_agent 内）
// 实现为「经本地代理」形态。测试可注入 mock LlmEndpoint 验证 agent loop
// 逻辑，不发真实网络请求。
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/result.h"

namespace core {

// Anthropic messages API 的一条消息（role + content blocks JSON 原文）
struct LlmMessage {
    std::string role;           // "user" / "assistant"
    std::string contentJson;    // content blocks 的 JSON 数组原文
};

struct LlmResponse {
    std::string stopReason;     // "end_turn" / "tool_use" / "max_tokens" ...
    std::string contentJson;    // 响应 content blocks 原文（含 tool_use blocks）
    int64_t inputTokens = -1;   // -1 = 未采集
    int64_t outputTokens = -1;
};

class LlmEndpoint {
public:
    virtual ~LlmEndpoint() = default;
    // 发一次 /v1/messages 请求。messages 为完整对话历史（agent loop 累积），
    // toolsJson 为 Anthropic tools 定义数组。systemPrompt 可空。
    // model 参数由调用方（LlmClient）经 IClaudeProxyService::resolveModelSlot
    // 解析后传入（见下方 H1）——本接口只发，不解析槽位。
    // > **M1 实况标注（2026-10-01，final-review Fix 4；按 Task 7 裁定，
    // > 上两行原文被取代）**：model 形参语义 = **槽位键名**（AgentLoop 原样
    // > 传 task.modelSlot），槽位 → 实际 model id 的解析在 LlmClient::send
    // > **内**经 IClaudeProxyService::resolveModelSlot 完成（proxy_.resolveModelSlot），
    // > 本抽象不解析槽位——与上两行的「解析后传入」相反，实现以此为准。
    virtual Result<LlmResponse> send(
        const std::vector<LlmMessage>& messages,
        const std::string& toolsJson,
        const std::string& systemPrompt,
        const std::string& model) = 0;
};

} // namespace core
```

**H1 — 模型槽位解析（扩展 `core::IClaudeProxyService`）**：

`AgentTask.modelSlot`（如 "default"/"sonnet"）是 profile 的槽位**键名**，但 Anthropic 请求 body 的 `model` 字段必须是具体模型 id（如 `claude-sonnet-5-5`）。代理是纯透传（`proxy_router.cpp` body 原样转发，`upstream_dialect` 只剥 `[1M]` 后缀、只改顶层 model 字节区间），**不按槽位改写**。若 agent 直接发 "default" 给上游 = 400 全挂。

解法：**扩展既有 `core::IClaudeProxyService`**（`src/core/claude_proxy_service.h`，已有 `isProxyRunning`/`proxyPort`/`stopProxy` 等，由 `GlobalClaudeProxyService<T>` 适配器转发到 `ClaudeService::global()`）加一个方法：

```cpp
// 扩展 core::IClaudeProxyService（既有接口加一个方法，GlobalClaudeProxyService 适配器转发）
// 返回激活 profile 该槽位的实际 model id；无激活 profile / 槽位空 → nullopt。
// ClaudeService 实现时经 ProfileRepository 取激活 profile + parseModelsConfig 解析槽位。
// non-const（M1）：取激活 profile 链（ClaudeService::getActiveProfile / ProfileRepository
// / SqliteDb::prepare）全 non-const，const 虚函数内调不了；签名去 const。
virtual std::optional<std::string> resolveModelSlot(const std::string& slot) = 0;
```

`LlmClient::send()` 发请求前调 `IClaudeProxyService::resolveModelSlot(task.modelSlot)` 取 model id 填进 body；返回 `nullopt` → `AgentLoop` 标 run `status='failed'` + error「无法解析模型槽位 `<slot>`（无激活 profile / 槽位未配置）」，不发包。

agent 仍不 include `claude_service.h`（经 `core::IClaudeProxyService` 抽象，由 main.cpp 注册的 `GlobalClaudeProxyService<ClaudeService>` 适配器转发）。模块隔离零违反。

**为什么扩展现有 IClaudeProxyService 而非新增 IProfileModelResolver**：`IClaudeProxyService` 已是「module_settings 等消费方读 ClaudeService 运行面」的既定窄接口（R0 审计 D6 设立），agent 取代理端口 + 解析槽位同属「读 ClaudeService 运行面」，复用同一接口不蔓延新抽象；加一个方法 = 表驱动扩展，非改既有方法签名（OCP）。

**为什么抽 LlmEndpoint**（D-8）：agent loop 的核心逻辑（tool_use 循环、迭代控制、落库）应可独立单测——抽 LlmEndpoint 后，注入 mock 返回固定 `tool_use` / `end_turn` 序列即可验证 loop 的所有分支，不发网络。LlmClient（module_agent 内）是生产实现（走本地代理），也只依赖 core 抽象 + platform::http。

---

## 3. module_agent（D-1）

`module_agent` 是新增的 Router 页面模块（像 `module_claude_proxy` 那样有管理界面）。内部组件：

| 组件 | 职责 |
|---|---|
| `AgentModule`（`Module` 实现） | 页面 compose（任务列表 / 详情 / 新建表单 / 结果展示）；`onStart` 注册到 Router、`onStop` 清理 |
| `AgentLoop` | 单次 run 的执行器：消息累积、调 LlmEndpoint、解析 tool_use、经 ToolRegistry 执行、回喂、循环、迭代控制、落库 |
| `LlmClient`（`LlmEndpoint` 实现） | 经 `core::IClaudeProxyService` 预检代理 + 取端口 + 解析槽位；经 `platform::http::HttpClient` POST 本地代理；解析响应为 `LlmResponse` |
| `Scheduler` | 触发：手动启动 / 定时（cron-like）；管理单次任务与常驻 worker 两种执行模型（事件触发 Phase 2，见 §13） |

```cpp
// src/module_agent/agent_loop.h
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "core/llm_endpoint.h"
#include "core/result.h"
#include "core/tool.h"

namespace module::agent {

struct AgentTask {
    int64_t id = 0;
    std::string name;
    std::string goal;           // 用户给的自然语言目标（作首条 user 消息）
    std::string modelSlot;      // profile 模型槽位键名："default"/"sonnet"/"opus"/"haiku"/"fable"/"subagent"
                                //   （provider_config.h DEVDESK_MODEL_SLOT_LIST 实际键，非 "def"）
    int maxIterations = 10;     // 迭代硬上限（D-7）
    bool allowDestructive = false;  // 破坏性工具授权（D-7，见 §5.3）
    // 触发配置（triggerType / triggerConfigJson）见 §5
};

class AgentLoop {
public:
    AgentLoop(core::LlmEndpoint& llm, core::ToolRegistry& tools,
              core::IClaudeProxyService& proxy);  // non-const（resolveModelSlot non-const，M1）

    // 同步执行一次任务（调用方通常在工作线程调）。cancelToken 置 true 则
    // 在下轮迭代前中止。runId 落库 agent_runs；每轮消息落 agent_messages。
    // 失败不抛：LLM 错误 / 工具真异常 → run.status='failed' + error；
    //          达迭代/墙钟/token上限 → status='limited' + error；
    //          cancel → status='cancelled'。详见 §5.2 状态语义。
    Result<int64_t> run(const AgentTask& task,
                        std::atomic<bool>& cancelToken);

private:
    core::LlmEndpoint& llm_;
    core::ToolRegistry& tools_;
    core::IClaudeProxyService& proxy_;  // 预检代理 + resolveModelSlot（non-const，M1）
};

} // namespace module::agent
```

**生命周期与 teardown（M5）**：`AgentLoop` 无状态（依赖注入 LlmEndpoint + ToolRegistry + IClaudeProxyService），可并发跑多个任务（每个任务一个 `AgentLoop` 实例 + 工作线程）。常驻 worker 是 `Scheduler` 内一个长存线程从队列取任务跑。

`AgentModule::onStop` 顺序（**结构保证，勿改**）：
1. 置所有在跑 run 的 `cancelToken = true`
2. `join` 所有 AgentLoop 工作线程（等待当前迭代结束）
3. 停常驻 worker 线程
4. 最后 `unregisterTool`（若有 module_agent 自己注册的工具，如 `app.stats`）

先 cancel + join 再 unregister，保证工具在用期间不被拆注册（与 M4 的 shared_ptr keep-alive 双保险）。

**stopAll 顺序与跨模块工具（第三轮审计 M4）**：`Router::stopAll()`（`router.cpp:107-112`）按注册**正序**遍历，兄弟模块（todo/markdown/claude_proxy）的 onStop（unregister 工具）先于 module_agent 的 onStop（cancel+join）执行。当前 exit 路径模块对象在 atexit handler 之后才析构（对象存活到 stopAll 返回之后），UAF 不会发生；但「运行中禁用模块」未来场景下，跨模块工具 unregister 早于 agent cancel+join 是已知限制。**本 spec 范围：teardown 保证仅覆盖 module_agent 自注册工具（`app.stats`）；跨模块工具靠 atexit 后析构兜底**。未来若支持运行中禁用模块，需 `Router::stopAll` 改逆序（LIFO）或 module_agent onStop 不依赖跨模块工具仍注册（留 plan 阶段评估）。

---

## 4. 工具协议（D-3 / D-2）

### 4.1 为什么只支持 Anthropic tool_use（D-3）

Anthropic `/v1/messages` 的 tool 协议：请求带 `tools` 数组（每个含 `name`/`description`/`input_schema`）；响应 `stop_reason == "tool_use"` 时 content blocks 里有 `tool_use` block（`id` / `name` / `input`）；回喂时 user 消息的 content blocks 里带 `tool_result` block（`tool_use_id` / `content` / `is_error`）。

**只支持 Anthropic 格式的理由**：

1. DevDesk 现有 `module_claude_proxy` 已经在转发 `/v1/messages`，`streaming_proxy` 已解析过 Anthropic SSE 事件（`SseUsageParser`），团队/代码对 Anthropic 格式熟。
2. 复用现有 profile 体系（含 failover），agent 经本地代理发 `/v1/messages` = 复用全部既有转发/方言/failover 逻辑，零新鉴权代码。
3. OpenAI function_calling 是另一种格式（`tools` + `tool_choice` + `choices[0].message.tool_calls`），支持它 = agent loop 里两套解析路径 + 两套消息累积格式。YAGNI——先只做 Anthropic，后续 profile 体系若明确走 OpenAI 兼容网关再加（`LlmEndpoint` 抽象使可扩展）。

**被否决**：同时支持两种协议（复杂度翻倍，且 DevDesk 的 profile 默认就是 Anthropic 形态）；只支持 OpenAI 格式（割裂既有 `/v1/messages` 经验）。

### 4.2 为什么经本地代理而非直连（D-2）

| 维度 | 经本地代理（选） | agent 直连上游 |
|---|---|---|
| 鉴权 | 代理已注入（按 profile 的 authField） | agent 自己解 Keychain + 拼鉴权头 |
| failover | 代理按 `shouldFailoverTo` 换备用 profile | agent 重写一遍 failover 逻辑 |
| settings.json 接管 | 代理已接管（Claude Code 与 agent 共用同一上游配置） | agent 另管一套，与 Claude Code 配置分叉 |
| 方言 | 代理已处理（upstream_dialect） | agent 重写方言归一 |
| 代价 | 依赖代理在跑（无激活 profile → 503，需提示用户先启代理） | 自洽 |

agent 定位为「DevDesk 内部消费者」——经本地代理 = 复用全部既有转发能力，零重复实现。代价（代理须在跑）通过 `LlmClient` 预检解决：run 前经 `IClaudeProxyService::isProxyRunning()` 判，未跑 → run 标 `failed` + error「代理未启，请先在 Claude 页启动代理」（**不自动拉起代理**，见 §13 YAGNI）；定时任务若跑到代理未启状态，同样落 `failed` run，UI 可见，用户下次启动代理后手动重跑。

### 4.3 消息累积模式（参考 cppgpt）

参考 `ignacionr/cppgpt` 的 `conversation` 累积模式：每轮把新消息追加进历史。Anthropic 形态下累积顺序：

```
[system] systemPrompt
[user]   goal（首条）
[assistant] 响应1 content（含 N 个 tool_use block）
[user]   tool_result_1, tool_result_2, ...（同一条 user 消息的多个 block）
[assistant] 响应2 ...
[user]   tool_result_* ...
[assistant] 最终响应（stop_reason == end_turn）
```

> **M1 实况标注（2026-10-01）**：`[system]` 行 M1 缺席——无 system prompt（`AgentTask` 无该字段），消息序列自 `[user] goal` 起；M2 引入。

每轮的 messages 全量发给上游（Anthropic 无自动历史，agent 自管）。长对话超模型上下文时，按 `max_iterations` + 用户可配的截断策略（YAGNI：先不截断，靠 max_iterations 兜底）。

---

## 5. 调度与执行模型（D-1 / D-7）

本阶段触发：手动 + 定时。**事件触发降级 Phase 2**（core 无事件总线，硬上违反模块隔离，见 §13）。

### 5.1 触发类型

| 触发 | 配置 | 执行模型 |
|---|---|---|
| 手动 | UI「启动」按钮点一次跑一次 | 单次任务跑完即停 |
| 定时 | cron-like 表达式（存 `trigger_config_json`） | 定时到点起一个单次任务 |
| ~~事件~~ | ~~`todo.created` / `md.saved` 钩子~~ | **Phase 2 后续，本阶段不实现**（core 无事件总线，发射点要改 module_todo/markdown 业务逻辑，违反 §9 零改动 + 模块隔离；core 加通用事件总线超出本 spec 范围） |

**常驻 worker**：用户可配一个「常驻」任务（`trigger_type='resident'`），`Scheduler` 起一个长存线程，从内部任务队列取子任务跑（队列空则等待）。常驻任务每轮 run 仍是单次 AgentLoop（受 `max_iterations` 单次上限约束）；常驻语义 = 「持续待命接受外部派单」，本阶段不接受外部事件派单（事件触发 Phase 2），故常驻 worker 本阶段实际只跑用户手动入队的子任务（如「整理所有逾期 todo」可重复跑）。常驻任务 teardown 见 §3 生命周期。

**定时触发（cron 语法，R4 定义）**：

- 语法：标准 5 字段 `分 时 日 月 周`；支持 `*` `,` `-` `/`（如 `*/5 * * * *`、`0 9 * * 1-5`）；周字段 0-6（0=周日）；**不支持** Quartz 扩展（`L`/`W`/`#`）与英文名称
- 时区：本地时间
- 解析：纯函数 `parse(str) → Result<CronExpr>`（任务保存时校验，非法即拒）+ `matches(tm) → bool` 纯函数，表驱动单测
- tick：`Scheduler` 内部线程按分钟边界醒来，对每个 enabled 的 schedule 任务调 `matches`；`lastFiredMinute` 同分钟防重
- 补跑：**错过的触发不补跑**（app 关闭期间错过的定时直接跳过，YAGNI）
- 时钟注入：构造注入 `std::function<int64_t()>`，测试零等待
- 存储：表达式原文存 `agent_tasks.trigger_config`（§6，JSON 形如 `{"cron": "0 9 * * 1-5"}`）

### 5.2 迭代控制（D-7）

每个 run（单次 loop）的硬约束：

| 约束 | 默认 | 上限 |
|---|---|---|
| `max_iterations`（tool_use 循环次数） | 10 | 用户可配，硬上限 50；AgentLoop 内 `effectiveMax = std::min(task.maxIterations, 50)` clamp（不只 UI 钮制，防绕过，L3b） |
| 单次 LLM 请求超时 | 60s | 用户可配 |
| 单 run 墙钟 | `max_iterations × 请求超时` | 硬上限 30 分钟 |
| token 累计 | 上游返回的 input+output 累计 | 硬上限 200K（防烧钱） |

> **M1 实况标注（2026-10-01，final-review Fix 4）**：30 分钟墙钟与 200K token 上限**M1 未实现**——M1 的实际边界 = `max_iterations`（默认 10，UI 可调至 50，loop 内 clamp）× 60s 请求超时（HttpClient 层）兜底；两硬上限由 AgentLoop 检查点强制（Scheduler 等触发路径经 loop 组合传递全覆盖；见 agent_loop.cpp 墙钟超时判定注释）。

**状态语义**（L1 统一）：

| status | 触发 |
|---|---|
| `running` | run 进行中 |
| `completed` | `stop_reason == "end_turn"`（LLM 给出最终回复） |
| `failed` | LLM 错误 / 工具真异常（C++ throw）/ 代理未启 / 模型槽位解析失败 / 破坏性操作未授权 |
| `limited` | 达 `max_iterations` / 墙钟 / token 上限 / `stop_reason == "max_tokens"` 或 `"stop_sequence"`（截断/受限，L1） |
| `cancelled` | `cancelToken` 置 true，下轮迭代前中止 |

达任一上限 → run 终止，`status='limited'` + error 说明；`cancelToken` → `status='cancelled'`。§3 AgentLoop 注释与此对齐。

### 5.3 破坏性操作授权（D-7，M2 修订）

工具按「副作用」分级：

| 级别 | 工具 | 处理 |
|---|---|---|
| 只读 | `*.list` / `*.search` / `app.stats` / `app.read_settings` 等 | agent 直接调 |
| 写但可逆 | `todo.create` / `todo.move` / `md.create` 等 | agent 直接调，落 agent_messages 可审计 |
| 破坏性 | `todo.delete` / `claude.switch_profile` / `md.delete` | **仅 `task.allow_destructive=1` 时执行**；`=0` 时 AgentLoop 遇到破坏性工具**直接终止 run**，`status='failed'`，error「破坏性操作 `<tool>` 未授权（需 allow_destructive）」 |

**不挂起、不弹 UI 确认**（后台自动化无对话窗，工作线程挂起等确认会导致定时/深夜 run 永久卡死、常驻 worker 队列堵死）。手动触发也同此规则：用户想跑破坏性任务须在任务配置里显式开 `allow_destructive`。`agent_runs.status` 枚举**不**加 `pending_sensitive`（不需要挂起态）。

---

## 6. 数据模型

三张新表，migration version **12**（R4 修正：原定 v11 已被并行交付的 `kClaudeExtraSettingsJson` 占用，2026-09-30 实测 `migrations.h` all() 末项 = v11；接续顺延 v12）。

```sql
-- migration v12：智能体子系统（spec 2026-09-30-agent-design §6；R4 修正 v11→v12）
--   - agent_tasks：任务定义（goal + 触发配置 + 迭代上限 + 模型槽位 + allow_destructive）
--   - agent_runs：单次执行记录（一个 task 可有多次 run）
--   - agent_messages：每轮消息（role + content blocks 原文，可审计 + 上下文重建）
-- 与既有表无外键关联（agent 自治；profile 经本地代理间接用，不在表里硬绑 profile_id）。
-- 注意：ALTER TABLE ADD COLUMN 无 IF NOT EXISTS，靠 schema_version 版本号保证只执行一次。
CREATE TABLE IF NOT EXISTS agent_tasks (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  name TEXT NOT NULL,
  goal TEXT NOT NULL,                       -- 自然语言目标
  trigger_type TEXT NOT NULL,               -- 'manual' / 'schedule' / 'resident'（'event' Phase 2）
  trigger_config TEXT NOT NULL DEFAULT '',   -- JSON（cron 表达式 / 常驻参数）
  model_slot TEXT NOT NULL DEFAULT 'default',-- 复用 profile 模型槽位键名（default/sonnet/...）
  max_iterations INTEGER NOT NULL DEFAULT 10,
  allow_destructive INTEGER NOT NULL DEFAULT 0,  -- 0 = 破坏性工具直接 fail run，1 = 允许执行
  enabled INTEGER NOT NULL DEFAULT 1,        -- 0 = 禁用（定时不触发）
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS agent_runs (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  task_id INTEGER NOT NULL,
  status TEXT NOT NULL,                     -- 'running'/'completed'/'failed'/'limited'/'cancelled'（见 §5.2）
  started_at INTEGER NOT NULL,
  ended_at INTEGER,
  iterations INTEGER NOT NULL DEFAULT 0,    -- 实际 tool_use 循环次数
  input_tokens INTEGER,                     -- 累计，NULL = 未采集
  output_tokens INTEGER,                     -- 累计，NULL = 未采集
  error TEXT,                                -- 失败/受限原因
  FOREIGN KEY (task_id) REFERENCES agent_tasks(id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_agent_runs_task
  ON agent_runs(task_id, started_at DESC);

CREATE TABLE IF NOT EXISTS agent_messages (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  run_id INTEGER NOT NULL,
  seq INTEGER NOT NULL,                     -- 消息序（按累积顺序）
  role TEXT NOT NULL,                       -- 'user' / 'assistant' / 'system'
  content_json TEXT NOT NULL,               -- content blocks 原文（含 tool_use / tool_result）
  tool_call_id TEXT,                        -- tool_result block 对应的 tool_use_id（便于审计）
  created_at INTEGER NOT NULL,
  FOREIGN KEY (run_id) REFERENCES agent_runs(id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_agent_messages_run
  ON agent_messages(run_id, seq);
```

**为什么三表而非一表**：task（定义）↔ run（执行）↔ message（轮次）是天然的三层——一个 task 多次跑（定时触发 / 手动重试），一个 run 多轮消息。合一表要么 message 重复存 task 配置、要么 run 状态无处放。参考既有 `claude_profiles ↔ claude_call_logs` 的「定义 ↔ 执行记录」两层模式，本设计扩展为三层。

**为什么 agent 表不绑 profile_id**：agent 经本地代理，profile 由代理侧管理（激活态 + failover）；agent 只指定用哪个模型槽位（`model_slot`），不感知具体 profile（经 `IClaudeProxyService::resolveModelSlot` 间接解析，见 §2.2 H1）。解耦后 agent 表数据不随 profile 增删漂移。

---

## 7. 内置工具清单（D-6）

仅内置，不支持用户自定义。每个工具实现 `core::ITool`，所属模块在 `onStart` 注册。

| 工具名 | schema 摘要 | 所属模块 | 读写语义 |
|---|---|---|---|
| `todo.list` | `{column?, completed?}` | module_todo | 只读 |
| `todo.create` | `{title, column?, priority?, due_at?}` | module_todo | 写（可逆） |
| `todo.update` | `{id, title?, priority?, due_at?}` | module_todo | 写（可逆） |
| `todo.move` | `{id, column}` | module_todo | 写（可逆） |
| `todo.complete` | `{id}` | module_todo | 写（可逆） |
| `todo.delete` | `{id}` | module_todo | **破坏性** |
| `md.search` | `{query, limit?}` | module_markdown | 只读（FTS5） |
| `md.read` | `{path}` | module_markdown | 只读 |
| `md.create` | `{path, content}` | module_markdown | 写（可逆） |
| `md.list_tags` | `{}` | module_markdown | 只读 |
| `md.delete` | `{path}` | module_markdown | **破坏性** |
| `claude.list_profiles` | `{}` | module_claude_proxy | 只读 |
| `claude.switch_profile` | `{id}` | module_claude_proxy | **破坏性**（切配置） |
| `claude.test_connection` | `{id?}` | module_claude_proxy | 只读（发测试请求） |
| `claude.recent_logs` | `{limit?}` | module_claude_proxy | 只读 |
| `app.read_settings` | `{}` | module_settings | 只读（读 settings 自己的配置） |
| `app.stats` | `{}` | **module_agent** | 只读（经 Router 查模块状态 + 直查 SQLite 统计表行数） |

工具集先覆盖以上四类，后续可加（每加一个 = 新建一个 `ITool` 实现 + onStart 注册，不改 agent）。

**`app.stats` 归属（A1）**：放 **module_agent 自己实现**——`app.stats` 要读笔记数 / todo 数等统计，数据在 `module_todo` / `module_markdown`，`module_settings` 读不到（违反模块隔离）。`module_agent` 可依赖 `core::SqliteDb` 直查 `todo_tasks` / `markdown_index` 表行数（M2：表名经 `migrations.h:67`/`migrations.h:29` 核实；agent 是这些表的消费者之一，直查统计不破坏业务逻辑——只读 COUNT）。`app.read_settings` 留 `module_settings`（只读 settings 自己的配置，OK）。

**为什么不含桌宠/系统操作工具**：YAGNI——agent 不需要操作桌宠；系统操作（开文件/启进程）会引入子进程依赖（违反 D-5）。

---

## 8. UI

`module_agent` 的页面（Router 一个 PageId，新增 `PageId::Agent`）：

| 区 | 内容 |
|---|---|
| 任务列表 | 表格：名称 / 触发类型 / 状态（最近 run）/ 上次结果；行内「启动」「禁用」按钮 |
| 任务详情 | goal / 触发配置 / 模型槽位 / max_iterations / allow_destructive；最近 N 次 run 列表（状态 / 耗时 / 迭代数 / token） |
| 新建/编辑表单 | goal 文本框 / 触发类型选择 / cron 输入 / 模型槽位下拉 / max_iterations / 破坏性授权开关 |
| Run 详情 | 某次 run 的消息序列（role + content 摘要 + tool_use 名 + tool_result 摘要）；token 用量；错误说明 |

UI 实现细节（具体组件、布局）在 plan 阶段定，本 spec 只约束「任务可配置、run 可审计、破坏性操作可见」。

**侧边栏 / 门控（L4 修订）**：`module_agent` 门控参照 **todo/markdown 的 gatingKey 行**（不是 module_pet——pet 不在 `app_registry`，`app_registry.h:43-50` 只有 6 个 Router 模块）：`app_registry` 的 `kModuleRegistry` 加一行 Agent（`module::settings::kModuleIdAgent`）+ `settings_config.h` 加 `kModuleIdAgent` 键。默认关闭（agent 跑 LLM 有成本，用户主动启用）。

**PageId 枚举位置（A2）**：`PageId::Agent` 加在 `PageId::About` **之前**（枚举序 = 侧边栏顺序，契约见 `module_interface.h` 注释）。最终序：`Home/Markdown/Todo/ClaudeProxy/Settings/Agent/About`。`kModuleRegistry` 表也把 Agent 行加在 About 行之前。

---

## 9. 现有模块改动

每个改动都是**新增**（实现 ITool + 注册），不改既有装配逻辑核心。

| 模块 | 新增 | 不改 |
|---|---|---|
| `module_todo` | `TodoTools.{h,cpp}`：6 个 ITool 实现；`onStart` 注册、`onStop` 注销 | todo 业务逻辑、UI、数据模型 |
| `module_markdown` | `MarkdownTools.{h,cpp}`：5 个 ITool 实现（含 md.delete）；onStart 注册 | md 解析、渲染、索引 |
| `module_claude_proxy` | `ClaudeTools.{h,cpp}`：4 个 ITool 实现（调 ClaudeService 既有方法）；onStart 注册；实现 `IClaudeProxyService::resolveModelSlot`（**non-const**，经 ProfileRepository 取激活 profile 的 models_config 解析槽位键→model id，M1） | claude_service.cpp 既有 43 签名、代理转发、failover |
| `module_settings` | `AppTools.{h,cpp}`：1 个 ITool 实现（`app.read_settings`）；onStart 注册 | settings 配置、UI |
| `module_agent` | `AppStatsTool`：1 个 ITool 实现（`app.stats`，经 SQLite 统计）；`AgentModule` + `AgentLoop` + `LlmClient` + `Scheduler` | — |
| `core/migrations.h` | 加 `kAgentTables` v12（§6 SQL） | 既有 11 个 migration |
| `core/module_interface.h` | `PageId::Agent` 枚举（加在 About 前）+ `pageIdToString` 分支 | Module 基类签名 |
| `core/claude_proxy_service.h` | `IClaudeProxyService` 加 `resolveModelSlot` 虚方法 + `GlobalClaudeProxyService` 适配器转发 | 既有方法签名 |
| `app_registry.h` | `kModuleRegistry` 加一行 Agent（About 前）+ `create_agent_module` 声明 | 既有 6 行 |
| `settings_config.h` | `kModuleIdAgent` 键 | 既有键 |
| `main.cpp` / `app_assembly` | AgentModule 实例化 + 装配（一行）+ `GlobalClaudeProxyService` 适配器注册（已有模式） | 装配顺序结构 |
| `src/main.cpp` getRouter switch | 加 `case PageId::Agent`（返回 agent 实例，参照 `case PageId::Settings/About` 既有结构，M3） | 既有 switch 结构 |
| `src/module_agent/CMakeLists.txt` | 新模块接线（static lib + 注册到 app，参照 `module_todo/CMakeLists.txt` 风格，M3） | — |
| `tests/test_core/test_router.cpp` `core_pageid_order` | 序变 `Home<Markdown<Todo<ClaudeProxy<Settings<Agent<About`，钉死 `agent=5`/`about=6`（M3，既有断言扩展） | 既有用例结构 |
| `tests/test_modules/test_app_assembly.cpp` | `assembly_registry_table_shape`（size 6→7、表序加 Agent、gatingKey[5]=kModuleIdAgent、[6]=nullptr）+ `assembly_build_gated_entries_real_table`（全开 gated 6→7 条、kEnumOrder 加 Agent）两用例更新（M3） | 既有用例结构 |

**遵循「优先扩展而非修改」（OCP）**：除 `PageId` 枚举加一项、`kModuleRegistry` 加一行、`migrations.h` 加一个 migration、`IClaudeProxyService` 加一个虚方法（这些都是表驱动/接口扩展，非改既有逻辑）外，既有方法体零改动。

**跨模块依赖检查**：`module_agent` 只 include `core/tool.h` + `core/llm_endpoint.h` + `core/claude_proxy_service.h`（core 头）；不 include `module_todo` / `module_markdown` 等头。各 `*Tools.cpp` 也只 include 自己模块的头 + `core/tool.h`。模块隔离守卫（`scripts/check-module-isolation.sh`）扩展加查 `module_agent` 不反向依赖兄弟模块。

---

## 10. 安全与迭代控制（D-7 汇总）

| 风险 | 控制 |
|---|---|
| agent 失控循环 | `max_iterations` 硬上限 50；单 run 墙钟 30 分钟 |
| 烧 token | 单 run token 累计硬上限 200K；落库可审计 |
| 删数据 / 切配置 | 破坏性工具经 `allow_destructive` 授权（=0 直接 fail，不挂起，见 §5.3） |
| 网络异常 | LlmEndpoint 返回 fail → run 标 `failed` + error；不抛 |
| 工具异常（M1 统一） | `ITool::execute` 返回 `Result fail` → `ToolResult.isError=true` 回喂 LLM（让 LLM 自纠），**不中止 run**；若 `execute` 真抛 C++ 异常 → AgentLoop 工作线程 `try/catch` 捕获，落 `run.status='failed'` + error（防 `std::terminate` 崩应用） |
| 并发 | 单文件单写者（ToolRegistry 读写锁）；同一 task 不并发跑两个 run（Scheduler 队列） |
| 持久化 | agent_runs / agent_messages 每轮落库，崩溃可查最后状态（不自动续跑，YAGNI） |
| 悬挂 run 清扫（L2） | 应用启动 / 首次进 agent 页时 `UPDATE agent_runs SET status='failed', error='应用异常退出（悬挂 run）' WHERE status='running'`（崩溃时在跑的 run 标 failed，UI 状态准确） |
| tool_result 体积（L3c） | 单 result 硬上限 32KB，超限截断 + 末尾 `[truncated]` 标记（防 `md.read` 长笔记 + 多迭代烧爆上下文致上游 400） |

### 工具回调线程模型（M3，R4 分层定案）

`ITool::execute` 在 **AgentLoop 工作线程**执行（非 UI 线程）。线程安全按服务实况分层（2026-09-30 逐一核实）：

- `todo.*`：**直调**。`TodoService` 无内存态、纯 DB 访问（`todo_service.h:104` 成员仅 `db_`），`SqliteDb` 的 `recursive_mutex` 已全局序列化全部访问（`sqlite_db.h:93-101`）——工具工作线程直调即安全，零改动。
- `md.*`：**走既有路径**。`MarkdownService` 已有 `rescanMu_` 串行化 watcher 线程与 UI 线程的 rescan（`markdown_service.h:175`）；工具调用复用既有 service 入口，plan 阶段如发现某方法需额外保护，锁加在该 service 内。
- `claude.*`：**service 内加锁（默认方案）**。`ClaudeService` 是 UI 线程单例且被工具触碰的方法无锁（listProfiles / getActiveProfile / switchProfile / testConnection / recentLogs）——实现 `ClaudeTools` 时在这些方法内加锁覆盖其内存态读写。**逃生舱**：仅当某操作本质必须在 UI 线程执行且加锁不可行时，经 EUI-NEO `core::async::runOnce` post 到 UI 线程并同步等待（带超时）——其 `then` 回调已在主线程队列执行（`async.cpp:119` finishTask 入 `gMainQueue`，UI 侧 `dispatchReady` 泵出，已核实）。硬约束：**UI 线程永不等待 agent 线程**（无死锁环）。逐方法加锁位置在 plan 阶段定。
- `app.stats` / `app.read_settings`：直查 SQLite（同 `recursive_mutex` 序列化，安全）。

---

## 11. 测试策略

| 层 | 测试 | 工具 |
|---|---|---|
| core 纯函数 | ToolSchema 拼接、Anthropic 消息累积、tool_use 解析 | 自制 doctest + mock LlmEndpoint |
| AgentLoop | 注入 mock LlmEndpoint 返回固定序列（tool_use×2 → end_turn），断言工具调用顺序 + 消息累积 + 落库 | InMemoryDb + 向真 ToolRegistry 注册 fake ITool |
| 工具实现 | 每个 ITool 用 InMemoryDb 单测（复用 TIN-06 测试 scaffold） | ScopedAppDatabase + HeadlessFrame |
| LlmClient | 注入的 http 发送函数返回固定 /v1/messages 响应，断言响应解析 + 代理预检 + resolveModelSlot 调用 | **std::function 注入缝（R4）**：`LlmClient` 构造注入 `std::function<Result<Response>(const Request&)>`，默认 = 真 `platform::http::send`（已核实其为自由函数，无类可 mock，零 platform 改动）+ fake IClaudeProxyService |
| 集成 | 真实本地代理（起 ClaudeService + fake 上游）跑一次完整 run | 端到端（可选，需真 API key） |
| 迭代控制 | mock 让 LLM 永远返 tool_use，断言达 max_iterations 终止（`status='limited'`） | mock |
| 破坏性授权 | mock 返回破坏性 tool_use，断言 `allow_destructive=0` 时 run `failed`、`=1` 时执行 | mock |

**测试 scaffold 复用**（TIN-06）：`HeadlessFrame` + `InMemoryDb` + `ScopedAppDatabase` + `DataRootEnv`，agent 测试直接用。`LlmEndpoint` 抽象使 agent loop 单测零网络。

**注**：`AgentLoop` 依赖具体 `ToolRegistry`（单例类，非接口）——测试时向真 `ToolRegistry::instance()` 注册 fake `ITool` 即可（register/unregister 成对），不引入第二抽象（YAGNI）。`LlmEndpoint` 是接口可注入 mock。

**R4 核实（请求头归属）**：`anthropic-version` 头由本地代理自动补注入（`upstream_dialect.cpp:161-162`：客户端没带时补 `2023-06-01`），**LlmClient 只需发 `content-type: application/json` + body**；模型名 `[1M]` 后缀剥离同样由代理处理。agent 侧零方言代码。

---

## 12. 设计决策汇总

| 编号 | 决策 | 排除 |
|---|---|---|
| D-1 | 后台自动化（手动 + 定时；事件触发 Phase 2） | 对话窗（用户拍板不要）；事件触发本阶段（core 无事件总线，违反模块隔离） |
| D-2 | 经本地代理发 /v1/messages | agent 直连（重复实现鉴权/failover/方言） |
| D-3 | 只 Anthropic tool use | OpenAI function_calling / 双协议（YAGNI） |
| D-4 | core 加 ITool+ToolRegistry | 扩展 Module 基类 / 全局 service 单例 |
| D-5 | 纯 C++ 零新依赖 | CPR / nlohmann_json / Python / Node / MCP 子进程 |
| D-6 | 仅内置工具 | 用户自定义 / MCP（安全 + YAGNI） |
| D-7 | 迭代硬上限 + 破坏性经 allow_destructive 授权 | 无限循环 / 自动执行破坏性 / 挂起等 UI 确认（后台不合适） |
| D-8 | LlmEndpoint 抽象 + 扩展 IClaudeProxyService::resolveModelSlot | agent 直依赖 platform::http / claude_service.h（不可 mock + 违反隔离） |

---

## 13. 不在本设计内（YAGNI 清单）

- agent 间通信 / 多 agent 编排
- 流式响应（agent 非交互，整响应即可）
- 长对话上下文截断（靠 max_iterations 兜底）；**tool_result 体积有硬上限（32KB），超限截断 + `[truncated]` 标记**（防 `md.read` 长笔记烧爆上下文，L3c；见 §10）
- agent 自动拉起代理（先靠 LlmClient 预检 + run 标 failed 提示）
- 工具权限系统 / 沙箱（破坏性 allow_destructive 已够）
- 用户自定义工具注册
- MCP client
- OpenAI function_calling 协议
- run 崩溃后续跑
- **事件触发（`todo.created` / `md.saved` 等）**——Phase 2 后续：core 无通用事件总线，发射点要改 module_todo/markdown 业务逻辑（违反 §9 零改动 + 模块隔离）；硬上 = core 加事件总线（超出本 spec 范围）。本阶段触发只保留手动 + 定时。

---

## 14. 交付切分（R4）

**M1 核心链路**（端到端闭环，手动触发）：

| 层 | 内容 |
|---|---|
| core | `tool.h`（ITool/ToolRegistry）+ `llm_endpoint.h`（LlmEndpoint）+ `IClaudeProxyService::resolveModelSlot` 扩展 + `GlobalClaudeProxyService` 适配器转发 |
| 数据 | migration v12 三表（§6）+ 悬挂 run 清扫（§10 L2） |
| module_agent | `AgentModule`（最小 UI：任务列表 / 新建表单 / 手动启动 / run 列表与详情最简版）+ `AgentLoop` + `LlmClient` + `AppStatsTool` |
| 工具 | `TodoTools`（6 个）+ `MarkdownTools` M1 部分（`md.search` / `md.read`） |
| 装配 | `PageId::Agent`、`kModuleRegistry`、`kModuleIdAgent`、main.cpp switch、CMakeLists、模块隔离守卫扩展、契约测试更新（router 序 / assembly 表，§9 M3 两用例） |
| 测试 | AgentLoop mock 序列 + 工具 InMemoryDb + LlmClient 注入 http + 迭代控制 + 破坏性授权（§11 全表） |
| 验收 | 手动跑通一次完整 run（如 todo.list → todo.create → end_turn），三表落库可查、UI 可审计 |

**M2 调度与工具补全**：

| 层 | 内容 |
|---|---|
| 调度 | `Scheduler`：cron 解析器（§5.1 R4 语法）+ 定时触发 + 常驻 worker 队列 |
| 工具 | `MarkdownTools` 余下（`md.create` / `md.list_tags` / `md.delete`）+ `ClaudeTools`（4 个）+ `AppTools`（`app.read_settings`） |
| UI | 任务编辑 / 触发配置表单（cron 输入校验）/ run 详情完整展示 |
| 测试 | cron 纯函数表驱动 + Scheduler 注入时钟 + 常驻队列 + M2 各工具 |

切分依据：M1 不含 Scheduler 即可交付完整价值闭环（手动触发→LLM→工具循环→落库审计）；调度与工具补全相互独立、可分别验收。M2 在 M1 合并后续作。

## 15. 工程纪律与工作区（R4）

- **模块化开发（用户纪律）**：每个工具集独立文件、归属各自模块（`TodoTools.{h,cpp}` 归 module_todo、`MarkdownTools` 归 module_markdown、`ClaudeTools` 归 module_claude_proxy、`AppTools` 归 module_settings、`AppStatsTool` 归 module_agent）；`module_agent` 内 `AgentModule` / `AgentLoop` / `LlmClient` / `Scheduler` 各自独立文件。单文件单职责，禁止跨模块 include（隔离守卫脚本查）。
- **工作区**：开发在独立功能分支 `feat/agent-subsystem`（worktree `DevDesk/.claude/worktrees/agent-subsystem`）；实现全部 TDD（红-绿-重构）。
- **worktree 构建坑**：worktree 内 `EUI_NEO_DIR` 默认 `../EUI-NEO` 相对路径会解析到错误位置，cmake configure 必须显式传 `-DEUI_NEO_DIR=/Users/coderstory/dev/gui/EUI-NEO`。
- **基线**：分支起点 = dev `e379950`（与已实测 646 用例的 dev 同码，仅多 .gitignore 一行）；worktree 全量构建 + ctest 作为实现阶段第一个任务实证基线。
