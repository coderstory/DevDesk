# 03 · 撞闸与解析失败 UI 静默（只落 LOG_WARN）

Status: `needs-triage`
实现: 部分完成 bb92b7d（2026-10-17）
建议优先级: 中
来源: handoff §3.C「A6」+ 2026-10-17 静态复核（证据补全）

## 现象

几种「这一跑没发生」的情形，**只写日志、UI 完全静默**——用户看到的是「任务配好了，什么都没发生」：

1. 同一任务上一跑未结束 → 新触发被**单闸**拒绝（`AlreadyExists`）
2. `trigger_config` 解析失败 → 该事件/该分钟跳过派发
3. cron 表达式非法 → 该分钟跳过派发

## 证据

- `src/module_agent/scheduler.cpp:325-329` — 撞闸或起跑失败：
  `LOG_WARN("Scheduler: run 未起 task=…: " + r.errorMessage())`，**无 UI 通道**
- `src/module_agent/scheduler.cpp:240` — 事件型 `trigger_config` 无法解析：「本事件跳过不派发」→ 仅日志
- `src/module_agent/scheduler.cpp:285` — cron 型 `trigger_config` 无法解析：「本分钟跳过不派发」→ 仅日志
- `src/module_agent/scheduler.cpp:293` — cron 表达式非法：「本分钟跳过不派发」→ 仅日志
- 对照：`src/module_agent/agent_loop.cpp:140` 注释表明单闸（`startRunExclusive`）是「全仓唯一同 task 不并发闸点」，手动与 Scheduler/cron 都经此 —— 即撞闸是**设计内**的常规路径，不是异常

## 影响（用户视角）

- **静默失败是最坏的失败模式**：用户以为任务跑了（或以为它没到点），实际被闸挡住/配置根本没解出来
- `trigger_config` 解析失败意味着该任务**永远不会触发**，而 UI 上它看起来完全正常
- 与 token 票重装叠加后风险上升：Scheduler `notify_all` 修复会让事件触发型任务真正开始触发，撞闸面随之变大

## 修复方向（待裁）

1. **任务级状态位**：解析失败 / 连续撞闸时给任务打「异常」徽章，任务列表可见（**最小可见面**）
2. **执行记录里落一行「未起跑 + 原因」**：并入运行历史，用户点进去能看到「被闸拒绝 / 配置非法」
3. **通知通道**：撞闸是否值得冒泡到既有 run-end 通知通道（`agent_module` 的 app 级通知）—— 需评估噪音（撞闸是常规路径，可能很频繁）

> 与 04 的关系：04 复核「执行记录呈现」，03 的选项 2 依赖 04 的结论。

## 验收判据（待定稿）

构造「非法 cron 的任务」与「连续两次触发同一任务」，两种情况都能在 UI 上被用户看到（而非只在日志里）；正常路径不产生噪音。

## Comments

### 实现（bb92b7d，2026-10-17）：只完成修复方向 1 的静态半边，撞闸半边未做（理由见下）

- 判据收成一个纯函数 `module::agent::triggerConfigProblem`（声明 `src/module_agent/trigger_config.h:39`、实现 `src/module_agent/trigger_config.cpp:41`），与 Scheduler 消费侧**同源**：event 型走 `decodeTriggerConfig`（= `scheduler.cpp:237` 那段校验），schedule 型走 `decodeTriggerConfig + parseCron`（= `scheduler.cpp:282/289` 两步），manual / resident 恒空（`scheduler.cpp:235/280` 的 `enabled && triggerType == …` 过滤都不含它们）。全仓只有这一份「配置有没有问题」的规则，不存在第二套。
- 比票面多覆盖一类（有意）：event 型配置能解出来、但 `events` 为空（如 `{"cron":"…"}` 落成 event 任务）——`processEvent` 的逐项比对恒不命中、同样永不触发，而 decode 会放行这种配置，所以补成第 4 条判据（`trigger_config.cpp:55-59`）。
- 呈现：任务行标题后 56px 错误色「配置异常」标记（`agent_module.cpp:1477-1487`），标记不消费点击（点行本体开详情）；原因全文（含 decode / parseCron 原始 errorMessage）落详情弹窗配置摘要（`agent_module.cpp:1873-1877`）。判定随 `refreshData` 算一次入缓存（成员 `agent_module.h:218`、填充 `agent_module.cpp:930-935`），不在每帧渲染里反复解析 JSON。
- 正常路径零噪音由两条对照钉住：纯函数对照组（`test_agent_trigger_config.cpp:133`，合法 event / 合法 cron / manual·resident / 事件型带非法 cron 全部空）；UI 对照组（`test_agent_ui.cpp:2748-2753`，合法 cron / 合法事件 / manual 三个任务**一个** `agent.task.<id>.warn` 节点都没有，而同用例前段的坏配置三条必须都有、文案「配置异常」、落在行内且不与标题/状态列重叠——宽度预算算错即红）。
- 先红后绿的现场证据：桩函数返回空串时 `--filter 配置异常` = `Total: 2 Passed: 0 Failed: 2`（`test_agent_trigger_config.cpp:112` 断言红 + `test_agent_ui.cpp:2727` 徽章节点缺失红）；实现后同 filter = `Total: 2 Passed: 2 Failed: 0`。
- 全量门禁：`bash scripts/build-test-run.sh --no-run` → `Total: 1447 Passed: 1441 Failed: 0`（测试全绿）+ UI probe `100% tests passed out of 7` + 模块隔离守卫通过。差 6 条是用例内 skip（live / 环境类），本单新增 3 条全部计入 Passed。

