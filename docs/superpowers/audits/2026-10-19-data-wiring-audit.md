> **存档说明**（非原文内容）：来源：只读审计子代理 2026-10-19 产出，原文照录（HTML 转义已还原）。作为《DevDesk 微信风格 UI/UX 整改》的数据面与接线点权威证据；工单与文档引用本文时只许引用行号原文，不许转述。

# DevDesk UI/UX 整改前数据面判定与接线点审计（只读）

## A. 数据面判定

**A1. Todo 任务字段（todo_tasks 全列穷举，schema.h:68-82，共 12 列）**：`id / column_id / title / description / priority / due_at / tags / position / completed_at / created_at / updated_at / archived_at`

| 项 | 字段有无与证据 | 判定 |
|---|---|---|
| 优先级 | 有：schema.h:73 `priority INTEGER NOT NULL DEFAULT 0`；Task 模型 todo_service.h:32（枚举 :16）；解析 todo_service.cpp:124-134。卡片当前未渲染它（todo_ui_board.cpp 卡片段无 priority，仅截止/归档/标签 chip） | **纯展示可做** |
| 截止时间 | 有：schema.h:74 `due_at INTEGER`；todo_service.h:33；解析 todo_service.cpp:142-147。卡片已在画（todo_ui_board.cpp:499-509 截止 chip，逾期红 :505） | **纯展示可做**（已在展示，只余增强） |
| 负责人 | **无**：上表 12 列穷尽无此列；全 src grep `assignee|owner|负责人` 仅命中 file_perms/github_api（非 todo） | **要动数据面**（加列+迁移，本期不做） |
| 标签 | 有：schema.h:75 `tags TEXT`（JSON 数组）；todo_service.h:34；卡上已展示前 2 个+溢出+N（todo_ui_board.cpp:526-545） | **纯展示可做** |
| 已归档状态 | 有：schema.h:80 `archived_at INTEGER` + 归档列标记 `kind='archive'`（schema.h:56，判据走 kind 不走值，todo_service.cpp:665）；Task 模型 todo_service.h:37；卡上归档时间 chip todo_ui_board.cpp:513-525 | **纯展示可做** |

补充：todo 列表 SELECT 即取全部内容列（todo_service.cpp:370/398）；todo 无补列迁移（schema.cpp:121-130 仅迁 agent 三表）。

**A2. 首页指标卡（5 张，kStatCards home_module.cpp:176-182）数据来源全列**：

| 卡片 | 取数链（file:line） |
|---|---|
| Token 速率 | 主数字=lastRate_.tps（home_module.cpp:413-415）← HomeService::getLastCallRate 取 claude_call_logs 最新成功行（home_service.cpp:263-271）；sparkline=core::proxy_stats::rateSeries(24)（home_module.cpp:441-442，2s 窗实时内存态，home_service.h:62） |
| 今日笔记 | COUNT markdown_index WHERE modified_at≥今日零点（home_service.cpp:167-169） |
| 待办任务 | COUNT todo_tasks WHERE completed_at IS NULL（home_service.cpp:174-175） |
| 逾期任务 | COUNT todo_tasks + core::overdue::kWhereSql（home_service.cpp:180-183；谓词单源 overdue.h:12-13） |
| API 调用 | COUNT claude_call_logs WHERE started_at≥今日零点（home_service.cpp:188-190） |
| （另）供应商余量卡 | listQuotaHomeRows（quota_home_view.cpp:76）← profile_quota_config/profile_quota_result 两表（schema.h:182-223），首页经缓存 refreshQuotaRows（home_module.cpp:567/833） |

