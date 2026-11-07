# 全站弹窗基础样式统一规范（设计文档）

- 池条目：need 156（[pool:need:156]）｜日期：2026-11-07｜角色：ui-ux-designer
- 老板规格（2026-11-06 原话，逐条落实于 §3）：① 顶部：标题区；标题下方一根分隔线；线下方留少量空隙；② 底部：按钮区——按钮统一靠右；主按钮（应用/确认）主色；副按钮（关闭/取消）次级色；按钮区上方一根细线；③ 中间：内容区；④ 弹窗最小尺寸限制。
- 事实基线：clue 18（theme token 实值表）、clue 105（弹窗现状盘点与 EUI-NEO dialog 实值，本任务新查证）
- 范围：站内自绘弹窗（`components::dialog` + `ui_kit/dialog_form` 接线）。**platform/message_box（macOS/win 原生系统弹窗）不在本规范域**；弹窗层平台无关，无 Windows 侧差异设计。

---

## 1. 页面级构图（先整体后参数）

### 1.1 构图三问

| 问 | 答 |
|---|---|
| ① 滚动模型对吗 | 三层结构：**遮罩层不滚**（吞滚轮，框架 backdrop onScroll 空实现即此行为）、**弹窗面板固定不滚**、**仅内容区内滚**（scrollview）。与全站「外壳不滚、内容区各自内滚」模型一致。现状佐证：git_error_dialog 日志区固定 248px 高内滚（git_error_dialog.cpp:29）。 |
| ② 有无大段无效空白 | 无。三段式高度全部指定用途：头区 69px（标题+分隔线）、底区 73px（细线+按钮行），剩余全部归内容区且内滚兜底——弹窗拉高只增高内容区，头/底区不随高度拉伸（禁「弹性区内容稀疏」）。 |
| ③ 密度与权重 | 高频信息=内容区正文（14px），视觉权重最高；标题 20px/700 是面板内唯一大字；按钮区是唯一彩色块（主按钮绿填充），同视口唯一主按钮。确认型弹窗正文 ≤3 行，表单/内容型正文区滚动，不追求一屏塞满。最小窗（1200×720）与宽窗两档下三段比例都成立：1200×720 视口下最大档 760×720-96=624 高，内容区仍有 624-158=466px。 |

### 1.2 三段式构图（ASCII 布局图）

```
┌────────────────────────────────────────────────────────┐
│  遮罩 backdrop（黑 alpha：暗 0.46 / 亮 0.28，吞滚轮，      │
│  点击遮罩 = 取消路径关闭）                                 │
│                                                        │
│   ┌────────────────────────────────────────────┐       │
│   │ ▓ 头区（固定 69px）                          │       │
│   │   标题 20px/700        ← 上缘距顶 16          │       │
│   │   ──────────────────── 分隔线 1px（y=52）    │       │
│   │   （线下隙 16 → 内容区顶 y=69）              │       │
│   ├────────────────────────────────────────────┤       │
│   │ ▓ 内容区（弹性，唯一内滚区）                  │       │
│   │   左右边距 24（宽 = w-48），内容底至细线隙 16  │       │
│   │   空/加载/错误/正文 四态，见 §6               │       │
│   ├────────────────────────────────────────────┤       │
│   │ ▓ 底区（固定 73px，自底向上）                 │       │
│   │   ──────────────────── 细线 1px（y=h-73）   │       │
│   │                [取消(次)] [确认(主)] ←右锚   │ y=h-56│
│   │                          右端 x=w-24，gap 8 │       │
│   │                          按钮行底距 16       │       │
│   └────────────────────────────────────────────┘       │
│   面板：surface 底 + 1px 边 + 圆角 16 + 阴影              │
└────────────────────────────────────────────────────────┘
```

### 1.3 区块锚定关系

- 头区/底区**上下锚定**弹窗面板（fixed），内容区占剩余全部高度（flex 弹性）。
- 按钮行**右锚**（右端 x = w-24），不随内容区宽度变化；副按钮居左、主按钮居右（取消在左、确认最靠右下——基线「弹窗右下取消（次）+确认（主）」）。
- 分隔线/细线**横贯面板全宽**（x=0 至 x=w，不通到边内侧缩进——结构线是面板级锚，不是内容级装饰）。

