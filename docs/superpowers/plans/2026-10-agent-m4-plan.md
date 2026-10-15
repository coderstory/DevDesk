# 智能体 M4（事件触发 + 任务级记忆）实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** agent M4 = 决策记录（`docs/superpowers/specs/2026-10-agent-roadmap-decisions.md` §3–§6，R-3/R-4/R-5/R-6/R-7）两项能力：① 纯进程内 EventBus（core 层发布/订阅）+ 首批 3 个动作事件（todo.created / todo.completed / md.saved）驱动 agent 任务自动起 run；② 任务级 context note（agent_tasks 新列、UI 可编辑、注入 system prompt、run 正常结束自动 append、4KB 滚动丢最旧）。R-7 明确不做：持久化事件队列、状态事件、批量聚合、LLM 摘要——实现中任何「顺手加上」都算越界。

**Architecture:** M4 首次为 agent 动 core（EventBus 新建 + schema.h 加列），锚点全部于 2026-10-16 写计划前实测核对；行号漂移时**以符号名重定位**，不改本计划的历史行号注记。

- **EventBus（全仓零命中，需 core 新建）**：`src/core/event_bus.h`（+ 可选 .cpp）——进程内同步或队列式发布/订阅，无持久化、无补发（与 cron「错过的触发不补跑」语义一致，spec §5.1）。发布方（module_todo / module_markdown）与订阅方（module_agent Scheduler）都只 include core 头，模块间零互相依赖（`check-module-isolation.sh` 必须保持绿）。
- **migration 口径（2026-10-16 主会话核实，已按用户口径定案）**：版本化迁移系统已于 d6a0f51（2026-09-30）删除，现为 `core/schema.h` 单份幂等 kSql（schema.h:10）。schema.h:1-4 顶部注释即口径本身：「不写迁移、不兼容旧数据——没有 schema_version 表 / ALTER 序列 / 新库守卫；改 schema 的唯一动作：直接改这份脚本」。M4 加列 = 直接改 schema.h 里 agent_tasks 建表语句，**零迁移零版本号**；开发期旧 DB 删除重建（程序未发布、无真实旧库）。M3 计划备注里的「migration 版本号两套口径冲突（v4/v12）」已随迁移系统删除而失效，不再适用。
- **发布点（裁决：放 module UI 层，service 层不发）**：
  - `TodoService::createTask`（todo_service.cpp:414-479）与 `TodoService::completeTask`（todo_service.cpp:632-649）是 service 写路径；但**工具侧 todo_tools.cpp:208 / :356 也调 svc_.createTask / svc_.completeTask**——若放 service 层，agent 自己经工具建待办也发 todo.created，事件型任务可能自触发循环（agent 建待办 → 发事件 → 冷却窗口一过再起跑 → 再建待办）。裁决：**发布放 module UI 层**——todo_module.cpp:172-174（doAddTask 成功分支）、todo_module.cpp:207（doCompleteTask 成功分支，todo_ui_dialogs.cpp:791-792 的完成路径也经 host 接线落到此处）、markdown_module.cpp:2874-2903 `saveEditor` 成功分支（:2897-2901，lastSavedText_ 更新处；:3546 lastSavedText_ 成员）。权衡：代价是「非 UI 的 service 调用不产事件」，而当前唯一非 UI 调用方恰是 agent 工具路径——正是要排除的自触发源，代价为零；M5 若出现第二个非 UI 事件生产者，再评估挪 service 层 + 事件负载带 source 标记。
