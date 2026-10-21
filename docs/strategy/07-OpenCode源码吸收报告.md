# 07 OpenCode 源码吸收报告

**数据来源**：本地克隆 `D:\project\gui\opencode2`（anomalyco/opencode，dev 分支）
**版本**：2.1.1（package.json 确认）
**阅读日期**：2026-10-21
**技术栈**：Bun + Effect-TS + TypeScript，monorepo（30+ packages）

---

## 1. 权限模型

**机制**：三段式规则引擎——每 Agent 带 `permissions: Rule[]`，每 Rule = `{action, resource, effect}`（effect 取 allow/deny/ask），最后匹配胜出，action 与 resource 均支持 `*` 通配。评估链路：Agent 配置 rules → Session 级 rules → 用户"始终允许"的 saved rules → Plugin 钩子可改写 final effect。缺 Agent 配置时默认全 deny，内置 Agent 默认全 allow（除 `.env*` 文件 ask）。

**源码出处**：`packages/schema/src/permission.ts`（Rule/Ruleset 定义）；`packages/core/src/permission.ts:87-97`（evaluate 函数）；`packages/core/src/permission.ts:173-189`（evaluateInput 组装链路）；`packages/schema/src/agent.ts:34-53`（Agent 默认 permissions）

**对 DevDesk 的启示**：**C 期**直接可用。我们目前只有一个全局 `allow_destructive` 开关，OpenCode 的 `action × resource → effect` 三元组 + wildcard 匹配是成熟方案。搬三样：(1) `Rule` 数据结构与 evaluate 算法（evaluate 只有 7 行）；(2) saved rules（用户答"始终允许"后持久化为规则追加到列表末尾）；(3) Plugin hook 改写最终 effect 的拦截点。

**为什么不照搬会话级 permissions**：DevDesk 目前不存在"不同会话不同权限"的需求（用户态的 agent 运行时权限是 agent 属性不是会话属性），A 期先只做 Agent 级，会话级在 B 期对接会话后视需要加。

---

## 2. Agent 角色体系

**机制**：Agent = `{id, name, model, request(provider settings), system(系统提示词), description, mode, hidden, color, steps(步数上限), permissions}`。mode 分三档：`primary`（用户可切换，Tab 键）、`subagent`（内部调用，不暴露给用户）、`all`（两者兼备）。内置 build/plan/general——build 全权限默认、plan 只读（deny 编辑但 ask bash）、general 作 subagent 用。用户自定义 Agent 放 `.opencode/agent/*.md`，YAML frontmatter 声明上述字段，正文为 system prompt。

**源码出处**：`packages/schema/src/agent.ts:22-54`（Agent.Info 类型）；`packages/schema/src/config/agent.ts`（用户可配置字段）；`packages/core/src/agent.ts:74-86`（selectable/default 解析逻辑）；`.opencode/agent/triage.md`（用户自定义 Agent 实例）

**对 DevDesk 的启示**：**D 期自主性 + E 期流水线**。D2"验收自评"可照 plan Agent 思路=只读 tool 子集 + 提示词教它审代码不写代码；E 期 PM/QA/Dev 三角色直接用 mode 区分 + 各自的 permissions/tools 白名单。搬三样：(1) `mode: subagent | primary | all` 三级暴露控制；(2) Agent 级 model 绑定（plan 可绑便宜模型省成本）；(3) steps 步数上限防 loop。不搬 hidden/color——DevDesk 桌面 UI 有自己的可见性/主题体系。

---

## 3. 工具定义 schema

**机制**：双层设计——(1) `@opencode/ai` 层的 `Tool.make()` 定义（parameters + success 的 Effect Schema，execute 是 Effect handler），namespace 支持，tool 通过 `toDefinitions(tools)` 转成 LLM 可见的 ToolDefinition[]；(2) `@opencode/schema` 层的 `Tool.Info` 接口，含 Context（sessionID/agentID/messageID/callID/progress），执行上下文可以发进度更新。支持两种模式：Typed（编译期 Effect Schema）和 Dynamic（运行时 raw JSON Schema，用于 MCP/插件等外部工具）。

**源码出处**：`packages/ai/src/tool.ts:38-135`（Definition 类型 + Tool.make 实现）；`packages/ai/src/tool-runtime.ts:24-54`（dispatch 执行链路）；`packages/schema/src/tool.ts:15-28`（Context 含 progress 回调）

**对 DevDesk 的启示**：**B 期对接 + C 期扩展**。B 期：我们的工具注册表现状是硬编码 enum，搬 `name + description + parameters schema + execute handler` 四元组结构。C 期：Dynamic 模式对 MCP/插件接入是关键——外部工具带 raw JSON Schema 进来，不需要 DevDesk 编译期知道类型。不搬 Effect Schema（C++ 无等价物，我们用 JSON Schema 验证即可）。

---

## 4. 会话与持久化

**机制**：Session = `{id, parentID, projectID, agent, model, cost, tokens, outcome, time(created/updated/idle/viewed/archived), title, location, subpath, metadata, permissions, revert}`，支持 fork（从历史某点分叉出新会话）。每条消息独立 ID（msg_ 前缀），按时间排序。存储层为 SQLite（`database.path` 配置），REST API 暴露 list/update/export/import。会话可 resume（continue）、可非交互执行（run --prompt）。

