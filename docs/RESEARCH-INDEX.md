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

- 2026-11-10 | 审计 | 只读 MD 查看器 QA 收口 @ wt/mdv-int 6c30ea90 — 半成品：全量回归 1523 单测 Failed 0 + 探针 17/18 绿（agent_e2e 环境前置）；真机主路径可用但新缺陷 BUG-1（P1：中文路径 .md 打开失败，ASCII 对照正常）；假绿抽查 3 条变异全红过；mac 全域+真双击+真注册表链未验 | docs/research/2026-11-10-md-viewer-qa-report.md
- 2026-11-10 | 调查 | F-07 前置验证 POC：Win10 静默写 .md 关联生效性 — 部分生效：HKCU 注册表写入可持久化/回读/幂等，但 ShellExecute 双击实测两次均弹 OpenWith 选择器、notepad 零次拉起，生效默认未变；接管需用户显式选择 | docs/research/2026-11-10-f07-win-userchoice-poc.md
- 2026-11-10 | 调研 | 只读 MD 查看器·平台文件关联 + EUI-NEO 多窗口承载 — Win 单实例已有但第二实例路径丢失是最大缺口；macOS AppleEvent 链路已全通；EUI-NEO openWindow/scrollview 能力确认可用零上游缺口；Info.plist 缺有效 UTI 声明需补 UTImportedTypeDeclarations | docs/research/2026-11-10-md-viewer-file-assoc-multiwindow.md
- 2026-10-27 | 审计 | 壁纸 P3a @ eec88dbb | PASS：2M7L2I（最重=死锁修复无自动回归网）；限定复审计 @ ef5bdf79+dce4a25a 通过（11 条全处置，新增仅 3 Info）| docs/superpowers/plans/2026-10-27-wallpaper-p3a-audit.md
- 2026-10-27 | 审计 | 壁纸 P5 UI @ eec88dbb | 有条件通过：3M3N（过滤宣称/遮罩z序/假绿）；限定复审计 @ ef5bdf79+dce4a25a 通过（6 条全修，残留 R-1 文案归源漂移 Low）| docs/superpowers/plans/2026-10-27-wallpaper-p5-ui-audit.md
- 2026-10-27 | 审计 | MCP 服务骨架 B1+B2+B3 提交段（4bccde3c..a028fb1e） — 有条件通过：2 major（SSRF 字面校验可被 userinfo/短拼 IPv4/v6 zone-id 绕过；台账 §3/§4 未回填且 B2 提交点门禁实红）+ 3 minor | docs/superpowers/plans/2026-10-27-mcp-b1b2b3-audit.md（feat/mcp-server-skeleton worktree）

## 存量补登欠账

- `docs/superpowers/plans/` 存量约 157 份文档（audit / research 混杂）先于本册存在、未经索引，补登为待派任务。**补登完成前，查重除本册外必须辅以 `grep` / 目录扫描 `docs/superpowers/plans/`**，不得只查本册就判定"未调研过"。
