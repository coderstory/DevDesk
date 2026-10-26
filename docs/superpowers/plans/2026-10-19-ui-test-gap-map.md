# UI 面测试覆盖缺口地图（只读盘点）

> 日期：2026-10-19 ｜ 性质：只读盘点，零构建、零代码改动，仅本文件一个 commit
> 基线：dev `d954cab`（worktree agent-a2e969b431b49b868 快进后）
> 三源：① `ctest -L probe`（tests/CMakeLists.txt:233-334 GLOB 注册，现 6 个 probe 源 = 8 条 ctest，含 home/screenshot 各 1 变体）② 无头几何/compose 断言单测（tests/test_modules/，`REGISTER_TEST` 宏 + `test_support.h` 的 `eui::Ui` harness）③ 2026-10-03 轻量化裁决（commit `9d032c2`，见 §1）
> 注意：仓库根 CLAUDE.md「probe 轻量化后剩 4」口径已过期——`agent_e2e_probe` / `agent_dialog_probe` 为 agent M2/M3 新增（agent_dialog_probe 已随 GLOB 注册，见 tests/CMakeLists.txt:233-289）。

## 0. 结论速览

- **缺口共 5 个**：markdown 2 ／ claude_proxy 1 ／ home 1 ／ agent 1 ／ todo 0 ／ settings 0 ／ about 0 ／ pet 面不存在（模块 2026-10-13 整体摘除，src/main.cpp:645）
- 覆盖底盘厚：无头 harness 处数 todo 408 / claude 518 / markdown 440 / settings 149 / agent 59 / home（test_home + test_home_quota_card）≈30+；probe 侧 home_probes 组 A–M、settings_dropdown_z、module_toggle_sidebar、agent_dialog、agent_e2e 各有独占判据
- 下文「已覆盖/缺口」证据均为 file:line（源码=src/ 下，测试=tests/test_modules/ 下）

## 1. 轻量化裁决锚 —— 「不建议回补」清单（刻意决策，不是缺）

commit `9d032c29`（2026-10-04 落 10-03 裁决）"轻量化第一批 —— probes 12→4"，删 8 个与无头单测逐条重复的 probe；docs/archive/lightweight-audit-2026-10-03.md:13-44 为判据文档。对应面**不要**再列缺口、不要回补 probe：

| 被删 probe | 对应 UI 面 | 现由谁覆盖（证据） |
|---|---|---|
| about_probe | about 布局/辉光 | test_ui_modern.cpp:1030（几何：900 内容区/贴底/极矮窗 clamp）、:1236（6 光斑参数） |
| agent_probes | agent 列表/弹窗几何 | test_agent_ui.cpp:242/:1667/:3524-3666（compose+三档几何） |
| claude_calls_latency_probe | calls 延迟展示 | test_claude_calls_latency.cpp（纯逻辑层） |
| claude_probes | profile 表/弹窗几何 | test_claude_ui.cpp:1691/:5693/:5743/:6613/:8239 |
| markdown_probes | md 树/浮层/双栏 | test_markdown.cpp:726/:1712/:1831（zIndex）/:3712（esc 栈） |
| pet_probes | 桌宠窗（曾是必红 TODO，docs/archive/lightweight-audit-2026-10-03.md:34-36） | 页面已整模块摘除，问题随面消解，无欠账 |
| settings_cards_probes | 设置卡片几何（曾是必红 TODO：effect.hint 无 clip/wrap） | 必红项已由 `8df78dc` Ellipsis 批修复；卡片几何现由 test_settings.cpp:790/:843/:1367/:1571 覆盖 |
| todo_probes | 看板/拖拽/逾期 | test_todo_ui.cpp:1008/:1479/:1590/:1748/:3170/:4577（拖拽=纯几何函数+帧驱动模拟，OS 级真指针**刻意不测**） |

## 2. 每页四问盘点（可见变化 / 交互链 / z 序 / 状态态）

