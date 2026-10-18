# 智能体「任务详情」弹窗重设计 — 设计与实施 spec

> 日期：2026-10-18 ｜ 范围：`module_agent` 详情弹窗（`agent.dialog.detail`）
> 状态：草案 / 未实施（待评审）。本文只描述设计与实施契约，不代表代码已改。
> 关联实现：`src/module_agent/agent_module.cpp` 的 `composeTaskDetailDialog` / `renderMessageRow` / `refreshData`，`agent_store`，`cron`，`message_blocks`，`tests/test_modules/test_agent_ui.cpp`，`tests/probes/agent_dialog_probe.cpp`。

## 1. 目标

详情弹窗首先回答两个问题：**有没有按时跑、跑成没成**。消息链路退到第二层，默认折叠。

1. 顶部摘要一屏给出：最近一次（时间 / 状态 / 耗时）、下一次（定时=具体时间；每日=今天或明天首次调度；手动/事件=不造假）、近 7 天成功与失败统计。
2. 左栏按天分组的执行时间线：每行有开始时刻、状态（颜色/圆点/文字）、耗时；失败给一行原因摘要。默认选中最近一次，空态显示「从未执行」。
3. 右栏 run 详情：状态、开始/结束、耗时、迭代、输入/输出 token；定时任务显示「计划 vs 实际」和「按时 / 延迟 N 分」；失败错误全文可换行、可滚动；消息默认折叠，展开后看完整链路。
4. 任务配置改为可折叠的覆盖式浮层，字段分段显示，不再把所有信息拼成一条 12px 灰字。
5. 复制操作有可见 toast；状态色、字号、行高、间距全部走 token 或具名常量。
6. 弹窗宽度/高度必须按 `dialog` 实际钳制后的尺寸排版；窄屏退化为单列 tab，不写死屏幕绝对坐标。

## 2. 非目标

- 不改 Scheduler 的触发语义、cron 语法、run 落库状态机。
- 不改 `agent_runs` 表结构，不新增列，不写迁移，不为旧数据加兼容分支。
- 不做执行记录筛选、搜索、分页、图表、成功率趋势。
- 不做 run 的重试/取消/删除动作；详情弹窗本批只读。
- 不重做任务列表、任务编辑弹窗、通知中心。
- 不引入英文文案或本地化框架；文案保持现有中文口径。
- 不依赖 emoji 或特殊字体作为布局/语义的唯一载体。

## 3. Anti-goals（明确不采用的做法）

| # | 不做 | 原因 |
|---|---|---|
| A1 | 继续把配置拼成一条 `目标：… · 触发：… · cron：…` 文本 | 不可扫描，窄屏必溢出；配置项会继续增长 |
| A2 | 用请求宽 760 计算内容宽 | `dialog` 实际会把宽高钳到 `screen - 48`，窄屏内容会排到面板外 |
| A3 | 用固定屏幕坐标（如 `screen.width/2`）定位弹窗内容 | 窗口缩放、DPI、fontScale 下必错；所有坐标从 `actualW/actualH/innerW` 推导 |
| A4 | 为窄屏再写一套完整节点树 | ID 契约会分叉；只做布局分支，节点语义共用 |
| A5 | 消息默认全部展开以「方便看」 | 详情首要回答执行是否成功；消息是二线证据 |
| A6 | 伪造手动/事件任务的「下次运行时间」 | 这类任务没有可预测的下一次；显示「不会自动运行 / 等待事件」才是真话 |
| A7 | 为「计划 vs 实际」新增 `agent_runs.trigger_source` 列 | 本批禁止 schema 迁移；先用启发式并在 §15 标风险 |
| A8 | 用 `#ifdef _WIN32` / `__APPLE__` 处理时间、字体或布局 | 业务代码禁止平台分支；统一走 `core::time` 与主题 token |
| A9 | 在每帧渲染里逐条回扫 cron | 稀疏 cron 的 `previousCronMatch` 可能扫描数十万分钟；必须缓存 |

## 4. 设计决策摘要

| # | 决策 | 说明 |
|---|---|---|
| D1 | 弹窗实际尺寸 | `actualW = min(760, screen.width - 48)`；`actualH = min(660, screen.height - 48)`；内容宽 `innerW = actualW - 56`，左边距 `x0 = 28` |
| D2 | 宽屏判据 | `actualW >= 680 && actualH >= 520` → 左右分栏；否则单列 tab |
| D3 | 宽屏但矮 | `wide && actualH < 600`，或右侧消息剩余高 `< 96` → 右栏改用 `.detail.scroll` 整体滚动，不建 `.msgscroll` |
| D4 | 窄屏 | 摘要 + 配置头 + `.nav` 两 tab（执行记录 / 运行详情）；选中 run 自动切到「运行详情」 |
| D5 | 默认选中 | 最近一次 run；切换任务/重开弹窗回到最近一次 |
| D6 | 消息默认 | 整段消息默认折叠；`.msgs.copyfinal` 在标题行保持可见；展开后沿用「最终 assistant 默认展开、其余折叠」 |
| D7 | 配置 | 配置头 40 高；点击展开覆盖式浮层，浮层 z20、遮罩 z19，不挤压主区 |
| D8 | 时间线数量 | 详情任务单独查最近 **20** 条 run；任务列表的 `latestRuns_` 每任务只查 1 条 |
| D9 | 7 天统计 | 本地日历「今天及前 6 天」，按 `started_at`；成功=`completed`；失败=`failed + limited`；`limited` 另给子计数；`cancelled/running` 不计入 |
| D10 | 下一次 | schedule 用 `nextCronMatch`；daily 用「今天/明天首次调度（需应用运行）」；manual/event/resident 显示真实语义；disabled 优先（once 已有 run 时显示其运行/结束语义） |
| D11 | 计划对照 | schedule 用 `previousCronMatch` 取 run 开始前最近计划时刻；once 用 `once_at`；其它显示「无计划对照」 |
| D12 | 按时口径 | `delta <= 90s` = 按时；`90s < delta <= 30min` = 延迟 N 分（`N = max(1, ceil(delta/60))`）；`delta > 30min` = 计划外启动；无匹配 = 无计划对照 |
| D13 | 复制反馈 | 页内 `ui_kit::drawToast`，ID `agent.detail.toast`，默认 z 1100；toast 只做乐观提示（剪贴板 API 无返回值） |
| D14 | 字号 | 只用 `fontSizeSmall(13)` / `fontSizeBody(15)` / `fontSizeLarge(18)` / `fontSizeLarge+2(20)`；行高按比例派生 |
| D15 | 现有 ID | `.info` 退休；`.runs.title` 改为 `.runlist.title`；`.summary` 由文本改为容器 |

## 5. 信息架构与线框

### 5.1 宽屏 760×660（默认）