### 1.4 空间节奏与信息密度预算

| 区块 | 高度 | 内容密度 |
|---|---|---|
| 头区 | 固定 69px | 标题 1 行（20/700，行高 28），不加副标题、不加右上角关闭钮（关闭走取消按钮与遮罩/Esc） |
| 内容区 | = h − 158，弹性 | 正文 14px 行高 28；表单控件行高 40；长内容内滚；密度上限=一屏可读，禁堆砌 |
| 底区 | 固定 73px | 按钮行 1 行（高 40），最多 2 颗按钮（主+副），禁 3 颗以上并排 |

---

## 2. 现存弹窗盘点（规范覆盖清单，grep 实证 2026-11-07）

### 2.1 覆盖清单

| 模块 | 弹窗（id / 锚点） | 现状实现 | 现状问题 |
|---|---|---|---|
| 智能体 agent | agent.dialog.task（agent_module_dialogs.cpp:518）、agent.dialog.tools（:1352）、agent.dialog.detail（agent_detail_dialog.cpp:143） | 自绘 components::dialog | P1 P2 P4 P6 |
| 智能体 agent | 详情/编辑表单（agent_module_dialogs.cpp ×3 处共享件调用） | 共享件 ui_kit/dialog_form | P1 P2 |
| 记忆 memory | 提案弹窗等 ×4（memory_module_modals.cpp） | 共享件 | P1 P2 |
| MCP | mcp.dialog.detail 执行详情（mcp_page_module.cpp:666） | 自绘 | P1 P2 P4 P6 |
| 上线发布 release | release.newtask.dialog（release_dialogs.cpp:343）、release.newtask.confirm（:545）、release.detail.validation（release_detail_draw.cpp:1839）、release.detail.config.editor（:1940）、release.detail.confirm（:2094） | 自绘 | P1 P2 P4（标题曾另有两套 19/600、20/600，S10 已统一 20） |
| 上线发布 release | 配置弹窗 ×6（release_config_ui.cpp） | 共享件 | P1 P2 |
| 设置 settings | settings_module.cpp ×3 | 共享件 | P1 P2 |
| 设置 settings | settings.github.err Git 错误详情（git_error_dialog.cpp，自绘行布局 + 共享按钮口径） | 混合 | P1 P2 P4（按钮 gap 12 vs 共享 8） |
| 代理 claude_proxy | claude_ui_form.cpp ×4、claude_ui_list.cpp ×2（删除确认 Compact 档、Profile 表单 Form 档） | 共享件 | P1 P2 |
| 笔记 markdown | md_module_menu_poll.cpp ×12 | 共享件 | P1 P2 |
| 日程 schedule | schedule.history.drawer（schedule_ui_history.cpp:461）、schedule.edit.dialog（schedule_ui_edit.cpp:471）、schedule.list.deleteConfirm（schedule_ui_list.cpp:1266） | 自绘 | P1 P2 P4 P6 |
| 待办 todo | todo.dialog.newtask（todo_ui_dialogs.cpp:210）、todo.dialog.detail（:860）自绘 + ×4 处共享件 | 混合 | P1 P2 P3（显式 zIndex popups.zOf） |
| 壁纸 wallpaper | wallpaper_ui.cpp ×2 | 共享件 | P1 P2 |
| platform | message_box（message_box_macos/win/posix.cpp） | 原生系统弹窗 | **域外**：平台原生样式，不纳入本规范 |

### 2.2 共性现状问题（P1–P6）

| # | 问题 | 证据 |
|---|---|---|
| P1 | 按钮排布两套：共享件=左锚各半宽 (w−68)/2×40；框架默认 actions 行=右锚 96–150px | dialog_form.cpp:141-150 / EUI-NEO dialog.h:165-168 |
| P2 | 全站 0 处标题分隔线、0 处按钮区细线 | grep 无 line/rule 元素于弹窗内 |
| P3 | zIndex 双轨：默认 1000 vs todo 显式 popups.zOf | dialog_form.h:23 / todo_ui_dialogs.cpp:215 |
| P4 | 按钮几何漂移：gap 8/12 双值、行底距 20（h−60）vs 框架 h−58、按钮字号 15 vs 框架 body | dialog_form.cpp:65-66 / git_error_dialog.cpp:38 / dialog_form.h:96 |
| P5 | 注释与实值漂移：metrics.h:42 注释「一律自 x=28 起」vs kDialogInsetX=24 | metrics.h:42-44 |
| P6 | 无最小尺寸下限：框架仅做 max clamp（≤视口−96），无 min 兜底 | EUI-NEO dialog.h:81-82 |

