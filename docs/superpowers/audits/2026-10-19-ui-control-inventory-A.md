> **存档说明**（非原文内容）：来源：只读盘点子代理 2026-10-19 产出，原文照录。作为 C++ 开发阶段"先看原本UI有啥"的参考 + demo 缺口分析依据（不倒灌进文档当镜像目标，按用户 2026-10-19 澄清"文档与代码无绝对映射"）。

---

# DevDesk UI 控件全量盘点 A（home / markdown / about）

仓库：`D:\project\gui\DevDesk\src`。弹窗触发点引 `2026-10-19-dialog-inventory.md` 编号。

## 一、module_home（首页）

源：`module_home\home_module.cpp`、`home_quota_card.cpp`。零弹窗/零右键/零 toast。

| # | 类型 | 用途/文案 | 区域 | file:line | id |
|---|---|---|---|---|---|
| H1 | 滚动区 | 整页外层 scrollView | 全页 | home_module.cpp:318 | home.scroll |
| H2 | 文本 | "欢迎回来～" | 页头 | :341 | home.h1 |
| H3 | 文本 | "今天是 X" | 页头 | :349 | home.date |
| H4 | 次按钮+图标 | "刷新" 0xF021 | 页头 | :365 | home.btn.refresh |
| H5 | 次按钮+图标 | "设置" 0xF013 → 跳设置页 | 页头 | :379 | home.btn.settings |
| H6 | 文本(空态) | "所有工具模块已关闭…" | 内容区 | :429 | home.stats.alloff |
| S1 | 统计卡 | "Token 速率 (tok/s)" 含 sparkline lineChart + onTimer | 统计行 | :467 / drawStatCard:56 | home.stat.tokens (+.press/.label/.number/.badge/.timer/.sparkwrap) |
| S2 | 统计卡 | "今日笔记" | 统计行 | :512 | home.stat.notes |
| S3 | 统计卡 | "待办任务" | 统计行 | :523 | home.stat.todo |
| S4 | 统计卡(危险态) | "逾期任务" accent=red when >0 | 统计行 | :533 | home.stat.overdue |
| S5 | 统计卡 | "API 调用" | 统计行 | :543 | home.stat.api |
| Q0 | 卡片 | 供应商余量卡壳 | 余量卡 | home_quota_card.cpp:317 | home.quota |
| Q1 | 文本 | "供应商余量" | 余量卡-卡头 | :333 | home.quota.title |
| Q2 | 次按钮+图标 | "刷新" 0xF021 | 余量卡-卡头 | :344 | home.quota.refresh |
| Q3 | 文本(空态) | "暂无 Claude Profile" / "余量加载失败：…" | 余量卡 | :362 | home.quota.empty |
| Q4 | 滚动区 | 行内容 scrollView | 余量卡 | :376 | home.quota.scroll |
| Q5 | 文本 | profile 名（ellipsis） | 行头 | :418 | home.quota.row.<pid>.name |
| Q6 | 文本 | provider 名（ellipsis） | 行头 | :430 | home.quota.row.<pid>.provider |
| Q7 | 标签/badge | "可能过期" warning 色 | 行头 | :442 | home.quota.row.<pid>.stale |
| Q8 | 文本 | 状态文案（Unconfigured/Pending 等） | 行头 | :454 | home.quota.row.<pid>.status |
| Q9 | 次按钮 | "去配置" → 跳 Claude 页 action:quota_config | 行头 | :472 | home.quota.row.<pid>.config |
| Q10 | 文本 | "余额 X.XX CNY" | 行-余额 | :505 | home.quota.row.<pid>.balance.amount |
| Q11 | 文本 | plan 名 | 行-余额 | :519 | home.quota.row.<pid>.balance.plan |
| Q12 | 文本 | 窗口名（5min/小时/天 等） | 行-窗口 | :265 | home.quota.row.<pid>.bar.<key>.label |
| Q13 | 进度条 | components::progress 用量% | 行-窗口 | :277 | home.quota.row.<pid>.bar.<key> |
| Q14 | 文本 | "62%" / "62.4%" 三档色 | 行-窗口 | :283 | home.quota.row.<pid>.bar.<key>.pct |
| Q15 | 文本 | 重置倒计时（"X 天后重置" 等） | 行-窗口 | :293 | home.quota.row.<pid>.bar.<key>.reset |
| Q16 | 文本 | 错误文案（error 色） | 行-错误 | :549 | home.quota.row.<pid>.error |
| C1 | 卡片 | 图表卡壳 | 图表 | home_module.cpp:617 | home.api.chart |
| C2 | 文本 | "今日 Token 用量分布（每小时输出 token 合计）" | 图表-标题 | :622 | home.api.chart.title |
| C3 | 文本(空态) | "今日暂无 API 调用" | 图表 | :639 | home.api.chart.empty |
| C4 | 矩形 ×4 | 网格线（低透明 border 色） | 图表-柱区 | :675 | home.api.chart.bars.grid.0..3 |
| C5 | 矩形 ×N | 自绘柱体（无 components::barChart） | 图表-柱区 | :705 | home.api.chart.bars.bar.<i> |
| C6 | 文本 ×N | 柱顶 token 数值标签 | 图表-柱区 | :719 | home.api.chart.bars.val.<i> |
| C7 | 文本 ×N | x 轴小时标签 "N时" | 图表-柱区 | :733 | home.api.chart.bars.label.<i> |
| A1 | 主按钮 | "+ 新建笔记" primary → action:new_note | 快捷按钮行 | :775 | home.act.newnote |
| A2 | 次按钮 | "+ 新建任务" → action:new_task | 同上 | :775 | home.act.newtodo |
| A3 | 次按钮+图标 | "切换 Claude Profile" 0xF46D | 同上 | :775 | home.act.claude |

