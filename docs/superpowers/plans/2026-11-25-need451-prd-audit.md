# need#451 PRD 独立审计记录（L 级审计门）

- 审计对象：文档台账 doc id=33（slug `2026-11-25-need451-proactive-push`，type=prd，need_id=451，version 1）
- 审计日期：2026-11-25
- 审计角色：product-manager（独立实例，非该 PRD 作者）
- 审计面：需求覆盖与目标达意性（用户视角，非实操）
- **结论：PASS_WITH_FIXES** —— 1 CRITICAL + 9 IMPORTANT + 6 NIT，全部文档内可修，不需推翻方案、不需新增功能点

## 六维结论摘要

| 维度 | 结论 | 相关 finding |
|---|---|---|
| 需求覆盖 | 18 个功能点覆盖 B/C 两档核心价值，无漏项；A 档边界定义与 C 档接入点存在矛盾 | A1 |
| 目标达意性 | 目标 1「3 秒内」与目标 4「不新增泵/线程」缺验收锚点 | C2 |
| 现状实证准确 | 智能体终态通知现状、托盘动态能力两处与代码不符 | B1、C3 |
| 状态集与边界完整 | 保留上限溢出规则、跨重启保留、定时 status 集、免打扰边界缺定义 | B4、B5、B6、B7 |
| 流程闭环与失败分支 | C-N2/C-N3 失败分支自相矛盾；首页折叠与抽屉 tab 两个「可选」元素未定 | C6、B9 |
| 文档规范与面向读者 | 矩阵缺文档列；面向老板段落夹技术词；F-13 与 D-01 未绑定取舍 | C1、C5、C7 |

六维均存在发现，均属文档内可修；无一处需要推翻既有方案或扩大功能范围。

## Findings 原文

### CRITICAL

【A1】§1.1「toast 行为保持不变」+非目标1「A 口径零变化」与目标2/F-14/F-16/F-10 验收矛盾：未定义 F-14 分流与 F-16 静默是否作用于 A 档投递。修法：把 A 档投递定义为「入账 + 分流（前台 toast / 后台系统通知 / 静默仅入账+圆点）」；§1.1 与非目标1 改为「触发/去重/聚合/排除口径不动，投递通道可配」；F-10/F-14/F-16 各补适用范围与验收（最小化到期→系统通知；静默期→零打扰但入账）。

### IMPORTANT

【B1】§1.2「智能体跑完→应用 toast」失实（实际 agent_module_schedule.cpp:856-862 只走 OS 通知且成功/取消静默；toast 仅为 app.notify 工具失败降级）。修法：更正现状行；F-11 写明「终态检测扩展（现只覆盖 running→failed/limited，agent_module_schedule.cpp:26-39）+ 复用 notifiedRunIds_ + 首启只记启动后新终态」，验收补「重启不重复入账」。

【B2】F-10 聚合入账会丢任务名（toast 正文最多 3 标题）：改为每任务一条入账（去重键 taskId+dueAt）或聚合条目列全本轮任务标题；明确 A 档重启补报的入账口径。

【B3】D-01「三类打扰」+F-13「仅入账」与现状冲突（用量告警已走系统通知，panel_state.cpp:579-585）：明示「保留现有系统通知，C 档只增入账不重复打扰」，并相应改 D-01 打扰清单表述。

【B4】收件箱跨重启保留是产品级属性：PRD 钉死「必须落库保留」；T-4 只定表形态；补「重启后通知与未读保留」验收。

【B5】定义保留上限溢出规则：优先淘汰最旧已读；无已读时淘汰最旧未读并在抽屉顶部提示「已达上限」；加验收。

【B6】F-12 补：入账 status 集（success/failed/skipped/missed/suppressed 取舍）、排除 origin='system:<module>' 系统行（schema.h:231-240）、启动后历史水位、通知时效（≤5s）；F-03 同步过滤口径。

【B7】F-16 验收改「设时段覆盖当前时刻」（白天可执行）；断言三件套（无 toast/无系统通知/有入账+圆点）+ 边界（起=止非法、跨零点）；A 边界补静默抑制声明。

【B8】F-14 依赖列注明 platform::window_visibility::mainWindowVisible()（window_visibility.h:1-33，fail-open=true）；C-N3 判定失败分支改为「按前台 toast（更安静）」并注明。

【B9】删 §6.3 notice.drawer.tab 的「可选」或把非目标8 改为「不做关键词搜索/分组」；home.section.collapse 与 tab 二选一升格为显式决策点（推荐/备选/影响）。

### NIT

【C1】矩阵加「文档」列（指向后续 ui/tech 卡）。

【C2】目标1「3 秒内」与目标4「不新增泵/线程」补验收锚点（首屏时延断言 + 线程/查询数守卫）或删时限。

【C3】订正托盘表述：setTrayMenu 已支持运行时更新（platform.h:96），F-15 的 POC 收窄为「图标叠加/标题」，「菜单项数字」降级无需 POC。

【C4】F-04 验收「改生产代码造错」改测试缝/标注仅 QA 环境。

【C5】D-01/D-02 面向老板段落「POC」「CI 挂」换白话（技术验证/自动构建失败）。

【C6】C-N2「入账失败仍走 C-N3」与 C-N3「已入账」矛盾：改为「入账失败→仅投递（不更新未读）+日志」。

【C7】F-13 取舍：D-01 拍板后二选一（保留或移非目标），本期标注。

## 复核证据（主会话抽验，2026-11-25）

- 智能体终态只走 OS 系统通知、completed/cancelled 静默：`src/module_agent/agent_module_schedule.cpp:856-862`
- 用量告警只走系统通知：`src/module_memory/panel/panel_state.cpp:579-585`（bug179）
- `platform::window_visibility::mainWindowVisible` 存在且平台拆文件（fail-open=true）：`src/platform/window_visibility.h:1-33`
- EUI-NEO `setTrayMenu` 支持运行时更新：`EUI-NEO/core/platform/platform.h:96`

## 修订环状态

- 本轮：product-manager 按本记录逐条修订 PRD → doc version 2（同 slug `2026-11-25-need451-proactive-push`）；闭环明细见 PRD「修订记录（version 2）」小节。
- 复审：修订后进入复审（本记录不改写；复审结论另出记录）。