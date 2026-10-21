# AUDIT-05终稿-实现可行性

> **审计员**：C++20 系统开发专家视角（实现可行性 lens）
> **被审文件**：`docs/strategy/05-技术路线图.md`（HEAD 6705a2ed + 四轮融合叠加 + 四条升格）
> **日期**：2026-10-21

## 总结论

**可行-带findings**：新增项在现有代码地基上整体可做，无结构性冲突；发现 3 处依赖顺序问题与 2 项遗漏。

## ① 升格项可行性 verdict

### a. 协议适配缝 — **直接可做**

| 证据 | 说明 |
|------|------|
| `core/tool.h:27-35` ITool 接口 | `schemaJson()` 已是自由串；加 `virtual std::string protocol() const { return "native"; }` 零破口 |
| `core/tool.h:39-62` ToolRegistry | 注册表按 `shared_ptr<ITool>` 管理，ProtocolAdapter 在注册表层包一层即可，不碰 21 个实现类 |
| `src/module_agent/agent_loop.cpp:193` `tools_.allSchemasJson()` | 调用点在 AgentLoop，适配层拦截此处（schemaJson → 协议转换） |

"约一天工作量"评估合理：protocol() 虚函数 + ProtocolAdapter 抽象类骨架 + AgentLoop 接入点 = 半日；21 个既有工具复测 = 半日。MCP 完整实现留 C 期，A 期只埋缝。

### b. 任务状态十态 — **要先补地基**（WAITING_APPROVAL 受阻）

| 证据 | 说明 |
|------|------|
| `src/module_agent/agent_store.h:25` `status` 为 TEXT | 加新状态值零 schema 迁移 |
| `src/module_agent/agent_loop.cpp:160` status 赋值 6 处 | 全为字符串字面量，加新分支直接可行 |

**分态评估**：

| 新态 | 直接可做？ | 理由 |
|------|-----------|------|
| WAITING_INPUT | 可做 | 循环内检测到需要用户输入时 break 设此态即可 |
| WAITING_EVENT | 可做 | Scheduler 已有事件机制（`drainEventQueue`），wait 态 + 事件到达 → 恢复 |
| PAUSED | 可做 | 用户显式暂停 = 设状态 + 持久 cancelToken（`cancelToken` 已是 atomic bool） |
| VERIFYING | 可做 | 自检阶段（D2）设中间态 |
| WAITING_APPROVAL | **受阻** | 需审批流（C 期权限三态）才能进入此态；当前遇破坏性未授权直接 `unauthorized=true; status="failed"`（`agent_loop.cpp:314-318`），无挂起→恢复通道 |

**依赖顺序问题**：十态在 A 期、审批流在 C 期 → WAITING_APPROVAL 会是一个死态（定义存在但永远不可达）。建议 A 期先定义枚举值但标记为"C 期前不可达"，不阻塞其余九态上线。

### c. 多维预算护栏 — **要先补地基**（费用、子任务并发两维）

| 证据 | 说明 |
|------|------|
| `src/module_agent/agent_loop.cpp:36-41` 三常量 | `kMaxIterationsHardCap=50, kTokenHardCap=200000, kWallClockHardCapMs=30*60*1000` |
| `agent_loop.cpp:207,212,236` 三处检查点 | 内联在循环体，未抽 BudgetGuard 类 |

**分维评估**：

| 维度 | 直接可做？ | 理由 |
|------|-----------|------|
| 步骤数 | 可做 | = 现有 iterations，语法糖 |
| 时长 | 可做 | = 现有 wallClock |
| Token | 可做 | = 现有 inTotal+outTotal |
| 工具调用次数 | 可做 | 循环内加计数器（`++toolCalls; if (toolCalls > budget) break`） |
| 费用 | **受阻** | `core/llm_endpoint.h:22-23` `LlmResponse` 只有 inputTokens/outputTokens，无价格字段；费用需模型定价表（端点实现层或配置层提供），当前无此数据源 |
| 子任务并发上限 | **受阻** | `src/module_agent/agent_store.h:14-21` `AgentTaskRow` 无 parent_id；当前任务模型无父子概念；`startRunExclusive` 只防同 task 并发，不知子任务 |

