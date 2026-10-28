# 上线模块 minors 分诊清单（2026-10-28）

> 日期：2026-10-28。
> 来源：`docs/superpowers/plans/2026-10-28-release-audit-A-logic-data.md`（A-xx 编号）、`docs/superpowers/plans/2026-10-28-release-audit-B-ui-wiring.md`（m 系列）、文档线 spec 核对（u 系列）。
> **结论已按批次执行，本文件是留痕。**
> 编号口径：A-01/A-02 对应审计 A 的 A-01/A-02，A-03~A-13 对应审计 A 的 B-01~B-11，A-14 对应 B-14；审计 A 的 B-12（GitHub 同步整库 dump 是否排除 release 九表）为待拍板项、B-13（测试覆盖面缺口）随修复批补用例，均不占本清单编号。审计 B 的 blocker/major（B1/B2/B3/M1~M5）不在本 minors 清单：B1/B2/B3 已在前续批次落地（配置编辑层、跳过/移除入口、原子建单，代码实证）；M1/M2 分别与 A-01/A-02 同源；M3~M5 处置以对应修复批次记录为准。

## 分诊总览

| 档 | 数量 | 编号 |
|----|------|------|
| 修 | 14 | A-01 / A-04 / A-05 / A-07 / A-10 / A-11 / A-14 + m1 / m3 / u4 / u8 / u9 / u10 / u14（m3 并入 A-11） |
| 不修 | 2 | A-06、u1 |
| 已修（批次 3/4） | 3 | m2、u11、u12 |
| 仅记录 | 12 | A-02 / A-03 / A-08 / A-09 / A-12 / A-13 + u2 / u3 / u5 / u6 / u7 / u13 |

## 明细

