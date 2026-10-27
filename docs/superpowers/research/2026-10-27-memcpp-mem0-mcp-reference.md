# mem0-mcp 参照实现拆解（用户指定参照）

- 日期：2026-10-27；调研：devdesk-researcher
- 对象：https://github.com/pinkpixel-dev/mem0-mcp（下称 mem0-mcp），v0.8.0（main 分支 @ 2026-10-21 推送）
- 对照文件（只读）：`docs/superpowers/specs/2026-10-27-memcpp-p0-core-spec.md` §4（MCP recall 契约）、`docs/superpowers/plans/2026-10-27-memcpp-proposal-card-design.md`（提案卡片）、`docs/superpowers/research/2026-10-27-memcpp-prior-art-evidence-v2.md` §3（mem0 2026-04 转 ADD-only）
- 来源分级：S=仓库源码（经 WebFetch 摘要转述，关键处带原文引文，均检索 2026-10-27）；A=mem0 官方文档；B=npm/GitHub API。查不到的如实标注"未证实/估算"。

## 结论（先行）

mem0-mcp 是 mem0 的一层**薄封装**（全部源码 6 个文件），自身没有任何记忆算法；三种存储后端（mem0 云 API / Supabase pgvector / mem0 开源版内存库）**全部要求联网与 API key**，"本地模式"默认内存态、重启即丢——对 memcpp「本地离线」轴的参照价值为零，但它的 **16 工具 MCP 面、能力上报模式、stdio 卫生纪律、批量删除确认门槛**有机制级借鉴价值。MIT 许可，无兼容障碍。

---

## 1. 它是什么：架构

```
Claude Code 等 MCP 客户端
        │ stdio (JSON-RPC 2.0)
        ▼
mem0-mcp（TypeScript，薄封装层）
  src/index.ts      工具注册 / 参数解析 / 后端按环境变量三选一
  src/types.ts      类型定义
  src/backends/     base.ts 抽象基类 + cloud.ts / supabase.ts / local.ts 三个适配器
        │                    │                    │
        ▼                    ▼                    ▼
  mem0ai SDK（云）    mem0ai/oss + pgvector   mem0ai/oss（内存向量库）
        │                    │                    │
        ▼                    ▼                    ▼
  mem0 云平台 hosted API   Supabase（自建）    本机进程内存
  （抽取/检索全在云端）   + OpenAI 嵌入模型    + OpenAI 模型（gpt-5-mini 等）
```

- **薄封装证据**：云适配器只做 `import("mem0ai")` + `new MemoryClientClass({ apiKey })`（src/backends/cloud.ts）；本地适配器只做 `import("mem0ai/oss")` + `new MemoryClass(this.config)`，检索、抽取、存储算法全在 mem0 侧（src/backends/local.ts，S 级）。
- **后端选择**（src/index.ts `initializeBackend`，优先级）：`MEM0_API_KEY` 有 → 云；否则 `SUPABASE_URL`+`SUPABASE_KEY` → Supabase；否则 `OPENAI_API_KEY` → 本地；全无 → `process.exit(1)` 直接退出。
- **存储后端**：无 Qdrant/Chroma 用户可选项（锁文件里出现只是 mem0ai 传递依赖）。云 = mem0 官方托管 API；Supabase = pgvector，需用户自跑 SQL 迁移建 `memories`/`memory_history` 表（README，S 级）；本地 = mem0 开源版的**内存向量库**（集合名 `mem0_default_collection`），README 自述 "in-memory and non-persistent by default"、"Development/testing only"（S 级）。
- **嵌入模型**：三条路最终都默认 OpenAI。mem0 官方 Node 快速开始："By default the Node SDK uses local-friendly settings (OpenAI `gpt-5-mini`, `text-embedding-3-small`, in-memory vector store...)"（https://docs.mem0.ai/open-source/node-quickstart ，A 级）。
- **网络与 key**：三后端全部必填 key、全部要联网。理论上可把开源版 LLM/嵌入换成 Ollama 等自托管端点（mem0 上游支持），但 mem0-mcp **未暴露任何此类配置入口**（README/源码均无，S 级）——即开箱即用形态无离线路径。
- **定性**：对 memcpp 的"本地离线"参照价值 ≈ 0；作为"MCP 记忆工具面生态"参照价值高（它是 npm 上被 Smithery/Glama 收录的成套 16 工具实现）。