**演进路径**：三护栏 → 六维不是重构而是叠加——前三维保留、后三维追加；但费用和子任务并发是「框架就绪、数据源缺失」的半成品，A 期只能声明维度、值写死上限（如费用=∞、子任务并发=0 即禁用），等数据源补齐后激活。

## ② 设计参考项可行性抽查

| 项 | 证据 | verdict |
|----|------|---------|
| **a. webhook 推本地 HttpServer** | `platform/http_server.h:70-91` Server 已支持 route/start/stop + RequestCtx header/body 全取；需补 HMAC-SHA256 验签逻辑（handler 内自行实现） | **直接可做** |
| **b. 无进展检测** | `agent_loop.cpp:156` `messages` vector 持有全量历史；tool_use block 解析在 `agent_loop.cpp:271-275` 逐块拆 name+input | **直接可做**（工具调用模式检测）；"工作区无新增文件"需 B1 文件工具上线后才能观测 |
| **c. 预算租约** | `agent_store.h:14-21` AgentTaskRow 无 parent_id；`agent_store.h:23-31` AgentRunRow 无 parent_run_id；Scheduler cancel 为单 run 独立 token（`agent_loop.h:59` `cancelToken`） | **与现有结构冲突**：无父子任务概念。应标注"依赖 E 期任务派生机制"而非 D 期 |
| **d. Context Manifest** | `agent_store.h:32-38` AgentMessageRow 四列（seq/role/contentJson/toolCallId）；Manifest 字段（来源类型/版本/Token/可信级/变换方式）与消息行正交 | **直接可做**，建议独立表 `agent_manifests`（粒度=每次 LLM 调用，非每条消息） |

## ③ 依赖顺序问题

| # | 严重 | 问题 | 证据 |
|---|------|------|------|
| 1 | **HIGH** | **WAITING_APPROVAL（A 期）依赖 C 期审批流**。十态枚举在 A 期定义，但 `WAITING_APPROVAL` 的进入路径需要 `per-tool deny/ask` 挂起机制——当前 `agent_loop.cpp:314-318` 遇破坏性未授权直接 `failed`，无挂起语义。C 期才实现权限三态。 | `agent_loop.cpp:314` unauthorized 分支 = 即时失败，无 suspend/resume |
| 2 | **MED** | **预算租约（D 期）依赖 E 期任务派生**。D 期评估"父停子停"预算租约，但当前 `AgentTaskRow`（`agent_store.h:14`）无 parent_id、无子任务 spawn API。任务层次结构在 E 期流水线才首次出现。 | `agent_store.h:14-21` 11 列中无 parent_id |
| 3 | **MED** | **D3 自唤醒的醒来自检依赖 D2 做完自检框架**。自检项（系统时间/心跳/上次任务完成）需 D2 的 verifier 机制；若 D2 延期则 D3 自检退化为硬编码探针——可行但弱于设计目标。 | D3 设计参考 §3：醒来先自检；D2 提供 verifier 框架 |

## ④ 遗漏清单

| # | 来源 | 遗漏项 | 建议 |
|---|------|--------|------|
| 1 | 07 §吸收优先级#4 | **Session outcome 字段**。每次 run 结束后结构化记录"产出了什么"（产物清单+摘要），不只是 status/error。落点：`AgentRunRow` 加 `outcomeJson` 列。07 明确定位 A/D 期，05 未收。 | A 期补收——与"撞闸原因可见"同批做，成本低（一列+TEXT） |
| 2 | 07 §吸收优先级#5 | **工具执行进度回调**。长任务（B 期文件拷贝/Git 克隆）无进度反馈 → 用户看到的是"卡死"。落点：`ITool::execute` 加 `ProgressCallback` 参数或 Context。07 明确定位 C 期。 | C 期评估——B 期上线文件/GitLab 工具后长任务需求立即可见 |

（07 吸收优先级 #1 "权限 Rule 三元组"、#3 "Agent mode 三级" 已在 05 的 C/D/E 期消化，不算遗漏。）

---

**verdict 分布**：直接可做 7 / 要先补地基 3 / 与现有结构冲突 1。最关键的发现：**WAITING_APPROVAL（A 期）的挂起-恢复机制依赖 C 期权限三态**——十态可先上线九态，但必须在十态文档里标注该态的"不可达"窗口期，否则上线即负债。