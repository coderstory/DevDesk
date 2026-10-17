# 同步产品缺口（用户视角审计定出）

> 来源：handoff §3.C 同步侧 3 条（`/var/folders/qc/yhfbgj4s1cv0j95x0qgfyb000000gn/T/devdesk-handoff-2026-10-16-late.md`）；
> 2026-10-17 逐条静态复核并补证。
> issue tracker 约定见 `docs/agents/issue-tracker.md`，标签词表见 `docs/agents/triage-labels.md`。

**范围**：认证失败的定向引导、同步载荷的知情披露、大内容与分页边界的测试与披露。
**不在范围**：token 存储位置（已由 `44b6d8c` 迁 DB，另见审计留痕）、`409` 冲突弹窗（已有较好基础）。

| # | 票 | 一句话 | 建议优先级 | Status |
|---|----|--------|-----------|--------|
| 01 | [401 无定向引导](issues/01-401-no-targeted-guidance.md) | token 失效走通用「同步失败详情」，无「去更新 Token」动作 | 高 | `needs-triage` |
| 02 | [整库 dump 上云未披露](issues/02-whole-db-dump-undisclosed.md) | 卡片文案说「同步笔记和待办」，实际推的是整库 SQL dump | 中高 | `needs-triage` |
| 03 | [大笔记与分页边界未测未披露](issues/03-large-note-and-pagination-edges-undisclosed.md) | >1MB 笔记与超量分页无测试、无用户告知 | 中 | `needs-triage` |

**建议推进顺序**：01 → 02 → 03。

**承重前置**：`github_sync_state` 必须留在 `core::sql_dump::defaultSkipTables()` 内（token 不上远端）—— 见 `docs/superpowers/specs/2026-10-15-github-sync.md` §Token 存储的承重不变量；02 的披露文案不得与它冲突。