---

## 3. 四段式结构规范（老板规格逐条 → 参数全落值）

> 间距一律 theme spacing 选档 4/8/16/24/40（src/core/theme.cpp:52-56）；色值一律 token 实值（§5 对照表）；所有数值可对照验收。

### 3.1 顶部：标题区（规格①）

| 参数 | 值 | token/出处 |
|---|---|---|
| 标题字号 / 字重 / 行高 | 20px / 700 / 28px 行高 | kDialogTitleFontSize=20（dialog_form.h:64）；行高 28（dialog_form.h:71 现状） |
| 标题颜色 | textPrimary | theme.cpp:30/91 |
| 标题上缘距弹窗顶 | 16px | dialog_form.h:69（现状 ≤16 口径） |
| 标题左缘 x | 24px | kDialogInsetX=24（metrics.h:43） |
| 标题→分隔线隙 | 8px | spacing 档 |
| **分隔线** | 1px，色 borderDefault，横贯面板全宽（x=0→w） | theme.cpp:34/95 |
| **线下空隙（少量）** | 16px → 内容区顶 y=69 | spacing 档 |
| 头区总高 | 69px（=16+28+8+1+16） | 推导值，固定不伸缩 |

### 3.2 中间：内容区（规格③）

| 参数 | 值 | token/出处 |
|---|---|---|
| 左右边距 | 24px（内容宽 = w−48） | kDialogInsetX=24 / kDialogBodyInsetX=48（metrics.h:43-44 现状实值） |
| 顶 | y=69（头区下缘） | §3.1 推导 |
| 底 | 至细线上隙 16px（内容区底 = h−89） | spacing 档 |
| 滚动 | 唯一内滚区；外壳（遮罩/面板/头/底）不滚 | §1.1 构图三问① |
| 正文字号 / 行高 | 14px / 28px 行高 | fontSizeBody（theme.cpp:63/125） |
| 说明/辅助字号 | 12px | fontSizeSmall |
| 内容底至细线隙 | 16px | spacing 档 |

### 3.3 底部：按钮区（规格②）

| 参数 | 值 | token/出处 |
|---|---|---|
| **按钮排布** | 统一靠右：右端 x = w−24；副按钮在左、主按钮最右 | 基线「弹窗右下取消+确认」+ 框架默认右锚（dialog.h:165） |
| **按钮区上方细线** | 1px，色 borderMuted（比标题分隔线轻一档），横贯面板全宽 | theme.cpp:35/96 |
| 按钮行位置 | y = h−56（高 40，行底距弹窗底 16） | spacing 档（现 h−60/底距 20 归一为 16） |
| 细线位置 | y = h−73（=按钮行上隙 16 + 线 1px；线→按钮行隙 16） | spacing 档 |
| 按钮高度 | 40px（基线 32-40 档上限，弹窗主操作取大档） | 基线组件家族 |
| 按钮最小宽度 | 96px；文字超出则自适应加宽（内边距 24×2），主/副各自 ≥96 | 框架默认下限（dialog.h:87） |
| 按钮间距 | 8px | spacing 档（现 8/12 双值归一为 8） |
| 按钮字号 | 14px | 基线「字 14」（现共享件 15 归一为 14） |
| 按钮圆角 | 10px | s.radius（theme.cpp:57/119，与全站按钮同源） |
| 主按钮（应用/确认） | 主色填充：底 primary、字 onPrimary、hover primaryHover、按下 primaryPressed | theme.cpp:23-25/84-86 |
| 副按钮（关闭/取消） | 次级：底 surfaceElevated、字 textPrimary、1px 边 borderDefault、hover 底 hover、按下底 active | dialog_form.cpp:84-89 现状组合 |
| 主按钮危险变体（删除确认类） | 底 error、字 onPrimary；hover/按下同色加深禁用（error 单值，无 hover 档） | dialog_form.cpp:107-111 现状 |
| 主按钮禁用态 | 底 primarySoftBg、字 textDisabled、无 hover/按压/果冻 | theme.cpp:26/87 + textDisabled |
| 副按钮禁用态 | 字 textDisabled、底/边维持、无 hover/按压 | textDisabled token |
| 同视口约束 | 主按钮（绿填充）同视口唯一；危险变体亦同视口唯一 | 基线四原则 |
| 键盘焦点 | focus-visible：2px primary 外环，偏移 2px | 基线「键盘焦点可见」 |

