# 02 · 无一次性任务语义（cron/事件触发恒为循环）

Status: `needs-triage`
实现: 已完成（2026-10-17）
建议优先级: 中高
来源: handoff §3.C「A3」+ 2026-10-17 静态复核

## 现象

智能体任务只能配「cron 周期」或「事件触发」，**没有「跑一次就停」的语义**。用户想让某任务跑一次（例如「今天 18:00 提醒我一次」），只能事后手动停用/删除，否则它会一直按周期重复。

## 证据

- `src/module_agent/agent_loop.h:16` `struct AgentTask` 的全部字段：
  `id / name / goal / modelSlot / maxIterations / allowDestructive / contextNote / autoNote`
  —— **无** `once` / `maxRuns` / `enabled` / 一次性标记
- `src/module_agent/trigger_config.h:21` `struct TriggerConfig` 只有 `cron` / `events` / `cooldownSec`
  —— 语义即「匹配即触发」，无次数上限；`cooldownSec` 只做节流
- `src/module_agent/scheduler.cpp:325` 每次触发都 `loop_.run(task, ...)`，任务本身不带「本次之后失效」的状态
- 调度侧无「触发后自动停用任务」的路径（`grep once|maxRuns|enabled` 在 `agent_*` 无命中）

## 影响（用户视角）

- 想「只跑一次」的用户必须记得回来手动停 —— 忘了就反复跑、反复消耗 LLM 配额
- 首次引导里若暗示「设个时间让它跑」而不说「它会一直跑」，属于预期落差

## 修复方向（待裁）

1. **给 `AgentTask` 加一次性字段**（如 `runOnce` 或 `maxRuns`），Scheduler 触发计数达上限后自动置停用 —— 语义最干净，需动表结构与 scheduler。
2. **只加 UI 层「跑一次」按钮**（立即触发一次，不建重复任务）—— 覆盖「现在跑一次」的场景，但覆盖不了「未来某时跑一次」。
3. **只做披露**：UI 明说「周期任务会持续重复，跑完请手动停用」—— 零代码，但没解决语义缺失。

> 与 01/03 的关系：02 决定「任务何时该停」，03 决定「该跑没跑时用户知不知道」。

## 验收判据（待定稿）

可配置一个「指定时间跑一次即自动停用」的任务；重复任务行为不回归；停用后不再占 run 队列。

## Comments

### 实现（2026-10-17，本提交）：trigger_config 可选 `max_runs` 键 + Scheduler 额度闸 + 「只跑一次」开关

选**修复方向 1 的零迁移变体**（未选方向 2「UI 跑一次按钮」：覆盖不了「未来某时跑一次」；未选方向 3 只披露：不解决语义）。字段选 `max_runs: int`（正整数 = 上限；缺省 = 不限次数）而不是 `once: bool`：「只跑一次」= 上限 1，将来放开「最多 N 次」不加键、不再迁移。

**零迁移路线（本票硬约束：不写 migration / 不改 schema / 不写回填与兼容分支）**：

- 载体 = 既有 `agent_tasks.trigger_config` 列的 JSON 文本：`encode/decodeTriggerConfig` 加可选键 `max_runs`（`src/module_agent/trigger_config.cpp:17` 键名、`:33` 编码、`:119-130` 解码；契约注释 `trigger_config.h:22-30/58-62`）。
- 编码**只在 >0 时写键**（`trigger_config.cpp:33`）→ 不限次数的产物与加键前逐字节相同；既有钉原文用例（`tests/test_modules/test_agent_ui.cpp:349` 的 `{"cron":"0 9 * * 1-5"}`）原样绿。
- 旧行没有该键 → decode 出 `nullopt` → 行为与今天完全相同 → 不需要回填、版本门、兼容分支。
- 「自动停用」复用既有 `AgentStore::setTaskEnabled`（不直写 SQL）；「计数」复用既有 `store_.listRuns(taskId, LIMIT)`（`scheduler.cpp:229`，LIMIT=maxRuns，返回行数 == maxRuns 即跑满）——**无内存计数器**，重启额度不复位，也没有新状态面。上限钳在 decode（`kMaxRunsUpperBound = 1000`）防手改库塞超大值把 `listRuns` 拉爆。

**失败语义（明确口径）**：记账单位 = **「起过跑」而不是「跑成功」**。派发成功、库里落了 `agent_runs` 行（running / failed / cancelled / limited 都算）即消耗一次额度；被单闸（同任务并发）拒绝、没落行则**不消耗**，额度留给下一次触发。理由（用户口径）：要的是「别再反复跑」——一次失败的 run 也是「跑过」，重试到成功会让「只跑一次」变成无限重试。用例：`tests/test_modules/test_agent_scheduler.cpp:882`（首跑 failed 仍吃额度，不重试到成功）。
另两条失败路径：计数读库失败 → 点名 WARN + 本次不派发（宁可不跑也不超跑，`scheduler.cpp:230-235`）；置停用写库失败 → 照样不派发（额度判定已成立），下一分钟 / 下一个事件重试写库（`scheduler.cpp:237-243`）。

**重复任务不回归的结构性保证**：

- 未配 `max_runs` → `runBudgetExhausted` 首行 `return false`（`scheduler.cpp:224`）：零查询、零行为差异，老配置走的就是老代码路径。
- 额度闸只加在 schedule（`tickNow`，`scheduler.cpp:286` 调用点）与 event（`processEvent`，`scheduler.cpp:337` 调用点）两条派发路径的**派发前**；manual / resident 根本不经过它。
- 放置位置：schedule 路在 `cronMatches` **之后**（坏 cron 的点名 WARN 不许被提前 `continue` 掉）、同分钟防重之前；event 路在 events 比对**之前**（事件型无周期扫描，下一次事件到达是唯一扫描机会，不匹配的事件也会把跑满的任务置停用——只改启停列，绝不因此派发 run）。
- 对照组：旧配置串（无该键）跨三分钟照旧反复派发且不被停用（`test_agent_scheduler.cpp:822`）。