四问标记：✅有断言（附证据）｜⚠部分｜❌无（进 §3 计缺口）。交互链/z 序区分「真派发栈（probe 独占）」与「状态驱动模拟（无头可做）」。

### home（含余量卡/滚动模式）
- 可见变化 ✅：test_home.cpp:190（速率卡三态 compose 冒烟）、test_home_quota_card.cpp:265（五态渲染 bar/tint/文案）、:686（负例不画条）；home_probes 组 G 统计网格逐字钉（home_probes.cpp:740-752）
- 几何 ✅：test_home_quota_card.cpp:748（矮窗 720 卡不丢内容）、:870+:899-924（滚动模式预算分支：drawsQuotaCard/chartHeight）；home_probes 组 K 三档 845/805/700（:1486-1490）+ 组 A spark/label 不相交（:511-521）+ 组 M 按钮等宽（:1838-1849）
- 交互链 ✅（probe 独占，弱交互页够用）：home_probes 组 B 真帧循环重绘计数（:530-543）；min_window 变体走真滚动管线（tests/CMakeLists.txt:306-314）
- z 序 ✅（probe 独占）：组 A spark 顶带整带位于 label 之上（:493-521）
- 状态态 ✅：空态「--」与图表空态（test_home.cpp:265/:302）；清库后滚动条消失+未配置行仍在（home_probes.cpp:1422-1453）；错误文案全树恰 1 次且不溢出（:1154-1203）
- **缺口 G4** ❌：`home.actions` 快捷按钮组「目标模块关→按钮隐藏、宽度按剩余均分、全关整行隐藏」（home_module.cpp:758-775）无任何**联动**断言——现有只是在场/几何（test_home_quota_card.cpp:785、home_probes.cpp:712/:1515）；test_module_gating.cpp:214-281 只测统计卡行隐藏，没测 actions 行

### markdown（编辑器/预览/目录树/搜索）
- 可见变化 ✅：test_markdown.cpp（440 处 harness）：:541（三态/大纲/状态栏 compose）、:851（空树+搜索 placeholder）、:7416（FTS 搜索过滤树实时态）
- 几何 ✅：:726（树行）、:1712（双栏三列无残隙）、:2120（状态栏两段对齐）
- 交互链 ✅（状态驱动模拟）：:2133（右键删链）、:2638（ctrl+n）、:3712（esc 关最上层）；test_md_keyboard.cpp:291/:386/:461（真编辑器输入 wiring）；test_md_live_spike.cpp:428/:487（pointer→caret、滚动残差）
- z 序 ⚠：大纲浮层 zIndex 有无头断言（:1831），但**真渲染浮层 z 碰撞无 probe 判据**——属 §1 markdown_probes 裁决范围，不回补
- 状态态 ⚠：树空态 ✅（:851）、外部修改冲突 toast ✅（:6784 MD-01、:6865 Ctrl+S 保留本地）；**缺口 G1/G2 见 §3**（树列表加载失败态、状态栏保存失败态均无断言）

### todo（看板/拖拽/逾期）
- 可见变化 ✅：test_todo_ui.cpp:202（compose+右键菜单）、:2364-2468（逾期弹窗/置灰）
- 几何 ✅：:1008（卡 inset/dropbar 宽）、:1748（列滚动几何）、:3170（due chip 窄列适配）、:4577/:4657（ellipsis）
- 交互链 ✅（状态驱动）：:302（键盘移动）、:3712/:3843/:3953（ESC 栈）、:4151-4333（event wiring）、:1374/:1479（拖拽命中+框架尺度）；OS 级真指针拖拽由 §1 todo_probes 裁决刻意不测
- z 序 ⚠：无 probe 判据——同属 §1 裁决范围，不回补
- 状态态 ✅：:1320（空列 dropbar）、:1819（错误态+重试）、:2511/:2577（查询/移动失败）、详情弹窗 :462/:542/:721/:1898；无状态覆盖良好
- **无缺口**