### 3.4 弹窗最小尺寸（规格④）

| 参数 | 值 | 说明 |
|---|---|---|
| **最小尺寸下限** | **420 × 240** | 420=宽五档下限（DialogWidth::Compact）；240=确认型可用下限：内容区净高 240−158=82px（≥2 行正文 28×2=56 + 余量）。下限防内容挤扁（P6 兜底）。 |
| 宽五档（不变） | 420 / 460 / 640 / 720 / 760 | DialogWidth Compact/Standard/Wide/ExtraWide/Form（metrics.h:18-40） |
| 视口 clamp | 弹窗宽高 ≤ 视口 − 96（上下左右边距各 48） | spacing.overlay=48（EUI-NEO theme.h:66，dialog.h:81-84 现行为） |
| clamp 与下限冲突 | 视口极窄（<516）时以视口 clamp 为先（弹窗收缩至视口−96，可低于 420） | 溢出比挤扁更不可接受；框架行为保留 |
| 内容兜底 | 内容区净高 < 82px 时仍内滚，头/底区不收缩 | §1.1 构图 |

---

## 4. 参数总表

### 4.1 间距选档（唯一允许值）

4 / 8 / 16 / 24 / 40（theme spacingSmall/spacing/spacingLarge/spacingXLarge/spacingXXLarge，theme.cpp:52-56）。本规范用到：8（标题→线、按钮 gap）、16（线下隙、细线→按钮、按钮底距、内容底隙）、24（侧边距/内容 inset）。

### 4.2 线规格

| 线 | 粗细 | 色（暗） | 色（亮） | 位置 |
|---|---|---|---|---|
| 标题分隔线 | 1px | borderDefault #3A3735 | #E5E7EB | y=52，全宽 |
| 按钮区细线 | 1px | borderMuted #2A2826 | #F3F4F6 | y=h−73，全宽 |

### 4.3 按钮规格汇总

| 属性 | 主按钮 | 副按钮 | 危险主按钮 | 禁用（主） |
|---|---|---|---|---|
| 高 | 40 | 40 | 40 | 40 |
| 最小宽 | 96 | 96 | 96 | 96 |
| 字号 | 14 | 14 | 14 | 14 |
| 圆角 | 10 | 10 | 10 | 10 |
| 底 | primary | surfaceElevated | error | primarySoftBg |
| hover 底 | primaryHover | hover | error | 无 |
| 按下底 | primaryPressed | active | error | 无 |
| 字色 | onPrimary | textPrimary | onPrimary | textDisabled |
| 边框 | 无 | 1px borderDefault | 无 | 无 |

### 4.4 层级与遮罩

| 参数 | 值 | 出处 |
|---|---|---|
| 面板 zIndex | 默认 1000；叠加弹层按打开顺序递增，步进 10（todo popups.zOf 动态值归一目标） | dialog.h:239 / todo_ui_dialogs.cpp:215 |
| 遮罩色 | 黑 alpha：暗 0.46 / 亮 0.28 | dialog.h:19 |
| 遮罩交互 | 吞滚轮；点击遮罩 = 取消路径关闭 | dialog.h:108 现行为 |
| 面板容器 | surface 底 + 1px 边（borderDefault 82% alpha，框架推导档）+ 圆角 16 + 阴影（blur 24 / offsetY 8 / alpha 暗 0.28 / 亮 0.12，黑） | dialog.h:18-31,123-129 / theme.h:299-301 |
| 面板圆角 | 16px（radius.section=radiusOverlay 同值档） | EUI-NEO theme.h:61 / core theme.cpp:60 |
| Esc 键 | = 取消路径关闭 | 交互统一 |

---

## 5. 亮暗两套 token 对照表（弹窗相关子集，src/core/theme.cpp 实值）