## 2. MCP 工具面全清单（v0.8.0，src/index.ts，S 级）

| # | 工具名 | 必填入参 | 出参要点 | 后端可用 |
|---|---|---|---|---|
| 1 | `add_memory` | 无（`content` 或 `messages` 二选一） | 新记忆列表 / 超时 pending 消息 / `eventId` | 三后端 |
| 2 | `search_memories` | `query` | 按 score 排序的记忆+元数据 | 三后端 |
| 3 | `search_memory` | `query` | 同上（旧名兼容别名） | 三后端 |
| 4 | `list_memories` | 无 | 分页列表（page 默认 1、pageSize 默认 25） | 仅云 |
| 5 | `get_memory` | `memoryId` | 单条记忆 | 仅云 |
| 6 | `update_memory` | `memoryId` | 更新后记忆（可选 text/metadata） | 仅云 |
| 7 | `delete_memory` | `memoryId` | success 消息 | 三后端 |
| 8 | `get_memory_history` | `memoryId` | 该条记忆的版本历史 | 仅云 |
| 9 | `get_memory_capabilities` | 无 | 当前后端特性矩阵（布尔标志集） | 三后端 |
| 10 | `batch_update_memories` | `updates`（{memoryId,text} 数组） | 批量结果 | 仅云 |
| 11 | `batch_delete_memories` | `memoryIds` + `confirm` | **confirm 必须字面量 `true`**，否则拒 | 三后端 |
| 12 | `rate_memory` | `memoryId`+`feedback` | 反馈登记（positive/negative/very_negative） | 仅云 |
| 13 | `get_memory_event` | `eventId` | 异步事件状态 | 仅云 |
| 14 | `list_memory_events` | 无 | 分页事件列表 | 仅云 |
| 15 | `create_memory_export` | `schema` | 导出任务（可带 filters/instructions） | 仅云 |
| 16 | `get_memory_export` | `exportId` | 取导出结果 | 仅云 |

通用入参：scope 参数 `userId`/`runId`（别名 `sessionId`）/`agentId`/`appId`/`metadata`，缺省回退环境变量 `DEFAULT_USER_ID` 等；搜索附加 `filters`（AND 组合的键值过滤）、`threshold`（默认 0.1）、`topK`（默认 10）、`rerank`（仅云）、`referenceDate`（仅云）。

**错误处理——与我们刻意不同的点**：工具内任何异常 `throw new McpError(ErrorCode.InternalError, ...)`，走 JSON-RPC 协议层错误，"tool results are never wrapped in `isError: true` content messages"（src/index.ts 摘要转述，S 级）；后端初始化失败与未捕获异常均 `process.exit(1)` 静默退出。对照 P0 §4.2（`isError:true` + 进程不崩）：它的后端是无状态远程服务，死了由客户端重拉；我们的库文件是本地唯一真相、MCP 进程死 = 记忆入口断，存活优先级更高——这是有意识的分歧，不是遗漏。参数校验层 `-32602 InvalidParams`（如缺 confirm）两边语义一致。

**对表 P0 §4.2 recall 契约**：
- 值得对齐：主动词形态同构（`search_memories(query 必填, topK)` vs 我们 `recall(query 必填, limit)`），`query` 命名两边一致，保持；scope 走显式可选参数 + 环境变量缺省，与我们 `project` 可选参数同思路。
- 刻意不同及理由：① 它的搜索**强制**至少一个 scope（云 API V3 约束），我们 `project` 缺省不限项目——桌面单用户全库检索是常态，无多租户问题；② 它出参是 score + 裸记忆文本，我们出参行前缀带 `[日期 · 状态 · 置信 · 来源]`（可行性报告 §4.5 "注入内容带时间、过时标记和来源 ID"的落实，封装层无此自觉）；③ 它 16 工具 + 1 别名，我们 P0 只有 `recall` 1 个——工具越多 agent 误选面越大，且 update/delete 类破坏性工具与我们的写入纪律直接冲突（见 §3）。

## 3. 记忆生命周期