## 二、module_markdown（Markdown 笔记本）

源：`module_markdown\markdown_module.cpp`（3783 行）。

| # | 类型 | 用途/文案 | 区域 | file:line | id |
|---|---|---|---|---|---|
| M0 | 容器 | 根 column | 全页 | :332 | md.root |
| M1 | 文本 | "Markdown 笔记本" | 页头 | :395 | md.h1 |
| M2 | 文本 | 副标题（键盘提示/"尚未初始化…"）+ clip 栈 | 页头 | :405/:409 | md.subtitle.clip / md.subtitle |
| M3 | Tab(segmented) | 4 项 "编辑/实时/双栏/预览" | 页头 | :423 | md.mode |
| M4 | 开关(switch) | "大纲" toggle | 页头-动作行 | :440 | md.outline.toggle |
| M5 | 主按钮 | "+ 新建笔记" → 弹窗 #12 | 页头-动作行 | :449 | md.btn.new |
| T1 | 矩形 | 树栏底色 | 侧栏-目录树 | :502 | md.tree.bg |
| T2 | 搜索框(input) | placeholder "搜索笔记…" FTS5 | 侧栏 | :509 | md.search |
| T3 | 图标文本 | fa-magnifying-glass 0xF002 | 侧栏 | :522 | md.search.icon |
| T4 | 文本 | "标签" 小标题 | 侧栏 | :534 | md.tags.title |
| T5 | 标签/chip ×N | 筛选标签 "name·N"（pill, 点击 toggle） | 侧栏 | :560 | md.tags.filter.<i>(+.bg/.name) |
| T6 | 滚动区 | 树/搜索结果 scrollView | 侧栏 | :686/:728 | md.tree.scroll |
| T7 | 文本(错误) | "列表加载失败：…" | 侧栏 | :597 | md.tree.error |
| T8 | 文本(空态) | "还没有笔记…" / "没有匹配的笔记" | 侧栏 | :612/:623 | md.tree.empty |
| T9 | 列表行-文件夹 | folder 行体 + 收起/展开 toggle + 箭头 ▾/▸ + 文件夹 icon 0xF07B + 名 | 侧栏-树 | :747 | md.tree.node.<i>(+.row/.toggle/.arrow/.ficon/.name) |
| T10 | 列表行-文件 | row rect + icon 0xF15C + 名（onContextMenu→#2） | 侧栏-树/搜索 | drawFileTreeItem:77 | md.tree.item.<i> 或 .node.<i> |
| T11 | 次按钮 | "+ 笔记" → 弹窗 #12 | 侧栏-底 | :641 | md.tree.new |
| T12 | 次按钮 | "+ 文件夹" → 弹窗 #13 | 侧栏-底 | :652 | md.tree.newfolder |
| E1 | 多行 input | 编辑器主输入（IME/scrollbar/lineStyles 高亮/onKeyEvent Tab/Enter/Ctrl+B/I/K） | 内容-编辑栏 | :998 | md.editor.input |
| E2 | 文本(空态) | "从左栏选一篇笔记开始编辑" | 编辑栏 | :963 | md.editor.hint |
| E3 | 标签 chip ×N | 当前笔记标签（pill + × 删除热区） | 编辑栏-标签栏 | :1156 | md.tags.chip.<i>(+.bg/.name/.del/.x) |
| E4 | 单行 input | placeholder "＋ 标签" onEnter 提交 | 编辑栏-标签栏 | :1203 | md.tags.add |
| E5 | 主按钮(pill) | "＋" 加标签 | 编辑栏-标签栏 | :1215 | md.tags.addbtn |
| E6 | 矩形 | 状态栏底 | 编辑栏-状态栏 | :1230 | md.status.bg |
| E7 | 文本 | 文件名 + ●未保存/✓已保存 HH:MM/✕保存失败 | 状态栏 | :1254 | md.status.save |
| E8 | 文本 | "行:列 · N 字 · N 篇笔记" | 状态栏 | :1304 | md.status.pos |
| P1 | 矩形 | 预览栏底（含渐变） | 内容-预览栏 | :1327 | md.preview.bg |
| P2 | 文本(空态) | "选择笔记后在这里看到渲染结果" | 预览栏 | :1347 | md.preview.hint |
| P3 | 滚动区 | 预览 scrollView（双栏同步滚动） | 预览栏 | :1370 | md.preview.scroll |
| P4 | 块流 column | 逐块渲染容器 | 预览栏 | :1445 | md.preview.blocks |
| P5 | 块包裹 stack ×N | hover 底衬 + 内容 + mouseArea hit 覆盖层 | 预览栏 | :1463 | md.preview.blk.<stableId>(+.hl/.hit) |
| P6 | CodeFence 自绘 | bg rect + 代码 text（带 runs 高亮） | 预览栏-代码块 | :1811 | <wrapId>.code(+.bg/.text) |
| P7 | markdown 组件 | 默认块渲染（非 CodeFence） | 预览栏 | :1508 | md.preview.block.<stableId> |
| L1 | 滚动区 | 实时态 scrollView（单一滚动权威） | 内容-实时栏 | :1722 | md.live.scroll |
| L2 | 多行 input | 活动块单输入（全局唯一，切块搬位置） | 实时栏 | :1916 | md.live.input |
| L3 | 矩形 | 活动块半透明主色边框 | 实时栏 | :1965 | <wrapId>.frame |
| O1 | 浮层面板 | 大纲面板 zIndex=10 | 浮层-大纲 | :836 | md.outline.panel |
| O2 | 矩形 | 面板底 | 大纲 | :841 | md.outline.panel.bg |
| O3 | 文本 | "大纲" 标题 | 大纲 | :847 | md.outline.panel.title |
| O4 | 次按钮 | "✕" 关闭大纲 | 大纲 | :856 | md.outline.panel.close |
| O5 | 文本(空态) | "先打开一篇笔记" / "没有标题…" | 大纲 | :868 | md.outline.panel.empty |
| O6 | 滚动区 | 大纲列表 scrollView | 大纲 | :890 | md.outline.scroll |
| O7 | 列表行 ×N | 标题项 row + 缩进 label（点击跳转） | 大纲 | :906 | md.outline.item.<i>(+.row/.label) |
| D1-D4 | 弹窗 #12/#13/#14/#15 | 新建笔记/新建文件夹/删除/重命名 | 弹窗 | :2316/:2384/:3426/:3465 | md.dialog.* |
| D2-dd | 下拉(单选) | 父目录选择（弹窗 #13 内） | 弹窗 | :2430 | md.dialog.newfolder.parent.dd |
| CM1-CM2 | 右键菜单 #2/#3 | 文件行/编辑器插入表格级联 | 右键 | :3352/:3396 | md.ctx / md.editor.ctx |
| TO1-TO2 | Toast | 普通提示 / "文件已被外部修改"action toast | 浮层 | :2501/:2504 | md.toast |

