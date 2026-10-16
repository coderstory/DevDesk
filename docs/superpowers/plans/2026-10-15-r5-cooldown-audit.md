# R-5 冷却键改任务×事件组合 — 只读审计（aa82edb）

- 审计对象：`aa82edb`（基于 dev `fc7a248`，46+/5−，scheduler.h / scheduler.cpp / test_agent_scheduler.cpp）
- 审计日期：2026-10-16（agent 只读审计；唯一写动作 = 本文件）
- 裁决：**放行**（1 low 文档措辞 nit，可选跟进）

## 核查点结论（六维）

1. **正确性 — 通过**。键改 `std::pair<int64_t,std::string>`（scheduler.cpp:253-258）后：
   - 默认 300s（`kDefaultEventCooldownSec=300`，trigger_config.h:15）不变；
   - `cooldownSec > 0` 可配覆盖不变；
   - 窗口内 `continue` 丢弃（不排队不延后）不变；冷却起点 = 判定通过之时、dispatch 前落账，位置不变；
   - 边界语义 `nowSec - stamp < cooldown`（+300s 整点放行）不变；
   - 旧单键行为「同任务同事件窗口内二次触发被压制」保留：同 (task,event) 组合仍走同一键压制（既有用例 `event_trigger_cooldown_default_300s_drops_within_window` 修前修后均绿，为对照）。
2. **回归面 — 通过，1 info**。grep 证实 `lastEventFireSec_` 全部使用点仅 scheduler.cpp:255-258（+头文件声明/注释），无漏改。map 无限增长：AgentStore 根本无 deleteTask API（createTask/updateTask 唯一生命周期），旧单键同样从不清理；新粒度上限 = 任务数 × 可达事件名数（onEvent 只接 kEventNames 三事件：todo.created / todo.completed / md.saved，agent_module.cpp:431-440），有界、量级可忽略，与 `lastFiredMinute_` 既有先例一致 — **info，无需动作**。
3. **测试有效性 — 通过**。修前红声明可信：本仓库 doctest.h 为极简自制框架，`DOCTEST_CHECK_EQ` 失败即 `return false`（tests/doctest.h:28-58，REQUIRE 仅是 CHECK 别名），test_main.cpp:185 `ok = t.second()` 以返回值定 PASS/FAIL — 旧单键下 ev.b 在 +100s 被压制 → runs 停 1 → 末尾 `CHECK_EQ(..., 2u)` 失败 → 用例红。断言为内容级（store 里 run 行数），非恒真。「解压」与「保留」两侧都有用例：新用例测不同事件不互压，既有 300s/override 两用例测同事件仍压制 + 可配窗口。
4. **平台对称 / 静默失败 — 通过**。diff 无平台分支、无新错误路径；dispatch 失败 LOG_WARN 原样。
5. **文档口径 — 1 low**。scheduler.h:64（processEvent 注释）、scheduler.h:77（成员注释）、scheduler.cpp:252 均已同步改「任务×事件级」。两处遗留旧措辞：trigger_config.h:13「任务级冷却默认值」、test_agent_scheduler.cpp:539「任务级冷却默认 300s」— 用例逻辑本身仍有效，仅注释措辞过期。**建议下次顺手改，不阻塞**。

## 复跑记录（worktree，实测）

- `DEVDESK_TEST_TMPDIR=/tmp/r5audit-tmp ./build/tests/devdesk_tests event_trigger` → Total 5 / Passed 5 / Failed 0
- 同二进制 `scheduler` 过滤 → Total 10 / Passed 10 / Failed 0
- 与 commit message 声明（修后绿 5/5/0 + 10/10/0）一致；修前红未复跑（需改 worktree 代码，超只读边界），以静态机制推证（见第 3 点）。

## Findings 汇总

| 位置 | 级别 | 问题 | 建议 |
|---|---|---|---|
| src/module_agent/trigger_config.h:13 | low | 注释仍写「任务级冷却默认值」，R-5 后粒度为任务×事件 | 下次提交顺手改 |
| tests/test_modules/test_agent_scheduler.cpp:539 | info | 同上措辞过期（用例本身仍有效） | 可选 |
| scheduler.cpp:255-258 | info | map 无清理（任务删除后键残留） | 无需动作：无 deleteTask API，旧键同样不清理；上限 = 任务数×3 事件名，有界 |

**裁决：放行。**