- **add**：`infer` 参数默认 true → mem0 用 LLM 先抽取事实再存："By default, Mem0 stores extracted memories, not a verbatim transcript"、"Use `infer=False` when you need to store raw content exactly as provided"（mem0 官方 how-it-works，https://github.com/mem0ai/mem0/blob/main/docs/core-concepts/how-it-works.mdx ，A 级）。新算法抽取路径 "extract memories (ADD only) from input and context"、"The automatic extraction path is additive"（同源）——与我们"写入只 ADD"同向，为 prior-art-evidence-v2 §3 记录的 2026-04 转向补上了 API 层佐证。
- **云 add 是异步事件**：平台返回 `eventId`，适配器每 500ms 轮询 `/v1/event/{id}/`，默认 `waitForCompletion=true`、`timeoutMs=15000`；超时不报错，返回 pending 消息；轮询中瞬时网络错误被吞掉继续轮（src/backends/cloud.ts，S 级）。
- **update / delete**：**显式工具动作，调用方 agent 直接执行，无任何审核**。mem0 官方口径："Use explicit `update` or `delete` operations when your application needs to correct or remove a memory"（A 级）——自动管线只加不改，纠错责任交还应用侧；mem0-mcp 把这对破坏性操作原样递给 LLM，唯一安全门是 `batch_delete_memories` 要求 confirm 字面量 true。**无 `delete_all` 工具**（v0.8.0 无此名，README/源码均未列，S 级）。
- **与我们（ADD-only + 提案卡片人审）的分叉点**：写入纪律相同（都不自动改写旧记忆），**纠错主体不同**——它是"机器改机器存"（agent 直接 update/delete，无审核、无撤销链、物理删除），我们是"人改机器存"（一切变更走提案卡片人工审核，零物理删除）。它恰好证明：业界连"纠错裸奔给 agent"都还没补上人审这环，提案卡片的差异化定位进一步坐实。
- **检索 API**：`search_memories` 语义 + BM25 混合检索（README："hybrid semantic/BM25"，S 级）；过滤维度 = scope id（user/agent/app/run）+ `filters` 元数据键值（AND 组合），**没有独立的 memory_type 维度**（等价物是自定义 metadata 键）；`list_memories` 分页全量（仅云）、`get_memory` 按 id 单取（仅云）。对比我们：FTS5 关键词 + LIKE 兜底、`project` + `status` 过滤、P0 无分页全量动词。

## 4. 可借鉴 Top3 / 应避开 Top3

**抄什么（机制，不抄代码）**：
1. **`get_memory_capabilities` 能力上报模式**：后端返回特性布尔矩阵，不支持的工具返回清晰的 "feature-unavailable" 消息而不是隐藏工具（src/index.ts + base.ts，S 级）。memcpp P1/P3 分层落地时（无向量 → 有向量 → 可选云抽取），MCP 工具面可以保持稳定、按能力自适应——比"每档换一套工具名"抗漂移。
2. **stdio 卫生纪律**：启动即把 console.* 全部替换为空函数、关闭遥测（`MEM0_TELEMETRY=false`）、日志只走 stderr、SIGINT/SIGTERM 先 `server.close()` 再退出；引文 "Suppress library console logging to avoid breaking the MCP stdio protocol"（src/index.ts，S 级）。P0 §4.2 已定"进程不崩"，应补"库日志静默 + 优雅关闭"两条进实现清单——这是它踩坑两轮（CHANGELOG 0.1.7/0.2.5 两连修 stdout 污染）换来的。
3. **异步写入确认契约**：`waitForCompletion`/`timeoutMs`/`eventId` 三态（已落库 / 处理中 / 失败）。P0 用不上；P3 若上云抽取，提案状态机与 MCP 出参可直接抄这个语义，避免"写完不知道成没成"。

**避开什么**：
1. **云优先与 key 门槛**：三后端全要 key 全要网，"本地模式"默认内存态不持久。正面撞我们"本地离线"轴；反过来看它也证明——"SQLite 单文件 + 零网络 + 重启不丢"在 MCP 记忆生态里是空位，memcpp 的定位有差异化空间。
2. **16 工具胖面板 + 兼容别名**：`search_memory` 是 `search_memories` 的纯别名（生态包袱的活标本）；破坏性工具（update/delete/批量删）直接暴露给 agent，与我们写入纪律冲突。我们坚持 P0 单工具，未来加工具也只加只读动词。
3. **依赖质量**：直接依赖仅 2 个（`@modelcontextprotocol/sdk` 1.29.0、`mem0ai` ^3.0.10），但 mem0ai 传递拖入约 **200+ 个包**（锁文件可见 Azure/Google/Anthropic/Qdrant/Redis/LangChain/Express 全家桶，含 better-sqlite3 原生编译；估算值——锁文件抓取被截断，未精确计数）。memcpp 单 exe 全静态（objdump 只剩系统 DLL）在"无人值守桌面常驻"轴上优势明确：不要求用户装 Node、无 npm 供应链面。

