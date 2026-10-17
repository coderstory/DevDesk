# 02 · 无一次性任务语义（cron/事件触发恒为循环）

Status: `needs-triage`
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