- **订阅/触发侧**：`Scheduler`（module_agent/scheduler.h:22）现只有 cron tick（scheduler.cpp:167-206 `tickNow`，:179 `decodeTriggerConfig`，:195-199 lastFiredMinute_ 同分钟防重）+ 常驻队列（scheduler.h:34 `enqueueResident`，:60 residentQueue_）。事件触发 = 新触发类型 `trigger_type="event"`（agent_module.cpp:200 `kTriggerKeys[]` 现 {"manual","schedule","resident"}，加第 4 项）+ `TriggerConfig`（trigger_config.h:12-14）扩展 `events` 数组与可选 `cooldownSec` 字段；编解码唯一真源 = trigger_config.cpp:13 `kCronKey` 旁加 kEventsKey / kCooldownKey。冷却窗口默认 5 分钟（kEventCooldownSec=300），按 task 记上次事件触发时刻（Scheduler 内存 map，mu_ 护，重启清零——与「不补发」一致）；`cooldown_sec` 可选字段按任务覆盖默认值。装配接线：module_agent 在 Scheduler::start 时把 core EventBus 的订阅回调接到 Scheduler 新方法 `onEvent(name)`（module_agent 内部接线，模块间仍零依赖）。
- **context note 读链**：agent_store.cpp:51-53 `kTaskColumns` SELECT 列清单 + agent_store.h:14-20 `AgentTaskRow` 加 contextNote / autoNote 字段；createTask（agent_store.cpp:96-117）/ updateTask（:196-221）同步读写（新参数走尾参默认值，既有调用点零改动）；AgentStore 新方法 `appendContextNote(taskId, text)`——db_.mutex() 锁内读-拼-裁-写单事务，4KB=4096 字节滚动，超限**从头丢最旧**、裁剪落在 UTF-8 字符边界。UI 编辑 = `saveTaskDialog`（agent_module.cpp:588-635）编辑表单加多行 textarea + 「结束后自动记录」开关；`openTaskEditor`（agent_module.cpp:562-585）回填两字段；保存按钮接线 agent_module.cpp:1572。
- **注入点**：`AgentLoop::run` 中 `llm_.send(messages, toolsJson, std::string(), task.modelSlot)`（agent_loop.cpp:202）第三参现为空串 = 不发 system 字段（llm_client.cpp:107-109 `if (!systemPrompt.empty())` 才带）。注入 = 第三参填 context note（非空时 `"任务执行记忆（时间序，供参考）：\n" + task.contextNote`，空串保持现状零变化）——纯注入，不改变消息历史落库。`AgentTask`（agent_loop.h:16-23）/ `taskFromRow`（agent_loop.h:27）加 contextNote / autoNote 字段（agent_module.cpp:463-464 既有调用点零改动）。
- **completed 自动 append**：AgentLoop::run 收尾 `store_.finishRun`（agent_loop.cpp:328-341）成功且 status=="completed" 且 task.autoNote 时，取**内存 messages 里最后一条 assistant** 消息的 text block 拼接（复用 message_blocks.h:31 `decodeMessageBlocks` + :34 `messagePlainText`），截断后 append 进 context note。截断口径：「前 500 字」按 utf8Truncate（message_blocks.h:43）1500 字节执行（500 汉字 ≈ 1500 UTF-8 字节；ASCII 回复实际留得更长，方向安全，零新代码）。append 格式：`"[yyyy-MM-dd HH:mm] " + 截断文本`。append 失败只 LOG_WARN，不改 run 终态。
- **schema 加列**：agent_tasks 建表语句（schema.h:217-229）加两列 `context_note TEXT NOT NULL DEFAULT ''`、`auto_note INTEGER NOT NULL DEFAULT 1`（开关裁决：**新列**而非复用 trigger_config——trigger_config 是触发语义唯一真源，塞行为开关会污染它的编解码契约与既有校验）。schema.h:1-4 顶部注释本就是「直接改脚本」口径，改列无需动注释，但须核对措辞仍成立。
- **测试锚**：test_schema.cpp:346-361 agent_tasks **11 列精确断言**（expectTasks 数组）——加列必红，Task 3 必须同步为 13 列；test_agent_ui.cpp 现有 30 个 REGISTER_TEST（实测 2026-10-16）、test_agent_store.cpp / test_agent_scheduler.cpp / test_agent_loop.cpp / test_agent_cron.cpp 为 agent 数据/调度/循环既有套件；todo 侧 test_todo_service.cpp / test_todo_ui.cpp、markdown 侧既有套件承接发布点用例。