```text
┌──────────────────────────────────────────────────────────────────────┐
│ 任务详情                                                        ✕    │ y24 h28
│ ┌ 最近一次 ─────────┐ ┌ 下一次 ──────────┐ ┌ 近 7 天 ─────────┐    │ y60 h81
│ │ 成功 · 3分12秒     │ │ 明天 09:00        │ │ 成功 6 · 失败 3   │    │
│ │ 今天 14:32（2h前） │ │ 定时 · 15 小时后   │ │ 其中受限 1 · 67%  │    │
│ └───────────────────┘ └───────────────────┘ └───────────────────┘    │
│ ▸ 任务配置  定时 · 0 9 * * 1-5 · sonnet · 迭代 20        展开       │ y149 h40
│ ┌ 执行记录 ─────────┐ ┌ 运行详情 ───────────────────────────────┐    │ y201 h435
│ │ 今天              │ │ ● 失败   #42              3分12秒        │    │
│ │ 10-18 14:32:05 ●成功│ │ 开始 2026-10-18 14:32:05               │    │
│ │ 3分12秒            │ │ 结束 2026-10-18 14:35:17               │    │
│ │ 10-18 09:00:01 ●失败│ │ 耗时 3分12秒 · 迭代 7                   │    │
│ │ 错误：LLM 配置不可用│ │ 输入 190,000 · 输出 15,000              │    │
│ │ 去 Claude 页 →     │ │ 计划 09:00 · 实际 09:00:01 · 按时       │    │
│ │ 昨天              │ │ 错误 [可滚动全文]                       │    │
│ │ ...               │ │ 消息（6）         展开   复制回复       │    │
│ │                   │ │ [消息区 / 空态]                         │    │
│ └───────────────────┘ └────────────────────────────────────────┘    │
└──────────────────────────────────────────────────────────────────────┘
```

### 5.2 窄屏（`actualW < 680` 或 `actualH < 520`）

```text
┌──────────────────────────────────────────────┐
│ 任务详情                              ✕      │
│ ┌ 最近一次 ────────┐ ┌ 下一次 ──────────┐   │ 摘要第一行
│ └──────────────────┘ └───────────────────┘   │
│ ┌ 近 7 天 ───────────────────────────────┐   │ 摘要第二行
│ └────────────────────────────────────────┘   │
│ ▸ 任务配置  定时 · 0 9 * * 1-5     展开      │
│ [ 执行记录 | 运行详情 ]                       │ `.nav`
│ ┌──────────────────────────────────────────┐ │
│ │ 当前 tab 内容，独立滚动                    │ │
│ └──────────────────────────────────────────┘ │
└──────────────────────────────────────────────┘
```

### 5.3 宽屏但矮（`wide && actualH < 600`）

- 头部、摘要、配置头不变。
- 左栏仍是时间线 scroll。
- 右栏只有一个 `.detail.scroll`，从 run 状态一直滚到消息末尾；**不建** `.msgscroll`。
- 消息展开后直接排在 `.detail.scroll` 内容里，整栏滚动，避免嵌套滚动。

## 6. 布局公式与响应式

所有数值以 `fontScale = 1.0` 为基准；字号缩放后由公式派生（见 §9.4）。

```text
actualW = min(760, max(0, screen.width - 48))
actualH = min(660, max(0, screen.height - 48))
innerW  = max(0, actualW - 56)
x0      = 28
wide    = actualW >= 680 && actualH >= 520

titleY   = 24;  titleH = 28
closeX   = actualW - 56; closeY = 22; closeSize = 28
summaryY = 60
summaryH = wide ? 81 : (actualH < 600 ? 116 : 122)
configY  = summaryY + summaryH + 8
configH  = max(40, lhS + 20)
```

### 6.1 宽屏

```text
mainY = configY + configH + 12
mainH = max(56, actualH - mainY - 24)
leftW = clamp(round(innerW * 0.40), 240, 282)
rightW = innerW - leftW - 20
```

默认 760×660：`summaryY=60`、`configY=149`、`mainY=201`、`mainH=435`、`leftW=282`、`rightW=402`。
左栏与右栏间距 20；底边距 24；两栏顶部对齐 `mainY`。
左栏 `.runlist` = `leftW × mainH`（标题 22 + gap 8 + `.runlist.scroll` 高 `mainH - 30`）；右栏 `.detail` = `rightW × mainH`。
摘要三卡等宽：`cardW = (innerW - 2*8) / 3`（默认约 229）。

### 6.2 窄屏

```text
navH     = actualH < 600 ? 32 : 36
navY     = configY + configH + 8
contentY = navY + navH + 8
contentH = max(56, actualH - contentY - 24)
```

默认窄屏正常：`summaryH=122`、`configY=190`、`navY=238`、`contentY=282`。
窄屏紧凑（`actualH < 600`）：`summaryH=116`、`configY=184`、`navY=232`、`contentY=272`。
窄屏摘要两行：第一行最近一次 / 下一次各 `(innerW - 8) / 2`，第二行近 7 天占满 `innerW`。

窄屏只渲染当前 tab 的节点：
- tab 0：`.runlist`（时间线），高度 `contentH`。
- tab 1：`.detail.scroll`（详情 + 消息），高度 `contentH`。
- `.nav` 用 `components::segmented`，items = `{"执行记录", "运行详情"}`，生成 `.nav.hit.<i>` / `.nav.label.<i>`。
- 时间线点选 run 后：`selectedRunId_` 更新，`detailTab_ = 1`，`messagesOpen_ = false`，`configOpen_ = false`。

### 6.3 宽屏右栏的两种滚动模式

```text
// 正常宽屏：详情顶部固定，消息区独立滚动
detailTopH = 量出的详情块高度（状态/元信息/计划/错误）
msgRoom    = mainH - detailTopH - 12
if (wide && actualH >= 600 && msgRoom >= 96)
    -> 固定详情 + .msgs.title + .msgscroll(height = msgRoom)
else
    -> .detail.scroll(height = mainH) 包住详情 + 消息
```

- `.msgs.title` 行高 32；`.msgscroll` 高度必须 `>= 96`，否则走整栏滚动。
- 错误块出现时 `detailTopH` 变大，`msgRoom` 缩小；不足 96 自动降级，不硬压消息区。
- `.detail.scroll` / `.msgscroll` 的 `contentKey` 必须包含影响内容高度的字段（见 §8.6）。

### 6.4 配置浮层

```text
popoverX = x0
popoverY = configY + configH + 4
popoverW = innerW
popoverMaxH = actualH - popoverY - 24
```

- `.config.scrim`：从 `configY + configH` 到面板底，覆盖主区；点击关闭浮层。不覆盖标题/关闭按钮。
- `.config.popover`：`zIndex(20)`；`.config.scrim`：`zIndex(19)`。两者都是弹窗内容内的相对层，不改变 dialog 的 z=1000。
- `.config.popover.scroll` 高度 `min(内容高, popoverMaxH)`；内容超出时内部滚动。
- 浮层不改变主区 `mainY/mainH`；关闭后主区回到原位，测试可用前后 y 不变断言。

## 7. 元素树与 ID 契约

### 7.1 保留 / 改造的 ID

