# 需求↔BUG 关联与状态联动最佳实践调研（2026-10-30）

执行：老学究 ｜ 用途：池子系统方案定稿参考 ｜ 方法：通用搜索无额度，改取三家官方一手文档（GitHub Docs / Linear Docs / Atlassian Support）。

## 三家怎么做（一手来源）

| 工具 | 关联机制 | 状态联动 | 聚合查询 | 来源 |
|---|---|---|---|---|
| **GitHub Issues** | ① sub-issues：父子层级（≤8 层嵌套、每父有上限），`gh issue create --parent`；② issue dependencies：`blocked by` / `blocking` 双向依赖；③ `Closes/Fixes/Resolves #N` 关键字把 PR 合并与 issue 关闭绑定 | ① 关键字自动关（PR merge→issue close）；② 父 issue 有子进度 rollup（Projects 里可按父分组视图）；**子全关不会自动关父** | Projects 按父 issue 分组/过滤/建视图 | docs.github.com「Using keywords」「Adding sub-issues」「Creating issue dependencies」 |
| **Linear** | parent/sub-issue（父子为主语义），另有 blocked/blocked-by relation | 父子进度 rollup；**状态默认不自动联动**，靠 automation 规则 | 视图按 parent 聚合 | linear.app/docs「Parent and sub-issues」 |
| **Jira** | link 类型矩阵（blocks/blocked by、causes、relates to、duplicates、implements…17 种，admin 可配）+ epic/subtask 层级 | **默认零自动联动**——全靠 automation 规则或人工；link 只是标注 | JQL/看板视图聚合 | support.atlassian.com「Link work items」 |

**共性规律**：三家都把「关联」（结构）与「联动」（行为）分开做——关联是数据，联动是可选规则；没有任何一家做「BUG 全关自动关需求」的默认行为，GitHub 用 PR 关键字、Jira/Linear 用 automation 显式配。

## 对照五表设计：三分判断

**已覆盖（不必动）**
- 需求↔BUG 父子外键（bugs.need_id）≈ GitHub sub-issues / Linear parent——结构一致。
- 按需求聚合查询（pool_fetch(need_id=X) 及三条配方）≈ Projects 按父分组。
- status_log 溯源 ≈ 三家的 audit/活动流，且我们更细（每次变更一行）。

**有差距（低成本可补，建议 2 条）**
1. **父关闭前置补子项校验**：`pool_update` 关需求时 server 检查 bugs 子表是否仍有未关闭行，有则拒绝并报「该需求还有 N 条未关闭 BUG」——与既有「关闭须有 acceptance」同一道校验逻辑，一行级。对标 GitHub 的心智：子状态齐了父才能关（它靠人看 rollup，我们直接程序挡）。
2. （P2 备选）兄弟条目 blocked-by 依赖：bug↔bug「被谁挡住」。对标 GitHub dependencies。条目量 < 百时价值低，先不建 deps 表，真出现「A 不修完 B 不敢修」再上。

**不必抄**
- 多层嵌套（GitHub ≤8 层）：我们需求→BUG 两层已够，加层级是过度设计。
- Jira 17 种 link 类型矩阵：admin 可配的通用关联系统是为跨团队场景服务的，我们单团队单库，一种父子语义足够。
- automation 规则引擎：我们状态机校验（程序化拒非法迁移）已把最该自动化的部分做掉了，规则引擎属重量级后置。

## 建议落点

给惨品并入方案：§4 状态机加一条「关闭需求前置：bugs 子表无未关闭行（server 校验）」。改动一个函数。
