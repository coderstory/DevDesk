# 智能体产品缺口（用户视角审计定出）

> 来源：handoff §3.C 智能体侧 4 条（`/var/folders/qc/yhfbgj4s1cv0j95x0qgfyb000000gn/T/devdesk-handoff-2026-10-16-late.md`）；
> 2026-10-17 逐条静态复核并补证，其中 04 的原始口径与静态证据冲突，已改为「待复核」。
> issue tracker 约定见 `docs/agents/issue-tracker.md`，标签词表见 `docs/agents/triage-labels.md`。

**范围**：`todo.create` 的能力面、触发器的一次性语义、撞闸/解析失败的可见性、执行记录的呈现口径。
**不在范围**：Scheduler 丢唤醒修复（已并入 dev）、token 存储（另有票）。

| # | 票 | 一句话 | 建议优先级 | Status |
|---|----|--------|-----------|--------|
| 01 | [todo.create 属性面过窄](issues/01-todo-create-attribute-surface.md) | 建任务只能给 title + columnId，其余属性要二跳 | 中 | `needs-triage` |
| 02 | [无一次性任务语义](issues/02-no-one-shot-task-semantics.md) | cron/事件触发恒为循环，没有「跑一次就停」 | 中高 | `needs-triage` |
| 03 | [撞闸与解析失败 UI 静默](issues/03-gate-and-parse-failures-ui-silent.md) | 只落 `LOG_WARN`，用户看不到「这次没跑」 | 中 | `needs-triage` |
| 04 | [执行记录呈现口径待复核](issues/04-run-trace-presentation-needs-review.md) | 原口径「工具级留痕不呈现」与静态证据冲突 | 低（先复核） | `needs-triage` |

**建议推进顺序**：04 先复核（可能推翻）→ 02 → 03 → 01。

**与其它工作流的关系**：本目录 4 条都属「用户旅程可见性/能力面」，由 handoff §3.D 的真机 UI 走查产出 bug 清单后会再收窄一次。