**Tech Stack:** C++20 / CMake / Ninja；自制迷你 doctest（REGISTER_TEST + DOCTEST_CHECK/REQUIRE，fail-fast）；EUI-NEO 表单组件（textarea 多行 / switch 开关 / dropdown）；构建一律 `bash scripts/build-test-run.sh`。

**Spec:** 决策记录 `docs/superpowers/specs/2026-10-agent-roadmap-decisions.md`（§3 EventBus 形态 / §4 首批事件 / §5 冷却窗口 / §6 context note / §2 R-7 不做清单——本计划唯一需求源，不许偏离）；设计 spec `docs/superpowers/specs/2026-09-30-agent-design.md`（§5.1 cron「错过的触发不补跑」语义、§13 事件触发降级说明）。

## Global Constraints

- **R-7 不做清单是硬边界**：持久化事件队列、状态事件（todo.overdue 等）、批量聚合、LLM 摘要——实现任何一处越界都算返工。事件触发遗漏不补偿（进程不在就不发，重启后冷却窗口从零计）。
- **core 边界**：EventBus 只进 core，零依赖 platform / EUI-NEO（core ❌ platform 铁律）；module_todo / module_markdown 只多一行 emit，不感知订阅者；module_agent 只订阅，不 import 兄弟模块头。改完跑 `bash scripts/check-module-isolation.sh` 必须绿。
- **不抛异常**：EventBus 回调在锁外执行且回调内异常不外溢（订阅方 Scheduler 自带 catch 模式，scheduler.cpp:160-164 先例）；错误一律 `core::Result<T>` / 日志，不静默吞。
- **线程契约**：emit 在 UI 线程发生（发布点全在 UI 写路径）；Scheduler 的 onEvent 只做「入队 + 唤醒」轻动作（照 residentQueue_ 模式，mu_ + cv_），事件风暴不阻塞 UI 线程；AgentLoop 单闸（startRunExclusive，agent_store.cpp:176-194）不变——事件触发与手动/cron 触发共用「同 task 不并发」闸。
- **TDD**：每任务先失败测试后实现；测试名/断言按本计划，不为绿灯弱化断言。
- **构建**：一律 `bash scripts/build-test-run.sh`（禁手敲 cmake）；定向 `./build/tests/devdesk_tests <子串>`（如 `agent_scheduler` / `agent_store` / `event_bus`）；涉及可见 UI 的必须有 probe 或等价无头几何断言覆盖；旧开发库换列后**删除重建**（口径即此，无迁移路径）。
- **worktree + 提交纪律**：在 worktree 隔离分支实施（首建直接跑 `scripts/build-test-run.sh` 自动定位 EUI-NEO 与平台参数）；原子提交、显式 pathspec、禁 `git add -A`；完成合 `dev`（串行合并）。
- **锚点漂移**：执行时行号漂移以符号名为准重新定位，不改本文件历史行号注记。

## Review Focus（四类最易咬人的点）

1. **自触发循环**：事件型任务的 run 里若用 todo/markdown 工具产生写动作，绝不能再发事件（发布点在 UI 层已结构性排除，但 Review 要验证工具路径确实无 emit——todo_tools.cpp:208/:356 与 markdown 工具路径逐条核）。另外：**事件型任务起 run 本身、以及 run 结束 append context note 都不是事件**，不参与冷却判定。
2. **冷却窗口语义**：冷却按「任务 × 上次事件触发时刻」计（默认 300s），窗口内事件**丢弃不是延后**（不排队、不攒批——攒批就是 R-7 的批量聚合，越界）；冷却计时从派发成功起算，run 执行时长不占冷却；重启进程冷却清零。注入 NowFn 测假时钟，不睡真时间。
3. **schema 加列的连锁**：test_schema.cpp:346-361 精确列断言必红（11→13 列同步）；kTaskColumns（agent_store.cpp:51-53）与 taskRowFrom（:56-90）漏加一列 = 运行期列缺失 fail；INSERT（:103-105）/ UPDATE（:204-205）列清单必须与 SELECT 一致——四处不同步就是静默丢数据。
4. **append 的字符边界**：4KB 滚动裁剪必须落在 UTF-8 字符边界（从最旧侧丢，中文 3 字节切半 = 崩 UI 渲染）；注入的 system prompt 与落库消息历史严格分离（system 字段不进 agent_messages，不改变既有消息流测试）。

