# DOCS-CONVENTIONS.md — 本项目文档目录地图

> 通用规范（命名 / 内容骨架 / 写作规范含密度原则 / 规范自更新规则）的权威：devflow 插件 `skills/devflow-setup/assets/docs-conventions-template.md`。本文件只记**本项目路径**；路径变更改本表。

| 类型 | 位置 | 生命周期 |
|---|---|---|
| PRD（含功能矩阵） | `docs/superpowers/specs/` | 长期，随版本演进 |
| 技术方案卡 | `docs/superpowers/plans/<slug>-tech.md` | 长期 |
| 测试清单卡 | `docs/superpowers/plans/<slug>-tests.md` | 长期（验收后留作回归参照） |
| 总结会纪要 | `docs/superpowers/plans/<slug>-retro.md` | 长期 |
| 待办与剩余决策 | `TODO.md`、`docs/superpowers/plans/2026-09-22-backlog.md` | 活文档 |
| 调研报告 | `docs/research/YYYY-MM-DD-<slug>.md` | 长期，结论过时须标注 |
| Agent 评测任务集 | `docs/research/agent-eval-tasks.md` | 活文档 |
| 证据物 | `evidence/YYYYMMDD-<slug>/`（git-ignore） | 测试清单卡引用的保留 |
| 能力 / 使用文档 | `docs/EUI-NEO-CAPABILITIES.md`、`USER_GUIDE.md` | 长期 |
| 角色定义 | devflow 插件 `agents/`（方法论 + 默认模型；项目约束由 AGENTS.md 承载） | 插件维护 |
| 经验沉淀 | `.zcode/lessons/<role>.md` | 活文档（防膨胀规则见 AGENTS.md §九） |
| 武器（脚本） | `.zcode/armory/`（门禁放 `gates/` 子目录） | 长期 |
| 小抄 | `.zcode/cheatsheets/` | 活文档，注明生成日期 |
| skill（流程类） | devflow 插件供给（`ask` / `req-analysis` / `tech-solution` / `consolidate` / `devflow-setup`）；自建 skill 才放 `.agents/skills/` | 长期 |
| hooks 配置 | `.zcode/config.json`（项目门禁）；白名单与激活 hook 在插件 | 长期，改动须报备用户 |
| 归档 / 产物（只读） | 原 `analysis/`、`pr-bodies/` 等已于 2026-10-29 废弃归档至工作区外备份（见迁移记录），仓库内不再保留，见 AGENTS.md §五 | 冻结 |