### claude_proxy（profile 表/详情弹窗/配置弹窗）
- 可见变化 ✅：test_claude_ui.cpp:1691（表滚动）、:505（表单 tabs roundtrip）、:5451（详情四 tabs 锚）、:7945-8274（quota 弹窗动态字段）
- 几何 ✅：:6105/:6184/:6369（badge/头像）、:5693（calls 行按高度）、:5743（矮窗零行）、:7096（key 溢出）、:8239
- 交互链 ✅（状态驱动）：:1813（esc 取消）、:2970/:3239（advanced 持久化）、:8123（quota 弹窗 ccswitch load 链）；真出网验收走 live（LABELS live 不进 probe 门禁，tests/CMakeLists.txt:333-343 设计如此）
- z 序 ✅（probe 独占）：无专测——claude_probes 已删属 §1 裁决；配置表单作为模态的 z 行为被 settings_dropdown_z 的通用 L2 扫描间接背书，不回补
- 状态态 ✅：:456-460 对应空态、:5743 零行、:4444/:5192 纯函数边界；calls 弹窗三态（claude_ui_logs.cpp:263/:272/:107）有无头几何+空行覆盖
- **缺口 G3** ❌：cc-switch **导入弹窗本体**（claude_ui_ccswitch.cpp:253 列表渲染、:280 无内容可导入空态）零 UI 断言——test_ccswitch_import.cpp 全纯逻辑（无 harness），test_claude_ui.cpp:7982/:8102 测的是 quota 弹窗里的 ccswitch 引用，不是导入弹窗

### agent（列表/创建弹窗/详情弹窗/草稿子弹窗/时间线）
- 可见变化 ✅：test_agent_ui.cpp:242（骨架）、:4637（状态视觉）；agent_dialog_probe.cpp:310-359（详情弹窗错误行+引导+复制 chip）
- 几何 ✅：:1667/:3250（弹窗几何预算）、:3524-3666（wide/narrow/short 三档）、:1845（详情几何+空态）
- 交互链 ✅（probe 独占判据）：agent_e2e_probe.cpp:546-587（真弹窗建任务：点 newTask→真键入→save）、:676-772（真点 .run 启动）；agent_dialog_probe.cpp:173-240（任务弹窗定时分支真键入+模板下拉）、:298-327（取消/详情 chip 真关真开）；无头侧 :398/:853/:896（保存/坏 cron/cancel+backdrop+esc）
- z 序 ✅（probe 独占）：agent_dialog_probe.cpp:277-283（模板弹层 assertNoEqualZOverlap）
- 状态态 ✅：:1614/:1749/:1779 对应的 page.error（:1541、:2869 引导跳 Claude）、runlist 空态、时间线错误行（:1483）；草稿流全路径（:4076-4419：chip/异步预填/降级警告/关闭丢弃）
- **缺口 G5** ⚠：草稿子弹窗（agent_module.cpp:2379-2430）**叠在任务弹窗上**的双层 z 序无判据——agent_dialog_probe 只扫了单层模板弹层；双层叠加是唯一「弹窗上开弹窗」面
- 注：详情弹窗 P1–P5 重设计在飞（docs/superpowers/plans/2026-10-18-ui-redesign-task-map.md，spec P5 自带新用例+探针补全），重设计后的面不在此列缺口；agent_e2e 依赖 LLM 腿、不可用时全记「未跑」是设计使然（:609-615 四态结论表），不算覆盖缺口

### settings（模块开关/主题）
- 可见变化 ✅：test_settings.cpp:619/:1253（卡片 compose）、:759/:2088-2215（主题切换/auto 跟随）
- 几何 ✅：:790（下拉不出窗）、:843（路径垂直居中）、:1367（最小窗适配）、:1571（行缩放）
- 交互链 ✅（probe 独占判据）：module_toggle_sidebar_probe.cpp:211-253（真点开关→当帧出现/消失/redirect+toast）；无头侧 test_settings.cpp:1914/:1982/:2184（esc 草稿/ctrl+S/下拉点击）、test_app_sidebar.cpp:342/:416（开关即时生效）
- z 序 ✅（probe 独占）：settings_dropdown_z_probe.cpp:136-143（跨卡等值 z 碰撞 L2 扫描）
- 状态态 ✅：:1298/:1335（模块关→整卡隐藏）、test_module_gating.cpp:62-281（默认/往返/损坏回退/全关）；GitHub Sync 卡有专门测试（test_github_sync_card.cpp）
- **无缺口**（settings_cards_probes 裁决不回补）

