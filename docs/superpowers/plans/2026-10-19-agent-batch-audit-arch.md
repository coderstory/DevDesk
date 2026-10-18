# 智能体批架构审计（软件架构师 lens）— 2026-10-19

- **范围**：`git diff 033d7db..d954cab -- src/ tests/`（36 commits，+6519/−809，25 文件）
- **口径**：只读、零构建；每条 finding = `file:line` + 问题 + 维护成本影响 + 建议，severity 按演进代价
- **环境备注**：本 worktree（`worktree-agent-a34cd893053660ab4`）创建时落在 master 旧点 `479f5fe`（10-14，落后 243 commits，属「worktree 隔离静默失效」已知形态）；已 `--ff-only` 快进到 `d954cab` 后再取 diff——直接按任务给的 `033d7db..HEAD` 在旧点上会得到 243 个提交的**逆向 diff**，结论全部失真。审计基线以此为准。

## Findings

### F1 · HIGH — agent_module.cpp 上帝类继续膨胀，本批又 +30%

- **位置**：`src/module_agent/agent_module.cpp`（全文件 3279 行；033d7db 时 2516 行，本批 +1113/−350）；`src/module_agent/agent_module.h`（405 行，约 40 个私有成员）
- **问题**：单类同时承载：页面骨架 `compose`（430 行，:1546-1976）＋ 任务弹窗 `composeTaskDialog`（393 行，:1976-2369）＋ 草稿子弹窗 `composeDraftDialog`（:2369-2468）＋ DraftJob 线程机制（:169-178 声明、:2494-2631 实现）＋ 删除确认弹窗（:2635+）＋ 详情弹窗 `composeTaskDetailDialog`（542 行，:2707-3249）＋ run 轮询/迁移检测/双通道通知（:1125-1409）＋ EventBus 订阅。四个弹窗合计约 1465 行 = 全文件 45%。
- **维护成本**：本批 36 个 commit 至少 8 个改这一个文件（P1 布局壳、C1 停止、E1 删除、D2 通知、level 参数、表单提示批、状态视觉批）——它已是派单碰撞热点，同文件多 agent 只能串行；已排期的详情弹窗 P2/P3 任务卡还会再进同一 TU；每次触碰 3279 行全量重编译。头文件 40 个成员横跨四张弹窗 + 轮询 + 草稿机制，读任何一个弹窗的行为都要通读整个类。
- **建议**：沿本批已开始的抽取惯性（task_draft_parse.h 已把纯函数搬出）继续拆三块：① `composeTaskDialog` + `edit*` 草稿字段聚成 `TaskDialogState` → `agent_task_dialog.{h,cpp}`；② `composeTaskDetailDialog` → `agent_detail_dialog.cpp`；③ DraftJob + `startDraftGeneration`/`reapDraftJob`/`applyDraftPrefill` 是与 UI 无关的自洽机制（线程 + epoch + 锁纪律），抽 `task_draft_job.{h,cpp}` 可独立单测。成员声明序契约（`scheduler_` 必须在 `loop_/llm_/store_` 之后，agent_module.h:256）随成员迁移时原样带走。

### F2 · MEDIUM — run 结束通知文案双写，两通道会漂移

- **位置**：`src/module_agent/agent_module.cpp:1318-1333`
- **问题**：同一段里 `"智能体任务失败"/"智能体任务受限"` 三元式和 `"「" + taskName + "」运行结束（" + runStatusText(st) + "）"` body 表达式各写了两遍——一份进 `pendingNotices_`（app toast），一份进 `platform::notify::Notification`（OS 通知）。
- **维护成本**：改文案必须双改；漏一处 → toast 与系统通知说法不一致；钉文案的测试也要钉两遍。D2 是新链路，正是文案最可能再调的时候。
- **建议**：循环体内提 `const std::string title = ...; const std::string body = ...;` 两行，两个通道复用——它们本就是同一通知的两种投递，不是两份文案。

### F3 · LOW — ASCII trim 同模块三副本 + 一处过时注释指向旧真源

