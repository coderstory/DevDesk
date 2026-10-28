# RESEARCH-INDEX 调研与分析成果索引

> **单一登记源**：researcher / investigator / auditor 落盘的每份文档必须在此登记一行——不登记 = 未归档。
> **查重先行**：一切调研 / 分析类派单前先查本册，已覆盖的课题不重复调研、不重复联网。
> **分工**：战略基线级调研报告（`docs/strategy/` 01–13）由 [`00-总览索引.md`](strategy/00-总览索引.md) 管，本册不重复登记；本册管任务级成果。
> **时效**：条目对应文档超 30 天未核对 → 文档移入 `docs/archive/` 并从本册摘除（沿 CLAUDE.md「状态文档时效约定」）。

**登记格式**（一行一条，最新在上）：

```
- YYYY-MM-DD | 类型(调研/调查/审计) | 主题 — 一句话结论 | 路径
```

---

## 登记簿

- 2026-10-28 | 调查 | memcpp S2 记忆模块 live 层验证 — venv/引擎对账、llama 联动、三视图截图、key 红线；4 处修复 1331450c/90cf043f/473204ff/00ac9931 | docs/superpowers/plans/2026-10-28-memcpp-s2-live-verify.md
- 2026-10-28 | 调查 | memcpp 端到端联调验收 — S3+S4 遗留 live 对账，5 处缺陷修复（MCP 会话缝/update 形参/delete 遗忘映射/UUID id 回填/融合段解析），Recall@5=100%，hook 真跑+ledger 闭环+fail-open | docs/superpowers/plans/2026-10-28-memcpp-e2e-verify.md

## 存量补登欠账

- `docs/superpowers/plans/` 存量约 157 份文档（audit / research 混杂）先于本册存在、未经索引，补登为待派任务。**补登完成前，查重除本册外必须辅以 `grep` / 目录扫描 `docs/superpowers/plans/`**，不得只查本册就判定"未调研过"。