| ID | 变化 | 说明 |
|---|---|---|
| `agent.dialog.detail` | 保留 | 弹窗根 |
| `agent.dialog.detail.title` | 保留 | 标题，20/28 |
| `agent.dialog.detail.close` | 保留 | 关闭，28×28，字号 small |
| `agent.dialog.detail.summary` | **改容器** | 不再是文本；子节点见 7.2 |
| `agent.dialog.detail.runlist` | 保留 | 时间线根容器 |
| `agent.dialog.detail.runlist.<i>` | **改容器** | 第 i 条 run 行，带 onClick；文本在子节点 |
| `agent.dialog.detail.runlist.<i>.err` | 保留 | 失败原因一行摘要 |
| `agent.dialog.detail.runlist.<i>.err.hint` | 保留 | 仅 LLM 配置类错误渲染「去 Claude 页 →」 |
| `agent.dialog.detail.runlist.empty` | 保留 | 从未执行 |
| `agent.dialog.detail.msgs.title` | 保留 | 「消息（N）」标题行 |
| `agent.dialog.detail.msgs.copyfinal` | 保留 | 「复制回复」chip；折叠态也可见 |
| `agent.dialog.detail.msgscroll` | 保留 | 正常宽屏消息滚动区 |
| `agent.dialog.detail.msg.<i>` | 保留 | 第 i 条消息容器 |
| `agent.dialog.detail.msg.<i>.head` | 保留 | 消息头行 |
| `agent.dialog.detail.msg.<i>.badge` | 保留 | 角色徽章 |
| `agent.dialog.detail.msg.<i>.toggle` | 保留 | 展开/收起 |
| `agent.dialog.detail.msg.<i>.copy` | 保留 | 展开态逐条复制 |
| `agent.dialog.detail.msg.<i>.preview` | 保留 | 折叠态预览 |
| `agent.dialog.detail.msg.<i>.blk.<j>` | 保留 | 展开态第 j 块 |
| `agent.dialog.detail.msg.<i>.blk.<j>.out` | 保留 | tool_result 输出 |
| `agent.dialog.detail.empty` | 保留 | 选中 run 无消息的空态 |
| `agent.dialog.detail.info` | **退休** | 由 `.config.*` 替代 |
| `agent.dialog.detail.runs.title` | **退休** | 改为 `.runlist.title` |

### 7.2 新增 ID

| ID | 类型 / 行为 |
|---|---|
| `agent.dialog.detail.summary.last` | 最近一次卡片容器 |
| `.summary.last.label` / `.value` / `.sub` | 标签 / 主值 / 副值 |
| `.summary.next` / `.next.label` / `.next.value` / `.next.sub` | 下一次卡片 |
| `.summary.stats` / `.stats.label` / `.stats.value` / `.stats.sub` | 近 7 天卡片 |
| `agent.dialog.detail.config` | 配置区根（头 + 浮层） |
| `.config.header` | 配置头行，40 高，整体可点 |
| `.config.header.title` | 「任务配置」 |
| `.config.header.hint` | 折叠态摘要（触发 · cron · 模型 · 迭代） |
| `.config.header.toggle` | 「展开」/「收起」 |
| `.config.header.hit` | 覆盖整行的透明命中面，点击开合 |
| `.config.scrim` | 浮层遮罩，点击关闭 |
| `.config.popover` | 浮层，z20 |
| `.config.popover.scroll` | 浮层内滚动区 |
| `.config.body.<field>` | 字段行；`<field>` ∈ `goal/trigger/schedule/model/iterations/destructive/limit/problem/context` |
| `.config.body.<field>.label` / `.value` | 字段标签 / 值 |
| `agent.dialog.detail.nav` | 窄屏 segmented 根；子 ID `.nav.hit.<i>` / `.nav.label.<i>` |
| `agent.dialog.detail.runlist.title` | 「执行记录」标题 |
| `.runlist.count` | 「最近 N 次」/「仅显示最近 20 次」 |
| `.runlist.scroll` | 时间线滚动区 |
| `.runlist.day.<k>` | 第 k 个日期分组头 |
| `.runlist.<i>.bg` | 行背景（选中/悬停） |
| `.runlist.<i>.dot` | 状态圆点，8×8 |
| `.runlist.<i>.time` | 开始时刻 `MM-DD HH:MM:SS` |
| `.runlist.<i>.status` | 状态文字 |
| `.runlist.<i>.duration` | 耗时 |
| `.runlist.note` | 20 条上限提示 |
| `agent.dialog.detail.detail` | 右栏根（宽屏）/ 详情根 |
| `.detail.scroll` | 矮屏/窄屏整栏滚动区 |
| `.detail.empty` | 未选中 run 的空态 |
| `.detail.status` | 状态行容器 |
| `.detail.status.dot` / `.detail.status.text` | 状态圆点 / 文字 |
| `.detail.id` | `#runId` |
| `.detail.meta` | 元信息区（6 行单列，保证完整时间戳不截断） |
| `.detail.meta.started/.ended/.duration/.iterations/.input/.output` | 每项一行，各带 `.label` / `.value` |
| `.detail.schedule` | 计划对照行容器 |
| `.detail.schedule.plan` / `.actual` / `.verdict` | 计划 / 实际 / 判定 |
| `.detail.error` | 错误块容器 |
| `.detail.error.label` | 「错误详情」 |
| `.detail.error.scroll` | 错误全文滚动区 |
| `.detail.error.text` | 错误全文文本，`wrap(true)` |
| `agent.dialog.detail.msgs` | 消息区根 |
| `.msgs.toggle` | 「展开」/「收起」 |
| `.msgs.count` | 「消息（N）」 |
| `.msgs.body` | 展开后的消息容器 |
| `.msgs.error` | 消息读取失败 |
| `.msgs.hint` | 折叠态提示「展开查看完整链路」 |
| `.msg.<i>.time` | 消息时间；跨天显示 `MM-DD HH:MM:SS` |
| `agent.detail.toast` | 页级复制 toast；子 ID 由 `drawToast` 生成（`.message` / `.title` / `.bg` 等） |

### 7.3 元素顺序

宽屏右栏：`.detail.status` → `.detail.meta` → `.detail.schedule` → `.detail.error?` → `.msgs`。
时间线：`.runlist.title` → `.runlist.scroll` → `.runlist.note?`；空态用 `.runlist.empty` 替代 scroll。
配置：`.config.header` 常驻；展开时先画 `.config.scrim`，再画 `.config.popover`。
页级：先画页面/两张弹窗，最后画 `agent.detail.toast`，保证 toast 在 dialog 之上（z 1100 > 1000）。

## 8. 数据来源与缺失字段补法

### 8.1 最近一次 / 时间线

- 来源：`taskRuns_[taskId]`（`AgentStore::listRuns(taskId, 20)`）。
- 最近一次：`taskRuns_.front()`；列表已按 `(started_at DESC, id DESC)` 排序。
- 状态：`AgentRunRow::status`；未知态显示「—」。
- 时间：`startedAt`、`endedAt`；耗时 = `endedAt - startedAt`，负差钳 0；运行中 = `now - startedAt`。
- 失败摘要：`AgentRunRow::error` 原文，`utf8Truncate(..., 48)`；完整错误在右栏。
- 不改 `listRuns` SQL；只改调用 limit。

### 8.2 近 7 天统计（当前缺失）

`AgentTaskRunSummary` 增加：

```cpp
int64_t okRuns7d = 0;       // started_at >= windowStart && status='completed'
int64_t failedRuns7d = 0;   // started_at >= windowStart && status IN ('failed','limited')
int64_t limitedRuns7d = 0;  // started_at >= windowStart && status='limited'（failedRuns7d 的子集）
```

`AgentStore::listTaskRunSummaries(int64_t windowStartSec)` 增加窗口参数，SQL 在现有 all-time 聚合上追加：