**↑↓ 趋势箭头判定**：DashboardStats 无任何"昨日"字段（home_service.h:43-49 仅 4 个统计值+recentNotes），getStats 只算今日（home_service.cpp:159-208）。但历史行数据在库里完整存在（claude_call_logs.started_at schema.h:139-141、markdown_index.modified_at schema.h:27、todo_tasks.created_at/completed_at schema.h:78-79）→ 昨日值可由**新增只读 SQL** 从既有行算出，不加列、不动表。**判定：不动库表，但需在 HomeService/HomeModule 新增查询与对比逻辑 = 服务层代码新增，非纯 UI 改动，属边缘情况**——按"数据模型已有对应数据"口径可做，按"只改展示层"口径超范围，建议按边缘项单独裁决。

**A3. Claude 切换器**：

| 项 | 结论与证据 |
|---|---|
| "1M" 上下文标记来源 | claude_profiles.models_config（JSON 文本列，schema.h:120）内模型名尾部 `[1M]` 后缀（常量与判定 provider_config.h:72-77）；落库 handler claude_module.cpp:564/581（setOneMMarker）；**纯展示可做**。当前 UI：详情页复选框（claude_ui_detail.cpp:234-236，经 a.setOneM claude_ui_detail.h:69-71）；列表摘要显示层剥掉 [1M]（claude_ui_logic.cpp:42，claude_ui_list.cpp:254） |
| 全局代理运行状态 | **内存态、不入库**：proxy_runtime.cpp:98 `server_ && server_->isRunning()`，经 claude_module.cpp:88 `svc->isProxyRunning()` 读。当前 UI：header 状态点+「运行中 <port>」/「已停止」（claude_ui_list.cpp:297-316）+ 启停按钮（:343） |
| Profile 选中状态 | DB 列 claude_profiles.is_active（schema.h:117，全局唯一索引 claude_service.h:5）。当前 UI：卡片边框/底色（claude_ui_list.cpp:145-148）+ "ACTIVE" 胶囊（:202-215） |

## B. 接线点核实

**B4. 空状态快捷按钮的既有入口**（各模块空态现状：todo 空板纯文案 todo_ui_board.cpp:195；markdown 空态纯文案指下方按钮 markdown_module.cpp:615；agent 纯文案 agent_module.cpp:1765；claude 空态文案+下方三键 claude_ui_list.cpp:460）：

| 入口 | 处理器位置（file:line） |
|---|---|
| 新建笔记 | `openNewNoteDialog()`：按钮 markdown_module.cpp:650（树底 "+ 笔记" :641-651，页头 :456）；弹窗 :2315-2380（commitNewNote）；跨页负载 "action:new_note" 处理 :2671 |
| 新建文件夹 | markdown_module.cpp:661-668（"+ 文件夹" onClick 置 newFolderOpen_）；弹窗 drawNewFolderDialog :2384+；服务 MarkdownService::createFolder（markdown_service.h:122） |
| 新建任务（Todo） | addTaskClicked：todo_ui_board.cpp:145 → todo_module.cpp:793-798（openNewTaskDialog，无列先报错 :794-796）；跨页负载 "action:new_task" todo_module.cpp:543 |
| 新建任务（Agent） | agent_module.cpp:1591 "+ 新建任务" onClick → `openNewTaskDialog()` |
| 新建看板列 | addColumnClicked：todo_ui_board.cpp:134 → todo_module.cpp:790-792 `openNewColumnDialog()`；弹窗 todo_ui_dialogs.cpp:139 |
| 新建 Profile | claude_module.cpp:430 onNewProfile → `openCreateForm`；按钮 claude_ui_list.cpp:569；快捷键 Ctrl+N claude_module.cpp:392 |
| 导入（cc-switch） | claude_module.cpp:434-436 onImportCcSwitch → `openCcSwitchImportDialog`；按钮 claude_ui_list.cpp:589-596；另有「导入当前配置」claude_module.cpp:431-433 `startImportCurrentConfig`（按钮 claude_ui_list.cpp:574） |
| 新建配置片段 | claude_module.cpp:449 onNewShared → `openSharedCreate`；按钮 "+ 新建片段" claude_ui_list.cpp:708-714 |