## 5. 许可证与依赖重量

- **License：MIT**（GitHub API `license.spdx_id: "MIT"` + 库内 LICENSE 文件，S 级）。与 DevDesk 兼容，无 GPL 传染问题。
- **维护活跃度**：建库 2025-03-12，最近推送 2026-10-21（调研日前 6 天，活跃）；最新版 0.8.0 发布 2026-06-24（CHANGELOG，S 级）；96 stars；npm 月下载 760（api.npmjs.org，B 级，其返回统计周期为 2026-06-02~07-01，口径存疑仅作量级参考）；open issues 0、open PR 3（GitHub API，B 级）。单人维护体量、社区规模小——作参照可以，作长期依赖需谨慎（我们本也不依赖它）。
- **依赖重量量化**：它 = Node 运行时 + 约 200+ npm 包（估算）；memcpp = 单 exe、SQLite amalgamation 源码并入、零运行时依赖。差距两个数量级（10^2 包 vs 0）。

## 对既有文档的修订建议

1. **P0 规格书 §4.2 错误处理条**：补半句"生态对照：mem0-mcp v0.8.0 走 McpError 抛 JSON-RPC 错、初始化失败即退进程；本方案 isError:true + 进程存活是刻意分歧——本地库是唯一真相，MCP 进程死 = 记忆入口断"，防后续审读者当遗漏。
2. **P0 规格书 §4.2 实现清单**：补两条 stdio 卫生——console.* 静默只留 stderr、SIGINT/SIGTERM 优雅关闭（mem0-mcp 在 CHANGELOG 0.1.7/0.2.5 两连修 stdout 污染踩坑后验证的模式）。
3. **prior-art-evidence-v2 §3**：补一行工具面层证据——"mem0-mcp v0.8.0 仍把 update/delete 直接暴露给 agent（无审核），仅 batch_delete 有 confirm:true 门槛；'自动管线 ADD-only、纠错交 agent 裸奔'是上游生态现状"，为提案卡片人审定位再加一钉。
4. **B4 提案卡片**：如需生态佐证，`batch_delete_memories` 的 confirm:true 可引为"破坏性操作最低配安全门"实例（比提案卡片弱两级：不审内容、不留撤销链、不防单条 delete_memory）。
5. **不建议对齐其工具命名**：`search_memories`/`topK`/`threshold` 是向量检索生态惯用形；memcpp P0 是 FTS 关键词检索，`recall(query, limit)` 保留即可。若 P1 上向量检索，再给 recall 增补可选 `topK`/`threshold` 参数（向生态形参靠拢），而不是改主动词名。

## 引用清单（均检索 2026-10-27）

S 级（pinkpixel-dev/mem0-mcp 仓库本体）：
- 仓库主页 https://github.com/pinkpixel-dev/mem0-mcp（stars/license/推送时间/三后端说明）
- src/index.ts（16 工具注册、McpError 错误处理、后端选择、stdio 卫生）
- src/backends/cloud.ts（异步事件轮询、infer 透传）、src/backends/local.ts（内存向量库、能力矩阵）、src/backends/supabase.ts、src/backends/base.ts（能力上报抽象）
- package.json / package-lock.json（依赖清单与传递树）、CHANGELOG.md（版本史）、README.md（安装与三模式）
A 级（mem0 官方文档）：
- https://docs.mem0.ai/open-source/node-quickstart（Node SDK 默认 OpenAI 模型 + 内存向量库）
- https://github.com/mem0ai/mem0/blob/main/docs/core-concepts/how-it-works.mdx（infer 语义、ADD-only 抽取、显式 update/delete 口径）
B 级（API，量化数据）：
- https://api.github.com/repos/pinkpixel-dev/mem0-mcp 、https://api.npmjs.org/downloads/point/last-month/@pinkpixel/mem0-mcp

未证实项：锁文件包数（约 200+）为截断估算；npm 下载统计周期口径存疑；v0.7 之前版本是否曾有 delete_all 工具未查（v0.8.0 现状确认无）。