---

### Task 1: core EventBus 落地 + todo/md 发布点

**Files:**
- Create: `src/core/event_bus.h`（+ `src/core/event_bus.cpp`，header-only 可行则不建 cpp）；`src/core/CMakeLists.txt` 注册
- Modify: `src/module_todo/todo_module.cpp`（:172-174 doAddTask / :207 doCompleteTask 成功分支各加一行 emit）+ `src/module_markdown/markdown_module.cpp`（saveEditor 成功分支 :2897-2901 加一行 emit）
- Test: Create `tests/test_core/test_event_bus.cpp`；`tests/test_modules/test_todo_service.cpp` / `test_todo_ui.cpp`、markdown 既有套件各追加「动作 → 事件」用例

**Interfaces:**
- Produces: `core::event::EventBus`——进程内单例（`core::event::bus()` 访问器）。最小面：
  - `emit(const std::string& name)`（R-4 首批 3 事件名常量：`kTodoCreated`="todo.created" / `kTodoCompleted`="todo.completed" / `kMdSaved`="md.saved"，命名建议落 core 头，双方共用一套常量避免字符串漂移）；
  - `subscribe(name, handler)` 返回 RAII 退订 token（析构即退订，防 Scheduler 生命周期后回调悬空）。
  - 契约：同步派发（emit 即调 handler）；无订阅者 emit = no-op 不崩；handler 抛异常不外溢（catch + LOG_ERROR，点名事件名）；**无持久化、无补发**。
- 发布点语义：emit 在**写成功之后**调用（Result fail 分支不发）；md.saved 由 saveEditor 成功分支发（editorText_ == lastSavedText_ 幂等早退路径 :2876-2880 不发——无真实写盘不产事件）。
- Consumes: 无（纯新增）。

**验收:**
- [ ] EventBus 单测：发布→订阅者收到；无订阅者不崩；多订阅者全收到；token 析构后 emit 不回调。
- [ ] todo UI 建待办/完成待办 → bus 收到对应事件；service 层直调（todo_tools.cpp 路径）**不**发事件（负向断言）。
- [ ] md saveEditor 真实写盘 → md.saved；幂等早退不发包。
- [ ] `check-module-isolation.sh` 绿（module_todo/module_markdown 只新增对 core 头的 include）。

- [ ] **Step 1: 写失败测试**（EventBus 四用例 + todo/md 发布正负用例）
- [ ] **Step 2: 红 → 实现 event_bus.h → 绿**
- [ ] **Step 3: 三处发布点接线 → 定向测试绿 + isolation 绿**
- [ ] **Step 4: 原子提交**（显式 pathspec：event_bus 新文件 + 两个 module 的发布点 + 测试）

---

### Task 2: agent 事件触发（trigger_config 扩展 + Scheduler 事件路由 + 冷却窗口）

**Files:**
- Modify: `src/module_agent/trigger_config.h` / `trigger_config.cpp`（TriggerConfig 加 `std::vector<std::string> events` + 可选 `cooldownSec`；kCronKey :13 旁加 kEventsKey / kCooldownKey）
- Modify: `src/module_agent/scheduler.h` / `scheduler.cpp`（新方法 `onEvent(const std::string& name)`；冷却 map；NowFn 复用 :25 既有时钟缝）
- Modify: `src/module_agent/agent_module.cpp`（:200 `kTriggerKeys[]` 加 "event"；:232 `triggerIndexOf`；saveTaskDialog :588-635 的 event 分支校验：events 至少勾选 1 个；装配处把 `core::event::bus()` 订阅接到 scheduler_->onEvent，onStop 退订）
- Test: `tests/test_modules/test_agent_cron.cpp` 或新建 test_agent_trigger_config 用例追加（encode/decode 新字段）；`tests/test_modules/test_agent_scheduler.cpp` 追加（事件路由 + 冷却）