- **位置**：`src/module_agent/app_notify_tool.cpp:21`（私有 trim 副本，注释「与 agent_module.cpp 的 trimCopy 同口径」）；`src/module_agent/task_draft_parse.h:37`（共享 `trimCopy`，本批新真源）；另 `src/platform/notification.cpp:111` `trimAscii`（platform 层私有，合理保留）
- **问题**：闹钟校验搬迁后 trim 真源已从 agent_module.cpp 移入 task_draft_parse.h，但 app_notify_tool.cpp 的注释还指向 agent_module.cpp（已过时）；且两者同属 module_agent，app_notify_tool.cpp 完全可以 include task_draft_parse.h 复用（头文件纯 inline，无新依赖）——task_draft_parse.h:18「同名副本不合并：模块间互不 include」的理由对这一对不成立，真正成立的原因只是它想避免拉进 agent_store.h 依赖，注释没这么说。
- **维护成本**：错误理由会误导后续 AI 认为跨文件合并被禁止而永不收敛（注释纪律：注释必须帮 AI 少犯错，失真理由是负资产）；同模块两份同口径 trim，改空白口径要两处同步、三处对账。
- **建议**：app_notify_tool.cpp 改用 task_draft_parse.h 的 `trimCopy` 并把注释改为指向新真源与真实取舍；facade 内副本不动（platform/module 层边界，共享反而要新建 core 字符串工具，不值）。

## 已核（无 findings 的面）

- **分层边界**：新通知链全部 module→platform 合法方向（`agent_module.cpp:1335`、`app_notify_tool.cpp:207`，无旁路直调后端）；task_draft / task_draft_parse 纯逻辑层零 UI/零 DB 依赖，耦合方向 UI→逻辑单向（task_draft.h 不知晓 AgentModule）；平台差异继续按文件拆（notification_macos.mm / _win.cpp / _posix.cpp + facade notification.cpp），业务代码零平台宏。已核。
- **Receipt 语义**：`notification.h:30-56` 契约文档充分——Accepted≠用户看到（Focus/勿扰静默抑制）、Deduped/RateLimited 仅 facade 产生、后端状态域五值、`Result::fail` 只留清洗失败与内部不变量；`notification.cpp:6-8` 后端调用在锁外（Windows 可阻塞 1.5s）。已核。
- **platform::notify facade 与 app_notify_tool 职责分界**：干净——工具层只做参数校验 + Receipt→JSON 映射（app_notify_tool.h:1-12 契约头注），清洗/截断/去重/限速/授权全在 facade，零逻辑复制；降级 fallback 经构造注入、线程契约写明（工作线程入队、不触 UI）。已核。
- **ToolGuard**：纯测试侧 helper（`tests/test_modules/test_agent_tools.cpp` / `test_agent_ui.cpp`），本批 src 零改动；2750e51 只修测试 UB（string 迭代器对构造）。已核。
- **trigger_config JSON 形状**：encode/decode 单真源（trigger_config.h）；草稿侧 `task_draft.cpp:152` 经 `encodeTriggerConfig` 产出（LLM 不直出载荷 JSON，解析层只产结构体）；消费侧 applyDraftPrefill（agent_module.cpp:2606）与 Scheduler、列表徽章判定同一 `decodeTriggerConfig`。已核。
- **可测试性**：注入点齐——`Deps{db, profileSource, http, schedulerNow}` 四缝（agent_module.h:102-110）；notify `setBackendForTest`/`setClockForTest`/`resetForTest`（notification.h:106-123）；兜底轮询时钟走 schedulerNow 缝（agent_module.cpp:1152）测试可冻结；`parseTaskDraft` 唯一一次真时钟读是头文件文档化决策（task_draft.h:61-63，用绝对时刻构造用例）。已核。
- **草稿弹窗 vs 任务弹窗表单重复**：怀疑不成立——子弹窗只有输入 + 两钮 + 三状态行，无表单逻辑；预填只写共享 `edit*` 字段（`applyDraftPrefill`），校验共用 task_draft_parse 抽取层，无第二份表单实现。已核。
- **过度设计**：未发现。`Receipt.retryAfterMs` 有消费、`editMaxRuns_` 保持整数有往返论证（agent_module.h:330-333）、`backendSlot` 原子槽有并发理由与测试复位路径。已核。
- **store deleteTask（E1）**：单事务显式三条 DELETE（子→父）、不押 FK CASCADE pragma、NotFound 契约三表零变化（agent_store.cpp:257-306）；statement 样板重复属 store 层既有风格，非本批新债。已核。