```sql
SUM(CASE WHEN started_at >= ? AND status='completed' THEN 1 ELSE 0 END) AS ok_runs_7d,
SUM(CASE WHEN started_at >= ? AND status IN ('failed','limited') THEN 1 ELSE 0 END) AS failed_runs_7d,
SUM(CASE WHEN started_at >= ? AND status='limited' THEN 1 ELSE 0 END) AS limited_runs_7d
```

三个 `?` 都绑定同一个 `windowStartSec`（现有 `bindAll` 只支持位置参数）。

- 窗口起点：`sevenDayWindowStart(now)` = 本地今天 00:00 往前推 6 个日历日。用 `std::tm` 把 `tm_mday -= 6` 后 `mktime`，避免 DST 日按 86400 秒硬减。
- 现有 `totalRuns/okRuns/totalSeconds/lastEndedAt` 保留（兼容现有调用/测试），但详情摘要不再使用 all-time 成功率。
- 成功率分母 = `okRuns7d + failedRuns7d`；`limited` 计入失败，同时单独显示子计数。
- 缓存键：`store_->writeVersion()` **且** `windowStartSec`；跨天即使无写入也必须重查。
- 无 run 的任务不出现在聚合结果里；UI 用 0 值兜底。

### 8.3 消息时间戳（当前缺失）

`AgentMessageRow` 增加：

```cpp
int64_t createdAt = 0;   // agent_messages.created_at，NOT NULL
```

`AgentStore::listMessages` 的 SELECT 改为：

```sql
SELECT seq,role,content_json,tool_call_id,created_at
FROM agent_messages WHERE run_id=? ORDER BY seq
```

- `appendMessage` 已经写入 `created_at = time(nullptr)`；**不需要 schema 迁移**。
- 旧库/异常行若读出 0，消息头时间显示「—」，不崩。
- 消息时间以选中 run 的 `startedAt` 为参照：同一本地日显示 `HH:MM:SS`，跨日显示 `MM-DD HH:MM:SS`。

### 8.4 计划 vs 实际（当前缺失）

在 `cron.h/.cpp` 新增：

```cpp
// 从 fromSec 所在分钟（含）起向前逐分钟回扫，找最近一次命中；
// 上限 366*1440 分钟，到顶 fail(ValidationError, "一年内无匹配时间")。
// 纯函数：fromSec 由调用方注入；时区转换只走 core::time::localTime。
core::Result<std::tm> previousCronMatch(const CronExpr& c, std::time_t fromSec);
```

- 与 `nextCronMatch` 的边界不同：`previousCronMatch` **包含 fromSec 所在分钟**（run 在 09:00:30 启动，cron `0 9 * * *` 应算 09:00 计划）。
- `once` 用 `trigger_config.onceAt` 与 `startedAt` 直接比较，不走 cron。
- 计算只在选中 run 变化 / `startedAt` 变化 / 任务配置变化时做一次，结果存 `detailPlan_`；**禁止在 compose 里逐帧回扫**。
- 缓存键：`taskId + runId + startedAt + triggerType + triggerConfig`。

### 8.5 下一次（当前缺失）

`describeNextRun` 输入 `AgentTaskRow`、`latestRuns_[taskId]`、`nowSec`、`triggerConfigProblem` 结果：

| 条件 | 主值 | 副值 |
|---|---|---|
| `enabled == false`（schedule/event/daily，或 once 无 run） | 已停用 | 启用后才会自动运行 |
| 配置异常 | 配置异常 | 具体问题原文（error 色） |
| schedule 正常 | `今天/明天/MM-DD HH:MM` | `定时 · N 小时后` / `cron: ...` |
| schedule 解析失败 | cron 无效 | `parseCron` 错误原文 |
| schedule 一年内无匹配 | 一年内无触发时间 | `nextCronMatch` 错误原文 |
| daily，今天没跑过 | 今天首次调度 | 每日任务 · 需应用保持运行 |
| daily，今天已跑过 | 明天首次调度 | 每日任务 · 今天已运行 |
| once，最近一次运行中 | 运行中 | 一次性任务 · 已触发 |
| once，最近一次已结束 | 已结束 | 一次性任务 · 最近 `MM-DD HH:MM` |
| once，已错过且未跑 | 已错过 | `YYYY-MM-DD HH:MM` · 应用当时未运行 |
| once，未到点 | `今天/明天/MM-DD HH:MM` | 一次性任务 |
| manual | 手动触发 | 不会自动运行 |
| event 正常 | 等待事件 | 事件触发 · 事件名（有则列出） |
| resident | 等待入队 | 常驻任务 · 由队列触发 |
| 未知 trigger | 未知触发 | 原样回显 triggerType（error 色） |

- 所有时间用本地时区；`nowSec` 每帧只取一次 `core::time::nowUnix()`。
- `nextCronMatch` 只在分钟变化 / 配置变化 / 最近一次 run 变化时重算；缓存键包含 `nextRunMinute_ + taskId + latestRunId + latestStatus`。
- 没有可预测下一次时**不显示假时间**，这是产品契约。

### 8.6 刷新与缓存

- `refreshData()`：
  - 每个任务 `listRuns(task.id, 1)` → `latestRuns_`（列表状态/通知足够）。
  - 仅 `detailTaskId_` 对应任务 `listRuns(task.id, 20)` → `taskRuns_`。
  - 详情消息 `listMessages(selectedRunId_)`；失败写 `detailMessagesError_`，成功清空。
  - 选中 run 不在列表时回落到 `front()`；无 run 清空 `selectedRunId_`。
  - 计算/更新 `detailPlan_` 缓存；选择变化时清空消息展开态。
- `runSummaries_` 缓存按 `writeVersion + windowStartSec`；详情摘要用 7d 字段。
- `timelineScrollOffset_` / `detailScrollOffset_` 在换任务、换布局模式时归零；同一任务刷新保留。
- 运行中 `uiPolling_` 已保证每帧刷新；不做额外定时器。
- 滚动内容 key（避免 MeasureCache 复用旧高度）：
  - 时间线：`runs.size() + selectedRunId + first.status`；运行中耗时文本变化不改高度，不需要每帧换 key。
  - 消息区：`msgScrollVersion_ + messages.size() + messagesOpen_ + expandedMsgSeqs_.size() + fontScale`。
  - 整栏详情：`selectedRunId + messages.size() + messagesOpen_ + expanded 数 + error 字节数 + fontScale`。
  - 配置浮层：`goal/context 字节数 + triggerType + triggerConfig + problem + fontScale`。

### 8.7 新增常量与状态成员

```cpp
constexpr int kRunListLimit = 1;          // 任务列表只取最近一次
constexpr int kDetailRunListLimit = 20;   // 详情时间线

bool messagesOpen_ = false;
bool configOpen_ = false;
int detailTab_ = 0;                       // 窄屏 0=执行记录 1=运行详情
std::string detailMessagesError_;
std::optional<RunPlanInfo> detailPlan_;
std::string detailPlanKey_;               // taskId+runId+startedAt+triggerType+triggerConfig
std::optional<NextRunInfo> nextRunText_;
int64_t nextRunMinute_ = -1;              // now/60，跨分钟失效
int64_t nextRunTaskId_ = 0;
int64_t nextRunLatestRunId_ = 0;          // 最近一次 run 变化（含 once/daily）也失效
std::string nextRunLatestStatus_;
float timelineScrollOffset_ = 0.0f;
float detailScrollOffset_ = 0.0f;
uint64_t detailContentVersion_ = 0;       // 选择/展开/刷新时 bump
int64_t runSummariesSeenWindowStart_ = -1; // 7d 窗口缓存键的另一半
ui_kit::ToastState toast_;                // agent.detail.toast
```