**Interfaces:**
- TriggerConfig 扩展（编解码唯一真源不动摇）：`{"events":["todo.created"],"cooldown_sec":300}`——cron 字段对 event 型**可缺省**；decode 规则：trigger_config 解出的对象必须**至少含非空 cron 或非空 events 数组之一**，两者皆缺 → fail（沿用 :29-50 的中文 fail 文案风格）；`cooldown_sec` 缺省 → 调用方填 300。schedule 型解码行为零变化（既有用例不许红）。
- `Scheduler::onEvent(name)`：锁内扫 store 任务太重——契约定为「轻入队」：先把 (name, nowSec) 记入待处理队列并唤醒既有 cv_，由常驻线程消费时再 `listTasks` 过滤 `trigger_type=="event"` 且 events 含 name 且 enabled 的任务 → 冷却判定（map<taskId, lastFireSec>，now − last < cooldownSec → 丢弃；否则 last=now、dispatchRun）——dispatchRun（scheduler.cpp:208）与 cron/手动路径完全共用，单闸语义不变。
- 常驻队列与事件队列合并或分列由实现定（分列更清晰：eventQueue_ 存 (name, nowSec) 对），但**事件消费不依赖 cron tick 线程的分钟节奏**（事件即时派发，不等下一分钟）。
- Consumes: Task 1 的 `core::event::bus()` + 事件名常量；`store_.listTasks()`；`dispatchRun`；NowFn 时钟缝。

**验收:**
- [ ] event 型任务（events 含 todo.created）在 bus 发 todo.created 后被起 run；manual/schedule/resident 型不受事件影响。
- [ ] 冷却：注入 NowFn，同任务 300s 内第二次事件不派发；窗口过后再派发；`cooldown_sec` 覆盖默认值生效。
- [ ] 窗口内事件**不排队不延后**（第二次事件直接丢弃，队列不留残单）。
- [ ] 坏 trigger_config（events 空数组 / 两者皆缺）→ 点名 task 的 WARN + 跳过，不崩不静默。
- [ ] 既有 schedule 型全部用例零回归（decode 对 cron 路径行为不变）。

- [ ] **Step 1: 写失败测试**（trigger_config 新字段编解码四用例 + 调度路由/冷却/丢弃三组用例，全用注入时钟零等待）
- [ ] **Step 2: 红 → 实现 TriggerConfig 扩展 → 绿**
- [ ] **Step 3: 实现 onEvent + 事件队列消费 + 冷却 → 绿**
- [ ] **Step 4: UI 触发类型下拉加「事件」+ 表单 event 分支校验 + 装配接线（start 订阅 / onStop 退订）→ 定向测试绿**
- [ ] **Step 5: 原子提交**

---

### Task 3: context note 数据层（schema.h 加列 + AgentStore 读写）

**Files:**
- Modify: `src/core/schema.h`（agent_tasks 建表 :217-229 加 `context_note TEXT NOT NULL DEFAULT ''` + `auto_note INTEGER NOT NULL DEFAULT 1`；核对 :1-4 顶部口径注释仍成立）
- Modify: `src/module_agent/agent_store.h`（AgentTaskRow :14-20 加 `std::string contextNote` + `bool autoNote = true`；createTask/updateTask 尾参默认扩展；新方法 `appendContextNote`）
- Modify: `src/module_agent/agent_store.cpp`（kTaskColumns :51-53、taskRowFrom :56-90、INSERT :103-105、UPDATE :204-205 四处同步；appendContextNote 实现）
- Test: `tests/test_core/test_schema.cpp`（:346-361 expectTasks 11→13 列）；`tests/test_modules/test_agent_store.cpp` 追加