### about
- 可见变化/几何 ✅：test_ui_modern.cpp:1030-1205（内容区/贴底/极矮窗 clamp/页脚）、:1236（辉光参数）；恒注册旁证 test_app_assembly.cpp:488/:529
- 交互/z 序/状态态：页面无交互无浮层无数据态，N/A
- **无缺口**（about_probe 裁决不回补）

### pet（独立窗）
- **面不存在**：整模块 2026-10-13 摘除（feat/remove-pet-module，src/main.cpp:645；src/ 下无 module_pet，app_registry 无 pet 项）。pet_probes 必红 TODO 随面消解。列此仅为了闭环任务清单，不是缺口

## 3. 缺口清单与建议补法

| # | 面 | 定性 | 位置 | 建议补法 + 理由 | 规模 |
|---|---|---|---|---|---|
| G1 | markdown | 缺状态态 | 树列表加载失败「列表加载失败：…每 5 秒自动重试」（markdown_module.cpp:596-630，listError 分支） | **无头单测**：注入 service 错误→compose 断言错误行在场+重试提示文案。状态驱动 UI，无头是既有惯例（对齐 :851 空树用例写法） | S |
| G2 | markdown | 缺状态态 | 状态栏保存失败「保存失败，自动重试中」（markdown_module.cpp:1228-1311） | **无头单测**：只读文件/坏路径强制保存失败→断言状态栏失败文案+重试态。数据安全反馈路径，现零断言 | S |
| G3 | claude_proxy | 缺 compose+交互链+状态态 | cc-switch 导入弹窗本体（claude_ui_ccswitch.cpp:253 列表、:280 空态、导入确认链） | **无头单测**：列表 compose + 无内容空态 + 「选择源→导入→落库」状态驱动点击链（对齐 :2133 delete_note_ui 写法）。纯逻辑层已有（test_ccswitch_import.cpp），缺的正是 UI 呈现层；真实用户迁移路径目前全靠手测 | M |
| G4 | home | 缺状态态 | home.actions 模块开关联动（home_module.cpp:758-775：关→按钮隐、宽均分、全关整行隐） | **无头单测**：改模块注册态→断言 actions 行宽均分/整行消失（对齐 test_module_gating.cpp:214-281 统计卡写法，同一机制另一半） | S |
| G5 | agent | 缺 z 序 | 草稿子弹窗叠任务弹窗双层 z（agent_module.cpp:2379-2430） | **probe**：扩展现有 agent_dialog_probe，草稿打开态跑一遍 assertNoEqualZOverlap（判据现成，settings_dropdown_z/agent_dialog 同款）。双层「弹窗上弹窗」是全 app 唯一，DOM zIndex 断言证不了渲染结果——probe 独占场景 | S |

## 4. 值得补的 top 排序（感知收益÷成本）

1. **G1+G2 markdown 错误态两项**（合一批做，S+S）——保存失败/列表失败是仅存的两个「用户可见但零断言」的数据安全反馈；无头 S 级成本，收益最高
2. **G3 cc-switch 导入弹窗**（M）——真实迁移入口 UI 层零覆盖，纯逻辑已备只差呈现；一个文件补齐 compose+空态+导入链
3. **G5 草稿双层 z 序**（S）——probe 判据现成、只加一段；双层浮层回归一旦发生无头全绿（假绿风险点）
4. **G4 home.actions 联动**（S）——机制与已测的统计卡门控同源，回归概率低，但补上后模块门控才算闭环

（均不建议以 probe 起步除 G5；§1 清单内各面不要回补 probe。）