- `openTaskDetail()`：清零 `selectedRunId_`、`detailMessages_`、`detailMessagesError_`、`expandedMsgSeqs_`、`autoExpandedSeq_`、`detailPlan_`、`detailTab_`、`messagesOpen_`、`configOpen_`、滚动偏移；`tasksDirty_ = true`。
- `closeTaskDetail()`：只退弹窗态，不清任务/run 缓存，不触发重查。

## 9. 视觉 token

### 9.1 状态色

| 状态 | 色 token | Dark | Light | 圆点 |
|---|---|---|---|---|
| completed 成功 | `t.colors.success` | `#788C5D` | `#5E7048` | 实心 |
| failed 失败 | `t.colors.error` | `#CF2D56` | `#B82850` | 实心 |
| limited 受限 | `t.colors.warning` | `#C29243` | `#9A6B30` | 实心 |
| cancelled 已取消 | `t.colors.textTertiary` | `#9A948A` | `#6B665E` | 实心 |
| running 运行中 | `t.colors.info` | `#6A9BCC` | `#4A7AAB` | 实心 |
| unknown | `t.colors.textTertiary` | `#9A948A` | `#6B665E` | 实心 |

- 状态不能只靠颜色：圆点 + 中文状态文字同时存在。
- 右栏状态行可用 `withAlpha(status, 0.14)` 作 pill 底、`0.40` 作边框；不再新增独立 ID，背景属于 `.detail.status` 容器。
- 选中时间线行背景 `t.colors.primarySoftBg`；左缘 2px `t.colors.primary`。
- 未选中行背景透明；hover 用 `t.colors.hover`。
- 卡片底 `t.colors.surfaceElevated`；边框 `t.colors.borderDefault`；主文字 `textPrimary`，次文字 `textSecondary`，弱文字 `textTertiary`。

### 9.2 字号与行高（fontScale = 1.0）

| 用途 | 字号 | 行高 |
|---|---|---|
| 弹窗标题 | `fontSizeLarge + 2` = 20 | 28 |
| 区块标题 / 配置头标题 / 详情状态 | `fontSizeBody` = 15 | 22 |
| 摘要主值 / 配置值 | `fontSizeBody` = 15 | 21 |
| 摘要标签 / 摘要副值 / 时间线三列 / 元信息标签 / 错误 / 消息正文 | `fontSizeSmall` = 13 | 18 或 20 |
| 消息徽章 / 时间 / 预览 | `fontSizeSmall` = 13 | 20 |
| chip 文字 | `fontSizeSmall` = 13 | 20 |

- 新详情代码**不再出现** 11/12/16/18 的字号字面量；需要 18 时用 `t.sizes.fontSizeLarge`。
- 行高派生：
  ```cpp
  lhS = std::lround(fsS * 1.4f);          // 13 -> 18
  lhB = std::lround(fsB * 1.4f);          // 15 -> 21
  lhL = std::lround(fsL * 1.35f);         // 18 -> 24
  lhT = std::lround((fsL + 2.0f) * 1.4f); // 20 -> 28
  ```
- `actionChip` 增加可选 `fontSize` 参数（默认 12，保持任务列表/其它站点不变）；详情传 `fsS`，高度仍 32。
- 消息正文/预览块统一 `fsS / max(lhS, 20)`；`estimateMessageHeight` 必须接收字号/行高，不能继续写死 12/18。

### 9.3 间距

| 位置 | 值 / token |
|---|---|
| 卡片内边距 | 12（`metrics.spacing.content`） |
| 紧凑卡片内边距 | 10（`metrics.spacing.control`） |
| 卡片之间 | 8（`metrics.spacing.compact`） |
| 左右栏间距 | 20（`metrics.spacing.large`） |
| 区块之间 | 12（`metrics.spacing.content`） |
| 行内小 gap | 4（`metrics.spacing.tiny`） |
| 行内常规 gap | 6（`metrics.spacing.small`） |
| 页面底边距 | 24（`metrics.spacing.panel`） |
| 配置字段行距 | 10 |
| 配置标签宽 | 64 |
| 时间线行高 | 52；带 `.err` 68；带 `.err.hint` 84 |
| 消息头高 | 24 |
| 消息条间距 | 6 |

### 9.4 fontScale 接法

- `core::theme::setFontScale` 只缩放 `SizeTokens.fontSize*`，间距/圆角不缩放（现有契约）。
- 详情布局必须用缩放后的 `t.sizes.fontSize*` 派生所有文本高度：
  - 摘要高度：`max(81, 2*10 + lhS + lhB + lhS + 4)`（摘要卡 pad=10，1.0 时正好 81）；窄屏两行再加一行 `lhS + 8`。
  - 配置头：`max(40, lhS + 20)`。
  - 导航：`max(36, lhS + 12)`（紧凑再减 4）。
  - 时间线行：`max(52, lhS + 34)`；带错误再加 `lhS + 4`。
  - 卡片/详情块高度全部由文本高度累加，不写死 18/20 的行高。
- 若摘要/配置头加总超过 `actualH * 0.55`，摘要进入「紧凑文本」：隐藏 `.summary.*.sub`，主值单行，卡片高收缩到 `lhB + 2*pad`；保证主区至少 56 高。
- `fontScale` 变化后滚动内容 key 要变，避免 MeasureCache 复用旧高度。

## 10. 状态与边界行为

| 场景 | 行为 |
|---|---|
| 无 run | 时间线 `.runlist.empty`「从未执行」；摘要最近一次「从未执行」、统计「近 7 天暂无执行」；右栏 `.detail.empty`「选择一次执行查看详情」 |
| 有 run 未选中 | 默认选中 `front()`；宽屏右栏显示该 run；窄屏默认 tab 0 |
| 加载 | 数据在 `refreshData()` 同步查库，不引入异步 loading 帧/骨架屏；`store_ == nullptr` 时显示「数据库未就绪」；查询失败走错误态 |
| 运行中 | 状态「运行中」info 色；耗时「进行中 · N秒」，结束「—」；token 未落库显示「—」；消息随轮询追加 |
| 失败 | 时间线一行摘要；右栏 `.detail.error.scroll` 完整错误，`wrap(true)`，可滚动；`.err.hint` 仅配置类错误 |
| 消息读取失败 | `.msgs.error` 红字「消息读取失败：…」；标题保留；不展示旧消息 |
| 摘要查询失败 | `.summary.last.value` / `.summary.stats.value` 显示「读取失败」，error 色；`.summary.next` 仍按任务配置显示 |
| 任务读取失败 | `.config.header.hint`「任务信息读取失败」；浮层字段值「—」；其余按缓存/空态 |
| 长目标/上下文 | 配置浮层内 `wrap(true)` + 估算高度 + 浮层滚动；不截断成单行 |
| 长错误 | 时间线截断；右栏全文换行 + `.detail.error.scroll` |
| 长消息 | `.msgscroll` 内滚；块文本 `maxWidth` 必须显式传给 `estimateTextBlockHeight` |
| 无消息 | `.empty`「（选中的 run 还没有消息）」；消息标题仍显示「消息（0）」 |
| 20 条以上 run | `.runlist.note`「仅显示最近 20 次」；不提供分页 |
| 窄屏 | 单列 tab；切 run 自动切详情；配置浮层覆盖主区 |
| 宽屏矮 | 右栏 `.detail.scroll` 整栏滚动；不建 `.msgscroll` |
| 窗口缩放到阈值 | 重新 compose 时按实际尺寸切换；滚动偏移归零，避免旧偏移越界 |
| 复制 | 立即 toast「已复制」+ 内容说明；剪贴板 API 无返回值，失败无法校验，只 LOG 留痕 |