**覆盖面（新增 10 条，先红后绿；定点 `--filter max_runs` = Total 10 / Passed 10 / Failed 0）**：

- 配置层 2 条：往返 + 旧串字节不变（`test_agent_trigger_config.cpp:97`）；坏值一律 fail（0 / -1 / "1" / true / null / 1001），边界 1 与 1000 绿（`:137`）。
- 调度层 5 条（假时钟 + `tickNow` / `drainEventQueue` 直调，零墙钟零网络）：上限 1 首跑后置停用且后续各分钟不再派发（`:770`）；旧配置对照组（`:822`）；上限 2 第 2 跑照常、第 3 次扫描停用（`:849`）；failed 也吃额度（`:882`）；事件型首跑后停用 + 不匹配事件也触发停用（`:912`）。
- UI 层 3 条：开关落库 `max_runs=1` + 编辑回填勾选 + 原样回写（`test_agent_ui.cpp:2778`）；列表徽章「一次性」/「限N次」+ 详情摘要「次数上限：只跑一次（跑完自动停用）」+ 对照组零噪音（`:2853`）；开关标签盒容得下文案（`:2925`，防静默裁剪）。
- 非空性自检：闸体强制 `return false` 时 4 条调度/事件用例转红（对照组仍绿）；标签盒改成 90px 时容纳用例转红（24 < 48）。

**UI 位置与几何预算**：开关落在**行为开关行**（「允许破坏性操作」右侧，右缘与 goal 输入对齐）而不是触发行——触发行宽度预算已被 cron 输入 + 模板下拉占满，塞进去只能把 cron 提示词挤成截断。为腾位把触发 / 槽位下拉 140→116（两下拉最长标签 4 字 = 48px，字段标签盒 `ddW − 48 = 68` 仍富余；`agent_module.cpp:1668`）。开关只对 schedule / event 渲染（`agent_module.cpp:1814`）——manual / resident 的 trigger_config 不被 Scheduler 消费，给它们显示开关等于假承诺。

**真窗口探针**：`tests/probes/agent_dialog_probe.cpp` 新增 `--shot-task-plain`（无弹层审阅帧——行为开关行不被模板弹层盖住的构图）+ 一个 `max_runs=1` 的种子任务（列表「一次性」徽章的审阅帧来源）。默认调用不变，`ctest -L probe` 7/7 通过。

**门禁数字**：全量 `bash scripts/build-test-run.sh --no-run` → `Total: 1458 Passed: 1451 Failed: 1`（+ Skipped 6，live/环境类）。唯一红 = `settings: 笔记/待办各写路径事件 → GitHub Sync 置 dirty`，**不是本单**：是并行会话在飞的 `md.tags_changed` 订阅半边（`src/core/event_bus.h` 新加常量在读工作区里、`src/` 全仓零订阅者）——把本单 4 个源文件 + 3 个测试文件 stash 掉重跑，同一条照样红。守卫全绿：模块隔离 / std include（417 文件）/ Windows 交叉编译（226 源文件集合）/ 探针跳过登记。

### 未做 / 未覆盖（如实）

- UI 只提供「只跑一次」（= 1）。`N > 1` 只能手改库：草稿是整数、编辑态原样回填（`agent_module.h:246-249`）、文案随 N 走（「限N次」/「最多 N 次」），但**没有**「跑 N 次」的输入控件（票面只要求一次性）。
- 手动「启动」不受 `max_runs` 限制（显式用户动作，不是触发器反复跑）；重新启用一个额度已耗尽的任务，会在下一次扫描（≤1 分钟 / 下一个事件）再次被停用——额度按历史计数、不因启停复位。两条都**未写用例**（有意：前者是既有语义、后者是可接受边界）。
- 「跑满即停用」本身不发通知 / toast（与 03 的撞闸同类：现有 run-end 通知只覆盖 run 终态）。未做。
- 真窗口侧只加了审阅帧与种子任务，**未**为列表徽章 / 详情行加 probe 断言（无头用例已覆盖，真窗口不重复）。
- 未覆盖：`kMaxRunsUpperBound` 之上的手改库值与 `listRuns` 行数截断的组合（只在 decode 边界测了 1001 fail）；`max_runs` 配在 `resident` 型上（不消费、无 UI 入口，只会被详情摘要的「配置异常」逻辑间接覆盖）。
- 双平台：Windows 侧仅过交叉编译守卫（226 源文件集合），未在 Windows 真机跑用例（同仓常规）。

### 与票面不符 / 补充

1. 票面修复方向 1 写「需动表结构与 scheduler」——scheduler 半边对；**表结构半边不必要**：键落在既有 `trigger_config` JSON 文本里，零 schema 变更（这正是本单选的路线，见上）。
2. 票面验收判据三条全达成：①「指定时间跑一次即自动停用」= 开关 + `max_runs=1`（定点用例 + 探针审阅帧）；②「重复任务行为不回归」= 未配键零查询零行为差异 + 三分钟对照组；③「停用后不再占 run 队列」= 两条派发路径在派发前就被过滤/置停用，停用不影响已起 run 的正常收尾。
3. 票面证据说 `AgentTask` 结构无 `enabled` / 一次性标记——准确（`agent_loop.h` 的 `AgentTask` 确实没有）；但**库表侧 `agent_tasks.enabled` 列存在且 Scheduler 已按它过滤**（`scheduler.cpp:268/331`），本单正是复用它做「自动停用」的那一半，没有新增启停面。