## 三、module_about（关于页）

源：`module_about\about_module.cpp`。零弹窗/零右键/零 toast/零 input。`about_anim.h` 纯状态机无 UI 元素。

| # | 类型 | 用途/文案 | 区域 | file:line | id |
|---|---|---|---|---|---|
| B0 | 矩形 | 透明背景层（onMove 唤醒 / onTimer 30fps） | 背景 | :193 | about.bg |
| B1 | 矩形 ×6×3=18 | 光斑同心圆叠层（含 shadow 霓虹边）= 极光波 | 背景 | :221 | about.glow.<i>.<k> |
| B2 | 矩形 ×2 | 聚光扫过带（gradient+rotate+shadow）= 极光波扫带 | 背景 | :257 | about.sweep.0/1 |
| H1 | 图标文本 | "fa-laptop-code" 0xF5FC hero 图标 | 内容-顶部 | :290 | about.hero.icon |
| H2 | 文本 | "DevDesk" | 内容-顶部 | :298 | about.hero.title |
| C1-icon | 图标文本 | "fa-circle-info" 0xF05A | 卡片1-头 | :100 | about.card.app.icon |
| C1-title | 文本 | "应用信息" | 卡片1-头 | :107 | about.card.app.title |
| C1-row ×6 | 文本对 | label 左 + value 右（版本/Git commit/构建时间/编译器/平台/C++ 标准） | 卡片1 | :119/:127 | about.card.app.label.0..5 / .value.0..5 |
| C2-icon | 图标文本 | "fa-layer-group" 0xF5FD | 卡片2-头 | :100 | about.card.deps.icon |
| C2-title | 文本 | "依赖与框架" | 卡片2-头 | :107 | about.card.deps.title |
| C2-row ×6 | 文本对 | GUI 框架/EUI-NEO/数据库/HTTP/TLS/字体 | 卡片2 | :119/:127 | about.card.deps.label.0..5 / .value.0..5 |
| F1 | 文本 | "© 2026 DevDesk" | 页脚 | :340 | about.footer |