### 未做：撞闸动态半边（票面修复方向 1 的「连续撞闸」+ 验收判据第 2 条）

结论：**没有低成本通道，未硬做**。核实过的事实链：

- 撞闸的落点只有两处，都只落日志：Scheduler 路 `scheduler.cpp:327-329`（`loop_.run` 返回 `AlreadyExists`），手动路 `agent_module.cpp:524-527`（「已在运行，忽略本次启动」）。
- **「这个任务此刻在跑」本来就在 UI 上可见**：撞闸的前提是上一跑仍在跑，而列表状态列此时显示「运行中」（`agent_module.cpp:1442` → `runStatusText`，running→运行中在 `agent_module.cpp:120`）。撞闸真正吞掉的信息只有一条：「本次触发被丢弃」。
- 要补这一条，**必须给 Scheduler 加新状态面**：`runs_` 对外只有聚合 `activeRunCount()`（`scheduler.cpp:96`），没有逐任务可见面；`dispatchRun` 的闸拒绝结果只活在派发线程栈上（`ActiveRun::runId==0` 是唯一残迹，`scheduler.h:57`），DB 里也没有「被拒」行（闸在落行之前，`agent_store.cpp:187-205`）。也就是说只能加「taskId → 时间戳 / 连续计数」这类新成员 + 访问器，且必须同时定一个阈值（多久算最近 / 连续几次算异常）——这正是票面留给裁量的噪音问题（票面修复方向 3、以及「撞闸是设计内常规路径」）。按本单范围（没有低成本通道就不硬做），**未实现**；建议并入 04 或单独开一张带噪音评估的票。

### 未覆盖的路径（如实）

- **撞闸动态路径零覆盖**：无 Scheduler 级用例、无 UI 级用例（功能未做，见上）。
- event 型「events 为空」这条新判据只在纯函数层测（`test_agent_trigger_config.cpp:115`）；UI 侧只覆盖了 `{"events":[]}` 这种 decode 直接失败的形态（`test_agent_ui.cpp:2700`），没为「cron-only 配置的 event 任务」再点一条 UI 用例。
- 徽章随 `refreshData`（`tasksDirty_` / `uiPolling_`）更新：外部直写库改坏配置且不置脏时，徽章与任务列表本身一起滞后（不是本改动引入的滞后）。
- 详情弹窗的原因取自同一次刷新的缓存快照，不是打开弹窗时重算（有意：避免每帧解析；好处是列表与详情永远一致）。

### 与票面不符的事实

1. 票面「撞闸 → UI 完全静默」只对「本次触发被丢弃」成立：任务上一跑仍在跑这件事列表一直是可见的（「运行中」）。票面把两条信息混作一处了。
2. 票面「`trigger_config` 解析失败意味该任务永远不会触发」成立，但漏了 event 型的另一种「永不触发」（配置解得出、只是没配任何事件）。
3. 票面验收判据第 2 条「连续两次触发同一任务，UI 可见」**未达成**（撞闸未做）；第 1 条（非法 cron 的任务 UI 可见）已达成并有用例。