**B5. 新建任务弹窗（Agent 任务弹窗，唯一带 AI 草稿的弹窗）**：
- 「AI 生成草稿」chip：agent_module.cpp:2023-2025 onClick → `openDraftDialog()`（声明 agent_module.h:212）；子弹窗「生成草稿」按钮 :2414-2423 → `startDraftGeneration()`（task_draft_job.cpp:22，worker 走 task_draft.cpp 构提示 → DirectLlmClient，上游模型名剥 [1M] direct_llm_client.cpp:218）；结果**只预填表单、永不落库**（task_draft_job.cpp:129-137）。
- 保存空目标校验：**有**——agent_module.cpp:786-789 `goal.empty()` → `editError_="请先填写任务目标"`，不落库不关窗。但 AgentStore::createTask（agent_store.cpp:103-125）自身无空校验直接 INSERT（schema goal TEXT NOT NULL，SQLite 空串合法）——即弹窗层挡住、store 层不挡，经弹窗无法存入空目标。
- 参考：Todo 新建任务弹窗为纯标题表单（todo_dialogs.h:90-103，无优先级/截止输入），空标题校验 todo_module.cpp:186；优先级/截止在详情弹窗编辑（todo_dialogs.h:161-162）。

**B6. 「逾期 N」计数**：
- 计算：todo_module.cpp:134-143 `refreshOverdueCount()` = `service_->listOverdue().size()`（todo_service.cpp:784-792；谓词单源 core/overdue.h:12-13「有截止且<now且未完成」）。
- 展示：todo_ui_board.cpp:107-123「逾期 N」按钮（N=0 置灰）。
- 点击行为（既有处理器）：todo_module.cpp:787-789 overdueClicked → `openOverdueDialog()`（:146-160）→ 弹窗列逾期任务（todo_ui_dialogs.cpp:410-426，行点击开详情）。**判定**：「点击查看逾期清单」的处理器已存在（零新逻辑）；若需求是「点击后在看板内过滤显示」则是新逻辑（现形态是弹窗、非板内筛选）；首页逾期卡点击仅跳页无筛选负载（home_module.cpp:532-540）。

## 覆盖度声明

- **已查范围**：DevDesk/src 全量 grep（core/module_todo/module_home/module_markdown/module_claude_proxy/module_agent/module_settings）；todo_tasks/todo_columns/claude_profiles/agent_tasks 等建表 DDL 逐行读毕（schema.h），补列迁移 schema.cpp 读毕（仅 agent 三表，todo/claude 无迁移）。
- **搜索词清单**：`CREATE TABLE`、`assignee|owner|负责人`、`1M|[1M]|context_note`、`昨日|趋势|today|昨天|compare`、`running_|isRunning|proxyRunning|运行中|全局代理`、`is_active|isActive|激活|使用中`、`新建笔记|新建文件夹|新建任务|新建列|新建 Profile|新建配置片段|导入`、`空状态|暂无|还没有|empty`、`逾期|overdue`、`goal.empty|目标不能为空|saveTask|commitTask`、`priority|Priority`(module_todo)、`action:new_note|action:new_task`、`onImportCcSwitch|onNewShared|新建片段`、`getLastCallRate|listQuotaHomeRows`。
- **可能遗漏**：① 未读 tests/、docs/、3rd/、EUI-NEO（组件能力不在数据面判定范围，但若改动依赖新 UI 组件需另行核 EUI-NEO-CAPABILITIES.md）；② 首页"昨日对比"判定基于 HomeService 现有公开接口与 schema，未逐行读完 proxy_stats.cpp 内部（只核了其输出为实时 2s 窗速率、非历史序列，home_service.h:62 注释+home_module.cpp:441-442 调用点）；③ "逾期标签点击筛选"的需求意图取决于评审文档原文（弹窗式查看已存在，板内过滤是新增），此处只核实现状。