| token | 暗（theme.cpp:14-49） | 亮（theme.cpp:74-111） | 用途 |
|---|---|---|---|
| background | #1A1816 | #F5FAF7 | 遮罩下衬参考（demo 画布） |
| surface | #232120 | #FFFFFF | 面板底 |
| surfaceElevated | #2C2A28 | #FFFFFF | 副按钮底 |
| hover | #353231 | #EEF6F1 | 副按钮 hover |
| active | #3E3B39 | #E2F0E8 | 副按钮按下 |
| primary | #36D399 | #07C160 | 主按钮底 |
| primaryHover | #2EC48F | #06AD56 | 主按钮 hover |
| primaryPressed | #23B07F | #059048 | 主按钮按下 |
| primarySoftBg | #0D3326 | #E8F8EE | 主按钮禁用底 |
| onPrimary | #FFFFFF | #FFFFFF | 主按钮字 |
| textPrimary | #FAF9F5 | #1F2937 | 标题/正文字 |
| textSecondary | #C9C5BD | #4B5563 | 说明字 |
| textTertiary | #9A948A | #6B7280 | 辅助/空态字 |
| textDisabled | #908A82 | #6B7280 | 禁用字 |
| borderDefault | #3A3735 | #E5E7EB | 面板边、标题分隔线、副按钮边 |
| borderMuted | #2A2826 | #F3F4F6 | 按钮区细线 |
| error | #F87171 | #EF4444 | 危险主按钮底 |
| errorText | #F87171 | #B91C1C | 校验错误行字 |
| radius（按钮） | 10 | 10 | — |
| radiusOverlay（面板） | 16 | 16 | — |

## 6. 状态表

| 状态 | 表现（参数） |
|---|---|
| 空态（内容区） | 「暂无内容」12px textTertiary 居中；就近操作用文字按钮（绿字或灰字，无底） |
| 加载态 | 「正在加载…」12px textTertiary 居中（轻提示文案，不做骨架屏） |
| 错误态（表单校验） | 错误行 12px errorText 就近字段下方（空串不建节点）；主按钮置灰（§4.3 禁用态）直至必填齐 |
| 错误态（全局，如 Git 错误详情） | 内容区红字区块 12px errorText，日志区固定高内滚 |
| 成功态 | 无停留成功态——成功即关窗 + toast 1.5s 自动消失（框架 toast 组件） |
| 弹窗开 | 遮罩+面板 160ms 淡入上浮（§7） |
| 弹窗关 | 160ms 淡出，关闭路径三选一：主按钮（提交）/ 副按钮（取消）/ 遮罩或 Esc（=取消） |
| 按钮 hover / 按下 | §4.3；按下附加果冻缩放 §7 |

## 7. 动效表

| 动效 | 时长 | 缓动 | 属性 |
|---|---|---|---|
| 弹窗开/合（面板+遮罩） | 160ms | OutCubic | opacity 0↔1 + scale 0.965↔1（origin 0.5/0.5）+ translateY 14↔0 |
| 按钮 hover 底色 | 120ms | OutCubic | background |
| 按钮按压（果冻） | 160ms | OutCubic | scale → 0.96，松手平滑回弹（基线果冻档 0.96-0.97） |
| 遮罩淡入淡出 | 160ms | OutCubic | opacity |
| 减弱动态效果 | 系统开启时：转场退化为直切（0ms），按压果冻关闭 | — | 框架/系统约定 |

## 8. 自适应行为表

| 条件 | 行为 |
|---|---|
| 视口宽 ≥ 856 | 五档按原宽（最大档 760 + 边距 96 = 856） |
| 视口宽 < 856 | 弹窗宽 = min(档宽, 视口−96)（框架 clamp 现行为） |
| 视口高不足 | 高 = min(设计高, 视口−96)；内容区收缩并内滚，头/底区固定 69/73px 不缩 |
| 最小可用窗口 1200×720（三栏档） | 五档全容纳：760+96=856 < 1200 ✓；760 宽弹窗内容区高最高 624−158=466px |
| 视口 < 516 宽 | clamp 优先于最小宽下限（§3.4） |
| 长内容 | 内容区内滚（唯一滚动区），禁弹窗整体滚、禁分页器 |

## 9. 交互流程