消息渲染补充：

- 头行顺序：`.badge` → `.time` → `.toggle` → `.copy`（`.copy` 仅展开态）；头高 24。
- 工具块文案从 `🔧 name` 改为 `工具 · name  args`（不用 emoji）。
- 折叠预览、正文、工具输出统一 `fontSizeSmall` / 20 行高；`maxWidth = 消息内容宽`。
- 消息区展开/折叠、消息内展开/折叠、切换 run、刷新新增消息，都要 bump 滚动 `contentKey` 版本。

## 11. 时间与文案 helper

新增 `src/module_agent/agent_time_text.h/.cpp`（module 内部纯 helper，UI 与测试共用）：

```cpp
std::string formatLocalDateTime(int64_t sec);        // YYYY-MM-DD HH:MM:SS
std::string formatLocalShortDateTime(int64_t sec);   // MM-DD HH:MM:SS
std::string formatLocalClock(int64_t sec);           // HH:MM:SS
std::string formatLocalDate(int64_t sec);            // YYYY-MM-DD
std::string localDayLabel(int64_t sec, int64_t nowSec); // 今天 / 昨天 / YYYY-MM-DD
std::string relativeTimeText(int64_t sec, int64_t nowSec); // 刚刚 / N 分钟前 / N 小时前 / N 天前
std::string formatScheduleTime(int64_t sec, int64_t nowSec); // 今天 09:00 / 明天 09:00 / MM-DD HH:MM
// cron 返回的是本地 tm（无 epoch）：直接格式化，避免 mktime 在 DST 缺失小时上二次换算
std::string formatLocalDateTime(const std::tm& local);
std::string formatLocalShortDateTime(const std::tm& local);
std::string formatScheduleTime(const std::tm& local, int64_t nowSec);
std::string formatRunDurationText(int64_t seconds);  // 0秒 / 45秒 / 2分13秒 / 1时23分 / 2天3时
std::string formatTokenCount(int64_t tokens);        // 190,000；0 -> 0
std::string runStatusText(const std::string& status);
std::time_t startOfLocalDay(std::time_t sec);
std::time_t sevenDayWindowStart(std::time_t nowSec);

enum class TextTone { Normal, Muted, Warning, Error };
struct NextRunInfo {
    std::string value;
    std::string detail;
    TextTone tone = TextTone::Normal;
};
struct RunPlanInfo {
    bool hasPlan = false;
    std::string plan;      // MM-DD HH:MM 或 "—"
    std::string actual;    // MM-DD HH:MM:SS
    std::string verdict;   // 按时 / 延迟 N 分 / 计划外启动 / 无计划对照 / 错误
    TextTone tone = TextTone::Muted;
};
NextRunInfo describeNextRun(const AgentTaskRow& task, const AgentRunRow* latest,
                            int64_t nowSec);   // 内部用 triggerConfigProblem
RunPlanInfo describeRunPlan(const AgentTaskRow& task, const AgentRunRow& run,
                            int64_t nowSec);  // schedule 走 previousCronMatch
```

- `startOfLocalDay`：`localTime` → `tm_hour=0/min=0/sec=0/isdst=-1` → `mktime`。
- `sevenDayWindowStart`：本地今天 00:00 的 `tm_mday -= 6` → `mktime`。
- 旧 `formatRunResultTimeText` 的测试改为 `formatLocalDateTime`（多出秒）；不要保留两套时间格式实现。测试头文件从 `agent_module.h` 迁移到 `agent_time_text.h`。
- 退役 `formatTaskRunSummaryText` 与匿名 `runRowText`；不要再在 UI 里拼「状态 · 耗时 · 迭代 · token」长串。

### 11.1 时间显示口径

| 位置 | 格式 | 示例 |
|---|---|---|
| 时间线行 | `MM-DD HH:MM:SS` | `10-18 14:32:05` |
| 时间线日期分组 | 今天 / 昨天 / `YYYY-MM-DD` | `今天` |
| 摘要最近一次主值 | 状态 · 耗时 | `成功 · 3分12秒` |
| 摘要最近一次副值 | `MM-DD HH:MM:SS · 相对` | `10-18 14:32:05 · 2 小时前` |
| 摘要下一次 | `今天/明天 HH:MM` 或语义文案 | `明天 09:00` |
| 右栏开始/结束 | `YYYY-MM-DD HH:MM:SS` | `2026-10-18 14:32:05` |
| 右栏计划 | `MM-DD HH:MM` | `10-18 09:00` |
| 消息头 | 同日 `HH:MM:SS`；跨日 `MM-DD HH:MM:SS` | `14:32:10` |
| 耗时 | 紧凑中文 | `3分12秒` |

- 统一本地时区；不显示 UTC 或时区缩写。
- 相对时间只用于辅助，不替代绝对时间；绝对时间必须能在同一屏找到。
- 跨天判断用 `core::time::localDayNumber`，不用 `tm_yday` 直接减。

### 11.2 计划对照文案

| 情况 | plan | actual | verdict |
|---|---|---|---|
| schedule，找到计划 | `MM-DD HH:MM` | `MM-DD HH:MM:SS` | 按时（success）/ 延迟 N 分（warning）/ 计划外启动（warning） |
| schedule，无匹配/回扫超限 | `—` | 开始时刻 | 无计划对照（tertiary） |
| schedule，cron 坏 | `—` | 开始时刻 | 无法计算计划时间（error） |
| once，有 `once_at` | `MM-DD HH:MM` | 开始时刻 | 按时 / 延迟 N 分 / 提前启动 |
| once，缺 `once_at` | `—` | 开始时刻 | 缺少 once_at（error） |
| daily / manual / event / resident | `—` | 开始时刻 | 无计划对照（tertiary） |

- 计划时刻只对**选中 run** 计算；切换 run 后重算并缓存。
- 计划对照是启发式：手动启动一个 schedule 任务可能被判为「延迟/计划外启动」。本批不新增 `trigger_source`；列入 §15 未决点。

## 12. 测试影响

### 12.1 必须保留的断言

- 详情弹窗开/关、Esc 关闭、点遮罩关闭。
- 关闭后任务列表不被回写；重开回到最近一次 run。
- `.runlist.<i>.err` 以 `错误：LLM 配置不可用` 开头；`.err.hint` 文案 `去 Claude 页 →` 可点并跳转。
- `.msgs.copyfinal` 在有最终回复时存在、无回复时不渲染；点击不崩。
- `.msg.<i>.toggle` / `.copy` / `.blk.<j>.out` 的行为。
- 面板底不被内容越出；窄屏下内容在面板内。
- `.detail.empty` / `.runlist.empty` / 消息空态的垂直居中或清晰可见。

### 12.2 需要替换的现有断言