| 编号 | 档 | file:line | 问题一句话 | 处置一句话 |
|------|----|-----------|-----------|-----------|
| A-01 | 修 | `src/module_release/release_logic.cpp:164-170` | 环境服务全部做完后「部署未完成」红条仍每轮报（判定只看「环境有未移除服务」） | dueItems 补完成度入参，仅对真有未完成步骤的环境发红 |
| A-04 | 修 | `src/module_release/release_store.cpp:118-143` | 外键约束失败上报 Internal，语义应为 NotFound/ValidationError | 写入口先查父行存在性，父 id 无效返回对应错误码 |
| A-05 | 修 | `src/module_release/release_logic.cpp:198-211` | validateTask 静默跳过 owner 缺失的配置（损坏数据当校验通过） | orphan 配置报告「归属异常」，不再静默放行 |
| A-07 | 修 | `src/core/ticker.cpp:99-104` | ticker 回调抛异常无保护，连带本轮其余订阅停摆 | 捕获单回调异常 + LOG_ERROR + 继续本轮；契约注明 |
| A-10 | 修 | `src/module_release/release_store.cpp:687-712` 等 | 数据层不校验日期格式/分钟范围/action 取值（绕过 UI 即产生「永不提醒」任务） | 写入口最小校验，非法输入返回 ValidationError |
| A-11 | 修 | `src/module_release/release_store.cpp:473-492` | listEnvs 每环境一条关联查询（N+1） | 单条 SQL 内存归组；m3（执行页 DST 口径）并入本票 |
| A-14 | 修 | `tests/test_modules/test_release_store.cpp:117-177` | 用例注释声称「任务快照不受影响」但未建快照、无断言（deleteService 误加级联不会红） | 本批已修：先建任务快照再删基础服务，逐字段断言快照不变 |
| m1 | 修 | `src/module_release/release_detail_ui.cpp:1078-1087` | 作废→恢复直接给 Pending/Running，绕过提交校验 | 从未提交过的任务恢复回 Draft |
| m3 | 修 | `src/module_release/release_detail_ui.cpp:128-145` | planMinuteNow 与提醒引擎 DST 口径不一致（DST 日偏移 ±60 分钟） | 并入 A-11，统一 mktime 口径 |
| u4 | 修 | `tests/probes/release_probe.cpp:625-626` | 注释声称「提醒引擎调度/抑制已有无头单测覆盖」，实际不存在 test_release_reminder* | 本批已修：注释改写为如实描述（提醒窗行为无自动断言） |
| u8 | 修 | spec §10 接入表 app_sidebar.h 行 | spec 写「默认展开」，实现为默认折叠 | 以实现为准改 spec（主仓副本已为「默认折叠」，本批核对确认，无需再改） |
| u9 | 修 | spec §10.3 已知限制与待办 | 「提交/服务拖拽尚未实现」失实（均已实现），真正未做的「分支编辑」没列 | 本批已修：按当前实现重写限制清单（提交/拖拽已实现，分支编辑未实现） |
| u10 | 修 | spec §10 模块接入点 | 「既有 8 行几何保持不变」不成立（上线父项占行、Settings/About 下移 44px） | 本批已修：改为「相对顺序与缩进不变，仅整体下移」 |
| u14 | 修 | spec §13.3 视觉契约表 / §5.1-5.2 | 二级项写「缩进 24」实为 kChildIndent=16；配置顺序写「可拖拽」实为上下移按钮 | 本批已修：两处改实现口径（缩进 16 / 顺序可上下移） |
| A-06 | 不修 | `src/module_release/release_logic.h:32` | 工单适用性注释漏写 live 环境/服务过滤（注释与实现语义不全） | 不修：仅注释口径问题，实现行为正确且已有测试覆盖 |
| u1 | 不修 | —（模块外） | 模块外历史文件 | 不修：超出本模块范围、历史遗留，不纳入本批 |
| m2 | 已修（批次 3/4） | `src/module_release/release_detail_ui.cpp:2781-2799` | applyContentRevocations 对 skipped 配置断链，doneAt 残留库中 | 批次 3/4 已修 |
| u11 | 已修（批次 3/4） | spec（主仓副本） | 文档线发现（spec 文案类漂移） | 批次 3/4 已修 |
| u12 | 已修（批次 3/4） | spec（主仓副本） | 文档线发现（spec 文案类漂移） | 批次 3/4 已修 |
| A-02 | 仅记录 | `src/module_release/release_logic.cpp:198-210,242-246` | 后端「固定 5 项配置」未落在纯逻辑，列表页进度分母与执行页不一致 | 记录理由：修复方案二选一（逻辑补期望槽位 / 建单时落 5 行）需拍板，暂不动已定的内存占位合成口径 |
| A-03 | 仅记录 | `src/core/schema.h:285-347` | 数据层唯一性（一天一任务/每环境一服务/每类配置一步骤）无 SQL 约束兜底 | 记录理由：加索引属 schema 变更，风险面大于收益，UI 单点保证暂接受 |
| A-08 | 仅记录 | `src/core/ticker.h:53-55` | hasSubscribers 把空回调订阅也算「有订阅」 | 记录理由：只影响空转优化语义，无实际故障 |
| A-09 | 仅记录 | `src/module_release/release_store.cpp:985-1005` | setStepDone(doneAt=0) 对从未完成过的步骤插一行 0 记录 | 记录理由：当前 UI 只在已有步骤上撤回，无可见后果 |
| A-12 | 仅记录 | `src/module_release/release_types.h:40-41`、`src/core/schema.h:349` | 日志 action 注释沿用旧口径（只写 confirm/revoke，实际还有 submit/reorder） | 记录理由：仅注释问题；主仓 spec §9 已列全 create/submit/confirm/revoke/void/restore |
| A-13 | 仅记录 | `src/module_release/release_logic.h:24-25` | nowMinute 契约未区分墙钟分钟与零点起流逝分钟（DST 日偏差 60 分钟） | 记录理由：中国时区无 DST；契约口径随 m3 修复一并收敛 |
| u2 | 仅记录 | `src/module_release/release_reminder.cpp:75-82` | 提醒三档配色为硬编码字面量，不走 theme token（深色档不跟随主题） | 记录理由：票据指定色系（代码已注明），改走 token 属后续主题工作 |
| u3 | 仅记录 | `src/module_release/release_reminder.cpp`（审计时 :591-595） | 临时诊断 LOG_INFO 遗留（注释自称可删） | 记录理由：无功能影响，随下次触碰该文件时清理 |
| u5 | 仅记录 | `src/module_release/release_dialogs.cpp:567-568` 等 | 空实现/无调用方死 API（drawFullConfigEditorDialog / drawWithdrawConfirmDialog / ReleaseViewState / demoModeActive） | 记录理由：不崩不误用，随配置编辑层收尾时一并清理 |
| u6 | 仅记录 | spec §13.3 视觉契约表 | probe 断言节点 id 写 `app.sidebar.child.Release.<id>.hit`，实现实为 `app.sidebar.item.ReleaseTasks.hit` | 记录理由：只影响表内节点引用；几何数值已随 u14 修正 |
| u7 | 仅记录 | spec §6.2 | 红档定义「部署未完成」与实现粒度不符（环境完成仍报红） | 记录理由：随 A-01 修复收敛口径，spec 侧不做单独改动 |
| u13 | 仅记录 | `tests/probes/release_probe.cpp` E 块 | 提醒窗（独立 OS 窗口）无任何自动化覆盖 | 记录理由：probe 无法驱动第二个窗口，留给 live 探针（与 u4 注释口径一致） |