1. 触发 → 弹窗开（160ms 上浮），遮罩出现，背景交互被遮罩阻断。
2. 表单型：必填未齐 → 主按钮禁用态 + 就近红字；填写齐 → 主按钮激活。提交 → 校验 → 成功关窗 + toast / 失败红字留在弹窗。
3. 确认型：正文 ≤3 行；主按钮为确认（删除类用危险变体）；取消/遮罩/Esc 均为放弃路径。
4. 内容型：内容区内滚浏览；关闭只走副按钮/遮罩/Esc。
5. 关闭：160ms 淡出后焦点回到触发控件。

## 10. 对现状的统一变更清单（盘点问题 → 规范裁决）

| P# | 现状 | 规范裁决 |
|---|---|---|
| P1 | 按钮左锚半宽 vs 框架右锚 | 统一**右锚**（老板规格②），min 宽 96 |
| P2 | 无双线 | 新增标题分隔线（§3.1）+ 按钮区细线（§3.3） |
| P3 | zIndex 双轨 | 统一默认 1000、叠加步进 10；todo popups.zOf 归一 |
| P4 | gap 8/12、底距 20/h−58、字号 15 | 统一 gap 8、底距 16（y=h−56）、字号 14 |
| P5 | metrics.h 注释 x=28 vs 实值 24 | 以实值 24 为准（注释遗留属实现侧清理项，不阻塞） |
| P6 | 无最小尺寸 | 新增下限 420×240（§3.4） |

## 11. 先验验证清单（功能×场景×边界格子矩阵，供开发前钉住）

| 格子 | 弹窗类型 | 交互（demo 阶段钉住） | 边界 | 平台差异 |
|---|---|---|---|---|
| G1 | 表单型=智能体新建（760 档） | 必填空→主按钮禁用+红字；填齐→激活；Esc/遮罩=取消 | 最小 420×240 也能容纳本规范头/底区；长提示词内滚 | 无（弹窗层平台无关；原生 message_box 域外） |
| G2 | 确认型=删除确认（420×240=最小档边界演示） | 危险主按钮按压果冻；遮罩点击=取消；双按钮右锚 | **正好落在最小尺寸下限**：内容区净高 82px ≥2 行正文；标题分隔线+细线在 240 高下不重叠 | 无 |
| G3 | 内容型=MCP 执行（640 档） | 内容区滚到底/滚轮不透传遮罩；无主按钮时仅副按钮右锚 | 200+ 行日志内滚流畅；内容区净高不足时仍可滚 | 无 |
| G4 | 全局 | 亮暗切换全 token 无硬编码；focus-visible 可见；减弱动态效果退化直切 | 视口 <856 弹窗收缩；<516 极窄 clamp 生效可低于 420；1200×720 最小窗五档不破版 | 无 |
| G5 | 叠加弹层 | 弹窗上开弹层（如日期选择）zIndex 步进 +10 | 关闭上层回落焦点 | 无 |

## 12. Demo

`docs/design/2026-11-07-dialog-standard-demo.html`——自包含单文件（样式内联、无框架、无外部依赖、浏览器直开），覆盖：亮暗切换 / 三个代表弹窗（G1 表单、G2 确认=最小尺寸边界、G3 内容）/ 果冻按压 / focus-visible / 窗口拖窄收缩。**demo 仅视觉校准，不是实现蓝图**；实现按本文档参数由 developer 落地。

## 13. 禁止项自查

- 四色语义：绿=操作（主按钮）、红=仅告警（危险变体/错误行）、蓝=未使用（链接域）、灰=中性（副按钮/线/字）✓ 无越界
- 禁大面积渐变/装饰色块：无 ✓；「大气美观」类不可执行描述：无，全部可对照数值 ✓
- 设计绑定实现技术：正文无框架/库名（EUI-NEO 组件名仅作现状盘点与实值出处引用，非实现指令）✓
- 局部参数精致、整体构图空泛：§1 构图节先行，三问已答 ✓
- AI 五 tell：无暖米衬线/暗夜霓虹/报纸排版/SaaS 卡片/模板镀铬；大胆单点=双线三段结构本身（老板规格），其余全取 token 安静档 ✓
- 最小窗口 1200×720 不破版 ✓；demo 双击可开、零外部依赖 ✓
