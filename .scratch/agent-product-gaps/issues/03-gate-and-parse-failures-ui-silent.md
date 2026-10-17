# 03 · 撞闸与解析失败 UI 静默（只落 LOG_WARN）

Status: `needs-triage`
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