**源码出处**：`packages/schema/src/session.ts:32-60`（Session.Info）；`packages/schema/src/session-message.ts:22-36`（消息 ID 与基类）；`packages/cli/src/commands/handlers/session/list.ts`（REST API：按 project+parentID 查询、分页、支持 json/table 输出）

**对 DevDesk 的启示**：对照我们已有的 agent_runs/agent_messages 三表审计——(1) 我们缺 `outcome` 字段（标识会话最终成功/失败/中断），A 期补上；(2) 我们缺 `fork` 机制，D 期"方案 A vs 方案 B 对比"需要；(3) 他们 cost/tokens 字段设计合理——B 期对接 LLM 后照搬；(4) 我们不搬 parentID（DevDesk 不设父子会话层级）。

---

## 5. ACP（Agent Client Protocol）

**背景**：ACP 是 `@agentclientprotocol/sdk` 定义的标准化 agent 通信协议，基于 JSON-RPC + ndJSON 流。OpenCode 的 ACP 实现把一个 OpenCode server 进程包装成一个标准 ACP agent，对外暴露 `agent({name:"opencode"})`，任何 ACP 兼容客户端（如 OpenHands Agent Canvas）都可驱动 OpenCode。协议定义 agent 能力（capabilities）、工具目录（catalog）、会话（sessions）、回合（turns）、权限询问（permission request）等标准方法。工具按 kind 分类：execute（shell）、edit（write/patch/edit）、search（grep/glob/context7）、read、think（subagent）、fetch（webfetch）。

**源码出处**：`packages/cli/src/acp/agent.ts:58`（`agent({name:"opencode"})` 入口）；`packages/cli/src/acp/tool.ts:16-39`（toToolKind 映射）；`packages/cli/src/acp/permission.ts:39-43`（permission options 三选：once/always/reject）

**对 DevDesk 的启示**：**仅作背景情报，不构成路线图承诺**。ACP 本质是"把不同 AI coding agent 当可替换后端"——趋势上值得关注，但 DevDesk 当前形态是 C++ 桌面应用内置 agent loop，没有"对外暴露标准 agent 接口"的需求。E 期若要做"PRD→MR 流水线的可替换执行引擎"，ACP 是天然插槽。

---

## 吸收优先级 Top 5

| # | 搬什么 | 对哪期 | 理由 |
|---|--------|--------|------|
| 1 | 权限 Rule 三元组 + evaluate（`action × resource → allow/deny/ask`） | C 期 | 7 行算法，直接解决我们"全局开关"太重的问题 |
| 2 | 工具注册四元组（name + description + jsonSchema + handler） | B 期 | 替换硬编码 enum，Dynamic 模式天然支持 MCP/插件 |
| 3 | Agent mode 三级（primary/subagent/all）+ 各自 tool 白名单 | D/E 期 | D2 验收自评、E 期角色切分直接套 |
| 4 | Session outcome 字段 + fork 机制 | A/D 期 | outcome A 期补，fork D 期做方案对比用 |
| 5 | tool 执行进度回调（Context.progress） | C 期 | 长任务（git clone、全量构建）实时进度推 UI |

## 明确不搬清单

- **Effect-TS Schema 类型系统**——C++ 无等价物，改用 nlohmann/json schema validation
- **Agent hidden/color 属性**——DevDesk 桌面 UI 自有可见性/主题体系
- **Session parentID 父子层级**——DevDesk 无父子会话需求
- **Bun 专属 API（Bun.stdin/write 等）**——平台绑定
- **Server-client 分离架构**——DevDesk 是单进程桌面应用，agent loop 在主线程内，不走 HTTP 服务
- **ACP 协议全栈实现**——仅作趋势参考，当前无对外暴露标准 agent 接口的需求
- **SQLite 存储层细节（effect-drizzle-sqlite ORM）**——我们已有自己的 SqliteDb wrapper
- **Plugin 系统（plugin hooks 改写权限/compaction 等）**——DevDesk 模块系统已覆盖扩展点

## 补充发现：server 端 Agent loop

本地 clone 中 `packages/server/src/` 源码完整可用（handlers/ 下 34 个文件）。核心 handler：`session.ts`（会话 CRUD）、`message.ts`（消息流）、`permission.ts`（权限断言）、`shell.ts`（命令执行）、`generate.ts`（LLM 生成入口）。agent loop 实际流程：客户端 POST 消息 → `message.ts` 注入 inbox → server 按 agent.permissions 断言工具权限 → 调 LLM（generate.ts）→ 收到 tool call → dispatch 到对应 handler → 结果回填消息流 → 循环直到 model 返回 text 或无更多 tool call。与我们自研 loop 的核心差异：他们的 loop 是 server 内异步事件流（Effect Stream），我们的 loop 会是主线程同步状态机——两者结构可比但实现路径不同，后续可对照 message.ts 和 generate.ts 做细化吸收。