**Interfaces:**
- schema：两列一次加齐（DEFAULT 保证既有 INSERT 零改动即兼容新库）；**零迁移零版本号**（schema.h:1-4 口径）；开发期旧 DB 删除重建。
- `AgentStore::appendContextNote(int64_t taskId, const std::string& text)`：db_.mutex() 锁内 SELECT context_note → 拼接 → 若超 4096 字节则**从头丢弃最旧内容至 UTF-8 字符边界**（复用/参照 message_blocks.h:43 utf8Truncate 的边界处理思路，方向相反）→ UPDATE 写回 + updated_at。append 空串 = no-op ok。返回 Result<void>，taskId 不存在 → NotFound。
- createTask/updateTask 新参数走**尾参默认**（createTask 默认空串/true、updateTask 尾参带默认），既有调用点（agent_module.cpp:618/:627、测试）不强制改。
- Consumes: schema.h kSql；sqlite_db.h prepare/bind/step 既有模式。

**验收:**
- [ ] test_schema expectTasks 更新为 13 列且绿；新库 agent_tasks 含两列、默认值正确（context_note=''、auto_note=1）。
- [ ] createTask → getTask 读回 contextNote/autoNote 一致；updateTask 覆写两字段。
- [ ] appendContextNote：多次 append 顺序累积；超 4KB 滚动丢最旧且新内容完整保留；UTF-8 边界不切半（中文混排用例）；不存在 task → NotFound。
- [ ] 既有 store 用例（列清单无关的）零回归。

- [ ] **Step 1: 写失败测试**（schema 13 列断言先红 + store CRUD/append 用例）
- [ ] **Step 2: 红 → 改 schema.h + store 四处同步 → 绿**
- [ ] **Step 3: 实现 appendContextNote 滚动逻辑 → 绿（重点验中文边界）**
- [ ] **Step 4: 删除本机旧开发库重建，冒烟启动不崩**
- [ ] **Step 5: 原子提交**

---

### Task 4: UI（任务表单编辑 context note + 自动记录开关）

**Files:**
- Modify: `src/module_agent/agent_module.h`（编辑态成员 editContextNote_ / editAutoNote_）+ `src/module_agent/agent_module.cpp`（openTaskEditor :562-585 回填；saveTaskDialog :588-635 持久化；表单 compose 加 textarea + switch，参照 :1261-1277 既有 isSchedule 分支的表单扩展先例）
- Test: `tests/test_modules/test_agent_ui.cpp` 追加

**Interfaces:**
- 表单：新建/编辑弹窗加「执行记忆」多行 textarea（预填 contextNote）+「结束后自动记录回复」switch（预填 autoNote）；编辑态经 `setEditCronDraft` 同族的 setEdit* 方法写入（agent_module.cpp:522-529 先例）。
- 保存：saveTaskDialog 把两字段并入 createTask/updateTask 调用（:618/:627）；校验仍前置（goal 空 / cron 非法照旧优先）。
- 弹窗高度契约：textarea 行高与 kDetailDialogH 同族高度常量联动（M3 Task 2 的 :173/883 同步注释规则——新增子项必须同步 childCount/childrenH 计算，不得靠既有 gap 吸收）。
- Consumes: Task 3 的 store 新参数；EUI-NEO textarea / switch 组件（能力清单确认可用）。

**验收:**
- [ ] 新建任务填记忆 + 开关 → 落库读回一致；编辑回填正确。
- [ ] 不填记忆 → 库里空串，后续注入与 append 行为与「无记忆」一致。
- [ ] 弹窗几何断言（无头）：总高不越界、子项计数同步、textarea/switch 存在性。
- [ ] 既有 30 个 test_agent_ui 用例零回归。

- [ ] **Step 1: 写失败测试**（表单组合：回填/保存/开关翻转 + 几何断言）
- [ ] **Step 2: 红 → 实现表单 + 编辑态 → 绿**
- [ ] **Step 3: 原子提交**

---

### Task 5: 注入 + completed 自动 append（AgentLoop）

**Files:**
- Modify: `src/module_agent/agent_loop.h`（AgentTask :16-23 加 `std::string contextNote` + `bool autoNote = false`）+ `agent_loop.cpp`（taskFromRow 映射；:202 send 第三参注入；:328-341 finishRun 成功后 append）
- Test: `tests/test_modules/test_agent_loop.cpp` 追加