| 现有测试 / 断言 | 替换 |
|---|---|
| `.info` 存在 + 文案含目标/定时/cron/sonnet/迭代/破坏性 | 先点 `.config.header.hit`，断言 `.config.body.goal.value`、`.trigger.value`、`.schedule.value`、`.model.value`、`.iterations.value`、`.destructive.value` |
| `.runs.title` | `.runlist.title` |
| `.summary` 文本「共 3 次 / 成功率 66% / 总耗时 / 最近结果」 | `.summary.last.value/.sub`、`.summary.stats.value/.sub`；文案改为近 7 天口径 |
| `.runlist.0` 的 `成功` / `token —` | `.runlist.0.status` / `.runlist.0.duration`；token 在 `.detail.meta.input/output.value` |
| `.runlist.1` 的 `受限` / `205000` | `.runlist.1.status`；token 在右栏 |
| 详情默认可见 `.msg.1.blk.0` | 先点 `.msgs.toggle`，再断言最终 assistant 块；默认断言 `.msgs.body` / `.msgscroll` 不存在 |
| `msg.2` 展开后高度增大 | 保留，但先展开消息区；高度比较相对 `.msg.2` |
| `.summary` 的 y=100、runs.title y=122/104、runlist y=144/126 | 删除绝对 y；改为「summary 底 + gap ≤ config 顶」「config 底 + gap ≤ main 顶」「main 底 ≤ 面板底 - 24」 |
| 消息区底 ≤ 面板底 - 19.5 | 改为 `.msgscroll` / `.detail.scroll` 底 ≤ 面板底 - 24 + 0.5 |
| 3 条 error 行互不重叠且不压消息标题 | 改为每个 `.runlist.<i>` 固定行高、相邻行不重叠；error 在行内，不再与消息标题竞争 |
| 汇总行存在才下移 18 | 摘要始终存在；改为断言摘要/配置/主区按 §6 公式排列 |
| `test_message_blocks.cpp` 的 `estimateMessageHeight/estimateMessagesAreaHeight` 调用 | 补 `fontSize/lineHeight` 参数；断言使用 13/20，不再用 12/18 |
| `test_agent_ui.cpp` 文件头的详情 ID 契约 | 改成 `.config.*` / `.runlist.*` / `.detail.*` / `.msgs.*` 新契约 |

### 12.3 新增用例

| 用例 | 断言 |
|---|---|
| `agent_time_text_formats` | 绝对/短/时钟/日期、今天/昨天/更早、相对时间、时长、token 千分位；用 `localTime` 构造期望，避免时区脆弱 |
| `agent_time_text_seven_day_window` | `startOfLocalDay` / `sevenDayWindowStart` 跨月、跨年、DST 日不漂 |
| `agent_cron_previous_match` | 含当前分钟；跨天；DOW 过滤；闰日一年内无匹配返回 fail；`* * * * *` 返回当前分钟 |
| `agent_store_list_messages_created_at` | `appendMessage` 后 SQL 改 `created_at=12345`，`listMessages` 读回 12345 |
| `agent_store_run_summaries_7d` | 1 天内 completed/failed/limited + 8 天前 completed；断言 7d 计数排除 8 天前 |
| `agent_ui_detail_timeline_day_groups` | 今天/昨天/8 天前 run；`.runlist.day.0/1/2` 文案；20 条上限 `.runlist.19` + `.runlist.note` |
| `agent_ui_detail_timeline_select` | 点 `.runlist.1` 选中；窄屏自动 `detailTab_=1`；消息区重置折叠 |
| `agent_ui_detail_next_run_semantics` | schedule 具体时间；daily 今天/明天；manual/event 不造假；disabled/配置异常显形 |
| `agent_ui_detail_schedule_vs_actual` | 按时 / 延迟 / 计划外 / 无计划对照 / cron 坏 |
| `agent_ui_detail_messages_collapsed_by_default` | 初始无 `.msgs.body` / `.msgscroll`；`.msgs.copyfinal` 可见；展开后最终 assistant 默认展开 |
| `agent_ui_detail_error_scroll` | 长错误 `.detail.error.text` 全文、`.detail.error.scroll` 存在、底不越面板 |
| `agent_ui_detail_config_popover` | 点头开浮层；主区 y 不变；浮层底 ≤ 面板底；点遮罩关闭 |
| `agent_ui_detail_copy_toast` | 点 `.msgs.copyfinal` / `.msg.<i>.copy` 后 `agent.detail.toast.message` 有文案且 root opacity=1 |
| `agent_ui_detail_narrow_tabs` | 700×560：panel 652×512、`.nav` 存在、tab 切换、无左右分栏 |
| `agent_ui_detail_short_wide_scroll` | 900×600：panel 760×552、`.detail.scroll` 存在、`.msgscroll` 不存在 |
| `agent_ui_detail_font_scale` | `setFontScale(1.5)` 后摘要/配置头高度增长、文字不裁切、滚动区仍可用；用例结束恢复 1.0 |
| `agent_ui_detail_task_read_failure` | 删任务后开详情：`.config.header.hint` 读取失败、字段值「—」、不崩 |

### 12.4 探针（真窗口）

`tests/probes/agent_dialog_probe.cpp` 保留现有 A（任务弹窗）与 B（详情弹窗）流程，B 增加：

- `.summary.last.value` / `.summary.next.value` / `.summary.stats.value` 至少一个非空；默认窗口 1200×1000 下 `.nav` 不存在。
- `.config.header.hint` 非空；点 `.config.header.hit` 后 `.config.popover` 存在，再点 `.config.scrim` 关闭。
- `.runlist.0.time` 长度 14 且形如 `MM-DD HH:MM:SS`；`.runlist.0.status` 非空。
- `.detail.status.text` 与种子 run 状态一致；`.detail.error.text` 含种子错误全文；`.detail.error.scroll` 存在。
- 默认 `.msgs.body` / `.msgscroll` 不存在；点 `.msgs.toggle` 后 `.msgscroll` 存在，`.msg.<i>.blk.0` 可见。
- 点 `.msgs.copyfinal` 后 `agent.detail.toast` 的 `.message` 文案含「已复制」。
- `--shot-detail` 截图覆盖宽屏；用 `--window-size 700x560` 跑同一探针时断言 `.nav` 存在、无左右分栏（可新增 ctest 条目或手工命令）。

探针失败仍以非零退出码为准；截图失败也算失败，不能只留图不判。

### 12.5 测试时间夹具

- 现有汇总测试把 run 时间钉在 1970 年（1000/1100/1200），新 7 天统计会全部落空。改为 `now - 1h` / `now - 1d` / `now - 8d`，并在 SQL 里写 `started_at/ended_at`。
- 不断言「今天」的具体日期字符串；用 helper 生成期望，避免跨零点/时区红。
- 几何断言优先用相对关系与 `frameContained`，不写 `panel.y + 122` 这类绝对偏移。

## 13. 分阶段实施