## 四、demo 缺口（盘点 vs demo，供后续 demo 补全用）

- **home 供应商余量卡（Q0-Q16）**：整块富控件簇（行头 profile/provider/stale badge/status/去配置 btn + 余额/plan + 窗口进度条/pct/reset + error）——demo 可能简化或漏。需补全。
- **home 统计卡 sparkline（S1）+ 危险态（S4 红色 accent）**：demo 可能缺。
- **home 图表自绘柱（C4-C7：网格/柱体/柱顶数值/x轴）**：demo 可能简化。
- **markdown 大纲面板（O1-O7）**：浮层面板 + 列表，demo 可能漏。
- **markdown 标签筛选 chips（T5）+ 实时态（L1-L3）+ CodeFence 自绘（P6）**：demo 可能简化/漏。
- **about 极光波（B0-B2 = 18 光斑 + 2 扫带）**：与 splash 同源；v1.0 §3.7.1 已计划移除（与 splash 反转 D-11 一致）。

## 五、覆盖度声明
已实读：home_module.cpp(880) / home_quota_card.cpp(572) / home_quota_card.h / home_service.h / markdown_module.cpp(3783 全文) / md_ids.h / about_module.cpp(357) / about_anim.h(189)。grep + 交叉核对 dialog-inventory §4（home/about 零弹窗/菜单/toast）一致。边界/可能漏：① 侧边栏 app_sidebar.cpp 与启动 splash 不属这三页（全局 chrome / 启动覆盖层）；② ui_kit 公共组件内层未展开；③ components 内部分层按黑盒记；④ about openDirectoryButton 标 [[maybe_unused]] 死代码不渲染；⑤ home 余量卡/图表按双重门控条件渲染分支已穷尽。