**Interfaces:**
- 注入：`llm_.send(messages, toolsJson, systemPrompt, task.modelSlot)`——systemPrompt = contextNote 非空时 `"任务执行记忆（时间序，供参考）：\n" + task.contextNote`，空串 = 现状零变化（llm_client.cpp:107-109 非空才带 system 键）。**不落库**：system 字段只进请求体，agent_messages 消息流一字不动。
- 自动 append：finishRun 成功且 status=="completed" 且 task.autoNote 时——取内存 messages 最后一条 role=="assistant" 的 contentJson → `decodeMessageBlocks` + `messagePlainText`（message_blocks.h:31/:34）拼接 text → `utf8Truncate(text, 1500)`（500 字口径，见 Architecture）→ 格式 `"[yyyy-MM-dd HH:mm] " + 截断文本`（本地时间，core::time::localTime）→ `store_.appendContextNote(task.id, …)`。append 失败 → LOG_WARN，不改 run 终态不影响 run 返回值。stop_reason 非 end_turn（failed/limited/cancelled）不 append。
- 消息流测试里 mock LLM 的既有用例照旧：system 参数变化只影响「注入用例」断言（mock send 可捕获第三参）。
- Consumes: Task 3 appendContextNote；message_blocks 既有纯函数；AgentLoop 内存 messages。

**验收:**
- [ ] contextNote 非空 → send 第三参含记忆头 + 原文；空 → 第三参为空串（既有用例不变即证）。
- [ ] completed + autoNote → note 追加一条带时间戳条目；failed/limited/cancelled → 不追加。
- [ ] autoNote=false → completed 也不追加。
- [ ] agent_messages 落库内容与注入前完全一致（system 不进消息流，逐字节断言）。
- [ ] 回复超 500 字截断、不足全留；append 写库失败仅 WARN。

- [ ] **Step 1: 写失败测试**（注入两用例 + append 三态用例 + 消息流不变断言）
- [ ] **Step 2: 红 → 实现注入 → 绿**
- [ ] **Step 3: 实现 completed append → 绿**
- [ ] **Step 4: 原子提交**

---

### Task 6: 收尾（全量回归 + 交付）

- [ ] `bash scripts/build-test-run.sh` 全量（零回归，Total/Passed/Failed 与开工基线对比；用例总数以运行输出 `Total:` 为准）
- [ ] `bash scripts/check-module-isolation.sh` 绿
- [ ] UI probe（`ctest -L probe`）绿或按三态门禁判定；无头几何断言全绿
- [ ] 截屏留痕（`--screenshot`）：任务弹窗含记忆 textarea + 开关 + 事件触发类型；事件端到端冒烟（UI 建待办 → 事件型任务起 run）
- [ ] 原子提交（显式 pathspec）→ 审计（六维度：正确性/回归面/平台对称性/静默失败面/测试有效性/文档口径）→ 串行合 `dev`

---

## 备注：裁决登记（本计划拍板、决策记录未细化的点）

- **发布层**：module UI 层发布，service 层不发（自触发循环结构性排除，分析见 Architecture；M5 出现第二个非 UI 生产者再评估挪层 + source 标记）。
- **trigger 形态**：新 trigger_type "event" + trigger_config 扩展 events 数组（而非只扩 JSON 不加类型）——「事件型任务」在任务列表/编辑弹窗是一等公民，纯 JSON 扩展会把触发类型与载荷拧成两套口径。
- **冷却可配**：R-5「任务×事件组合可配」本期落为**任务级** `cooldown_sec`（默认 300）；按事件粒度细分配置无真实需求，不建。窗口内事件丢弃不排队（排队=变相批量聚合，R-7 越界）。
- **auto_note 开关**：独立新列（不复用 trigger_config——触发真源不塞行为开关）；默认 1（R-6「任务级可关」= 可关，默认开）。
- **500 字口径**：utf8Truncate 1500 字节（500 汉字），零新代码；ASCII 回复多留，方向安全。
- **M3 计划备注失效项**：其「备注：口径外事项」里的 migration 版本号冲突核实项已随迁移系统删除（d6a0f51）而关闭，M4 按本计划 schema 口径执行。