| 阶段 | 内容 | 依赖 | 文件范围 | 退出条件 |
|---|---|---|---|---|
| P1 布局壳 | 实际宽高、宽/窄分支、摘要/配置/主区骨架、`.nav`、滚动容器、`toast_` 成员 | 无 | `agent_module.h/.cpp` | 760×660 / 700×560 / 900×600 三种尺寸节点树正确；现有测试按 §12.2 同步改 |
| P2 时间线与摘要 | `agent_time_text`、`startOfLocalDay`、7d summary 查询、时间线按天分组、摘要三卡、20 条 run | P1；store/time helper | `agent_time_text.*`、`agent_store.*`、`agent_module.*`、`tests/CMakeLists.txt`、`src/module_agent/CMakeLists.txt` | 时间线/摘要用例绿；现有 `.summary` 文本断言替换 |
| P3 run 详情 | 状态/元信息/计划对照/错误滚动、`previousCronMatch`、计划缓存 | P2；cron | `cron.*`、`agent_time_text.*`、`agent_module.*` | 计划/错误/运行中用例绿；`previousCronMatch` 单测绿 |
| P4 配置与消息 | 配置浮层分段字段、消息默认折叠、消息时间、工具 chip 文案、复制 toast | P1/P2 | `agent_module.h/.cpp`、`message_blocks.*`、`ui_kit/toast` 仅调用 | 配置/消息/复制用例绿；探针可跑 |
| P5 测试与探针 | 新用例补全、探针断言/截图、全量回归、跨平台构建 | P1–P4 | `tests/test_modules/*`、`tests/probes/*`、`tests/CMakeLists.txt` | `bash scripts/build-test-run.sh` 全绿；Windows 构建通过 |

实施纪律：

- 每个阶段同步改对应测试，不把整套测试留到最后一次爆炸。
- 不修改 `cc-switch/`、不提交、不构建；本 spec 落地后由实施者按阶段执行。
- `agent_time_text.cpp` 必须加入 `src/module_agent/CMakeLists.txt` 的显式源列表；`test_agent_time_text.cpp` 必须加入 `tests/CMakeLists.txt`。
- 新增文件避免 `#ifdef`；时间转换只走 `core::time`。

## 14. 跨平台注意

| 项 | 要求 |
|---|---|
| Windows CJK 字体 | 现有回退已含 `msyh.ttc` / `simhei.ttf` / `seguiemj.ttf`；新 UI 不依赖 emoji（工具 chip 用「工具 · name」，不用 🔧） |
| DPI / 逻辑像素 | 所有布局用逻辑像素；不读物理分辨率、不按像素密度手调；DPI 由 EUI-NEO 缩放 |
| 固定行高裁字 | 时间线行、摘要卡、消息头等固定高度必须按 `fontScale` 派生；长文本走 wrap + 滚动，不靠截断掩盖 |
| 时间 | 只用 `core::time::localTime/nowUnix/localDayNumber`；`std::mktime` 用于本地日边界；不在业务代码写平台分支 |
| 换行/省略 | 时间线错误摘要允许 `utf8Truncate`；右栏错误全文必须 `wrap(true)`；消息块 `maxWidth` 显式传入 |
| 剪贴板 | Windows/macOS 实现已有；`setClipboardText` void，toast 只能乐观提示 |
| 构建 | macOS arm64 与 Windows 11 都要过；Windows 侧注意 MinGW 下 `std::lround`/`snprintf` 包含 `<cmath>/<cstdio>` |
| 字体缺失 | 测量为 0 时 `estimateTextBlockHeight` 退 1 行；不因字体缺失把高度算成 0 |

## 15. 风险与未决点

| # | 风险 / 未决 | 现状处置 | 需要拍板 |
|---|---|---|---|
| R1 | 时间线从 5 条扩到 20 条，`listRuns` 查询量增加 | 仅详情任务查 20；任务列表只查 1 | 20 是否够；是否要「加载更多」 |
| R2 | `AgentMessageRow.createdAt` 需要改 SELECT | 表已有 `created_at`，不改 schema | 是否接受消息时间取写入时刻（同一秒多条会并列） |
| R3 | 7 天窗口口径 | 本地日历今天及前 6 天；`started_at`；limited 计失败并单列 | 是否改为「最近 168 小时」；limited 是否算失败 |
| R4 | daily 文案「今天首次调度」可能被理解成「首次打开」 | 副值明确「需应用保持运行」 | 是否改成「今天尚未运行」 |
| R5 | 计划对照缺 trigger source | 启发式：≤90s 按时，≤30min 延迟，>30min 计划外 | 是否接受手动启动 schedule 任务被判「计划外」；未来是否加 `agent_runs.trigger_source` |
| R6 | `previousCronMatch` 最坏 366×1440 次 `localTime` | 只对选中 run 算一次并缓存；超限显示「无计划对照」 | 闰日/稀疏 cron 是否要更长回扫或解析式求解 |
| R7 | 历史 run 用当前 cron 对照，配置改过会误判 | 本批接受；缓存键含当前配置 | 是否记录 run 创建时的计划快照（需 schema） |
| R8 | 复制 toast 乐观提示 | API 无返回值，失败只 LOG | 是否接受；是否需要「复制失败」兜底 |
| R9 | 窄屏阈值 `actualW<680` 是经验值 | 700×560 面板 652 走单列；900×600 走宽屏矮 | 是否调整阈值 |
| R10 | 摘要/配置头在 fontScale 2.0 下挤压主区 | 超过 `actualH*0.55` 进入紧凑摘要、隐藏副值 | 是否允许摘要滚动而非压缩 |
| R11 | 消息展开区在矮屏变成整栏滚动，滚动位置与消息内滚动不同 | 矮屏不建 `.msgscroll`，避免嵌套 | 是否接受滚动语义变化 |
| R12 | 运行中耗时每秒变化但 `contentKey` 不变 | 行高固定，文本变化不需重测；只在高度相关字段变化时换 key | 是否需要每秒强制重绘 |
| R13 | 时间线 20 条全是错误行时左栏很高 | 左栏独立滚动，行高固定 | 是否要错误折叠 |
| R14 | 配置浮层覆盖主区，用户可能迷失 | 遮罩只盖主区、标题/关闭仍可用；头行可再次点「收起」 | 是否需要浮层内「关闭」按钮 |
| R15 | 手动/事件「下一次」显示语义而非时间，可能与用户预期不同 | 明写「不会自动运行 / 等待事件」，不伪造时间 | 是否改成「无计划」并隐藏卡片，或增加最近事件时间 |

## 16. 验收清单

- [ ] 760×660：标题/摘要/配置头/左右栏坐标符合 §6；无节点越出面板底。
- [ ] 700×560：面板 652×512，单列 `.nav`，时间线/详情可切；选中 run 自动切详情。
- [ ] 900×600：宽屏矮，`.detail.scroll` 存在，`.msgscroll` 不存在。
- [ ] 摘要三卡回答最近一次、下一次、近 7 天；手动/事件不显示假时间。
- [ ] 时间线按天分组，行有 `MM-DD HH:MM:SS`、状态色/圆点、耗时、失败摘要。
- [ ] 右栏有状态、开始/结束、耗时、迭代、输入/输出 token；定时任务有计划对照。
- [ ] 错误全文可换行可滚动；`.err.hint` 文案/跳转不回归。
- [ ] 消息默认折叠；`.msgs.copyfinal` 折叠态可见；展开后最终 assistant 默认展开。
- [ ] 配置浮层分段字段，覆盖主区但不改变主区几何。
- [ ] 复制有 `agent.detail.toast` 可见反馈。
- [ ] `fontScale=1.5` 不裁字；Windows/macOS 构建通过。
- [ ] 现有基线中受影响的用例已替换，新增用例与探针全绿（总数以运行输出 `Total:` 为准，不写死）。
