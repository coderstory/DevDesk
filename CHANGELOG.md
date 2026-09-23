# CHANGELOG

本文件记录 **a1a106a（macOS 中断现场收编）起** 的条目式变更，按恢复计划的三波（W1 止血收编 / W2 平台架构 / W3 剩余需求）组织，波内按 feat / fix / test / docs 分组。更早的 macOS 主线历史（P0–P8，66 测试通过）见 git log 与 spec §16。

> **当前基线（2026-09-23，master @ 296b9c8）**：**330/330 测试全绿**。各波次条目内的测试数为该波时点事实，不代表当前总数；近期波次按合并时间倒序排列（[Pet-Fix] 之后已并入 audit-batch1 +13、winfx-pet +1 两波，代码修复细节见对应分支的 commit message，未单列波次条目）。

## [Pet-Fix] 2026-09-23 —— 桌宠 Windows 真机四症状修复

分支 `fix/pet-window-bugs`（EUI-NEO 同名分支已合上游 main）。首次真机验证暴露
「状态对但行为错」四症状，全部先根因后修；**DevDesk 312/312 全绿**（+1 拖拽
锚点用例），EUI-NEO 46/46 全绿。根因链详见各 commit message。

### fix（EUI-NEO，D:/project/gui/EUI-NEO main）

- **症状 2 黑背景**：`GLFW_TRANSPARENT_FRAMEBUFFER` hint 只由 app 级窗口效果
  档位驱动（`windowEffect != None` 才开），设置页「关闭」档（用户实测即此档）
  → `setWindowEffect(None)` → 桌宠 sprite 窗不透明创建，全透 clearColor 落在
  不透明表面 = 黑底。新增 per-window `transparentFramebuffer`（DslWindowConfig
  → DslWindowRequest → WindowCreateRequest 三层透传），与全局档位解耦。
- **症状 3 任务栏图标**：双重根因——GLFW 的 Win32 窗口自带 `WS_EX_APPWINDOW`
  （与 `WS_EX_TOOLWINDOW` 互斥，只加不清不消失）且可见创建先注册任务栏按钮。
  修法：window_style_win 对称清/补 APPWINDOW + `visible` 创建期属性
  （hideFromTaskbar 窗隐藏创建、样式就位后 glfwShowWindow 首秀）。
- **症状 4 主窗点 X 无反应**：托盘可用时 close callback 撤回关闭并请求隐藏到
  托盘，但主循环「有子窗即取消隐藏」守卫把请求吞掉——桌宠存活期间点 X 什么都
  不发生。删守卫（模态保护在 callback 层）+ hiddenToTray 分支补子窗泵（桌宠
  动画/输入/开窗关窗照常，设计 R5「宠物独立于主窗存活」）。
- **症状 1 拖拽抖动（上游示例同款）**：examples/pet_window 拖拽改锚点跟随。

### fix（DevDesk）

- **症状 1 拖动抖动非常厉害**：onDrag 按 `DragEvent.delta` 移动窗口——delta 是
  窗口内相对坐标，窗口自身移动后参考系漂移（+10/-5/+10 振荡），且单位是帧缓冲
  像素（DPI≠100% 时差一个 contentScale）。改 `PetDragAnchor` 锚点跟随
  （getWindowPos + getCursorPosition 光标屏幕位 + 锚点，1:1、DPI 无关），
  `petDragMove/petDragEnd` 纯函数化。
- **症状 2（DevDesk 侧）**：开窗请求声明 `.transparentFramebuffer(true)`，
  `pet_start_window_request` 补透传断言。

### test

- `pet: 拖拽锚点跟随——1:1 跟光标，无反馈抖动`（旧方案必现负增量的轨迹回归）
- `pet: 开窗请求逐字段断言（无头）` +transparentFramebuffer
- EUI-NEO `pet_window_config` +transparentFramebuffer 透传/默认值断言

### 冒烟（真机，2026-09-23）

窗口样式枚举（TOOLWIN=1/APPWIN=0）、主窗 WM_CLOSE → 隐藏到托盘且进程/桌宠
存活、托盘驻留期间桌宠持续动画（截屏差分 135px）、桌宠窗口角落像素非黑
（255,255,25x，桌面透出）、桌宠 WM_CLOSE 仅关桌宠。任务栏按钮不出现、拖拽
手感、磨砂视觉三项登记人工点验。

## [Theme-Hierarchy] 2026-09-23 —— 浅色主题层级全量审计与修正（结构性面板错用卡片白）

分支 `fix/theme-hierarchy`（worktree `wt-themehier`，基线 e0b0a70 侧边栏修复后续）。**315/315 全绿**（+4 层级回归用例）。

**根因**（承接侧边栏修复）：结构性面板（页面结构的一部分）错用卡片白 `surface`（浅色 #FFFFFF），浅色模式下「页面米色、面板纯白」刺眼。层级口诀（spec §6.5 方案 B）：页面结构 → `background`（#FAF9F5 / 暖墨 #1A1816）；浮动卡片/对话框/弹层/按钮 → `surface`/`surfaceElevated`（白 / #232120 / #2C2A28）；行 hover / 分组底 → `hover`（#F5F2EC，既有 surfaceMuted 语义由 hover token 承担）。

### fix（结构性面板 → background）

- **markdown**：左栏树面板 `md.tree.bg`、编辑区面板 `md.editor.bg`、预览区面板 `md.preview.bg`、编辑器底部状态栏 `md.status.bg`、树行底（文件行/文件夹行）——全部 `surface` → `background`（行底随面板，hover 仍 #F5F2EC 显色）。
- **todo**：看板列容器 `todo.col.N.bg` `surface` → `background`；看板卡片 `surfaceElevated` → `surface`（深色回落 #232120 卡片层级，曾被无谓抬到浮层色；浅色两值同为白无视觉变化）。
- **claude**：左栏 Profile 列表 `claude.profiles.bg`、右栏详情面板 `claude.detail.bg`、最近调用「查看全部」命中区——`surface` → `background`。
- **markdown 大纲浮层** `md.outline.panel.bg` + 大纲行底：`surface` → `surfaceElevated`（zIndex 浮层语义，深色下正确抬到 #2C2A28 浮层色）。

### 审计确认无需改（防过度修正）

- **home**：统计卡 / 最近编辑卡是**浮动卡片**，留 `surface` 白（浅色下卡片白浮在米色页面上正是 Claude.ai 风格）；行底随卡。**settings**：卡片浮动该白；卡内嵌字段框（路径/Token/字段底）是控件层 `surfaceElevated`，层级正确。四模块页面底 `pagebg` 均已是 `background`。
- **EUI-NEO componentTokens 映射**（`core/component_theme.h`）：dialog 面板/dropdown 弹层 → `surface`（浮层 ✓）；dropdown 字段/input 底/segmented 轨道 → `surface`（控件层白，Claude.ai 象牙底+白输入框同款）；scrollView 不铺底（透出结构性面板底 ✓）。上游零改动。
- **深色同规则核对**：修正后层次 = 页面/面板 #1A1816 < 卡片 #232120 < 弹层/按钮/字段 #2C2A28 < hover #353231，符合 spec §6.5.1。

### test

- `tests/test_modules/test_theme_hierarchy.cpp`（+4 用例）：无头 compose 色值断言（防再漂移）——todo（列容器=background、卡片/dialog=surface、交互打开 dialog 后复验）、markdown（左栏/编辑/预览/状态栏=background，Ctrl+Shift+P 切 Split 保三栏全渲染）、claude（左栏/详情=background）、home（统计卡=surface 不得跟风变米色）；明暗两主题各验一遍。

### 人工点验项（无头测试覆盖不到的）

- 侧边栏（`main.cpp app.sidebar.bg`，e0b0a70 已修，无头 compose 不含 main.cpp）浅色米色/深色暖墨；
- 浅色下逐页目测：markdown 三栏、todo 看板列、claude 双栏、home 卡片浮起感、settings 卡片；深色下看板卡片 #232120 与列底 #1A1816 的层次。

## [PhaseC-MD-Highlight] 2026-09-23 —— 编辑器 Markdown 语法高亮（styled-runs Phase C）

分支 `feat/md-highlight`（worktree `wt-mdhl`）。上游 Phase A/B 已在 EUI-NEO main @ b7e0e9f。基线 303/303 → **310/310 全绿**（+7 高亮用例）。

### feat

- **md_line_scanner 扩展**：`scanFences(text) -> vector<MdFenceBlock>`（开栏行/内容区间/闭栏行字节偏移，``` 与 ~~~ 各自配对，未闭合到文末）；`scanHeadings` 改为消费 scanFences——大纲与高亮共用一套围栏状态机（一鱼三吃），原围栏内标题语义不变（既有用例全绿）。
- **行级高亮器** `src/module_markdown/md_highlight.{h,cpp}`：`MdHighlighter::runsForLine(fullText, lineText, lineStartByte) -> vector<TextRun>`（全文档坐标）。块级：标题 `#` 前缀、引用 `>`（正文 textSecondary + 行内 token 后写覆盖叠加）、列表 `-`/`*`/`+`/`N.`、表格管道（仅 `|` 开头行）；行内：`` `code` ``（同长度反引号配对，含双反引号）、`**bold**`（标记弱化，内容等 Phase D weight run）、`*italic*`（无 italic 字形用 info 色替代）、`[text](url)` / `![alt](url)`。围栏行整行弱化、围栏内容整行 success 色；围栏状态由高亮器内部按 display 文本重建（文本变化首回调一次 O(n)，其后的可视行回调走等值快路径）——与框架回调同源（IME preedit 快照），字节坐标不会错位。块级规则仅硬行首触发（软换行段中恰以 `# ` 开头不误判）。
- **色表** `mdHighlightColors(Tokens)`：§6.2 全部映射现有语义 token（标记 textTertiary / 代码 success / 链接+斜体 info / 列表 primary / 引用正文 textSecondary），明暗两套自动适配，未新增 token。
- **接线**：markdown_module 编辑器 input 挂 `.lineStyles(回调)`（从 InputState 解析当前 display 文本喂高亮器）；主题名变化 = 换色表 = bump `InputState.styleRevision` 触发全部可视行重高亮（compose 每帧检查，无模块间耦合）。

### test

- `test_md_highlight.cpp` +7 用例：scanFences 区间（配对/未闭合/空内容/CRLF）、块级前缀与行内 token 字节区间断言（CJK 边界、未闭合标记不误标、粗体星不误判斜体）、围栏跨行、软换行段防误判、色表映射、无头接线（行元素 runs 落色 + 主题切换 styleRevision++ 且换色）、万行性能冒烟。
- 性能实测（万行 / 2500 围栏块 / 可视窗 27 行）：首帧 1.13ms，击键轮（围栏重建 + 27 行重高亮）avg 0.98ms / max 1.53ms——预算 8ms 内 5 倍余量。

### docs

- 设计文档 §8 Phase C 状态回填；预览侧 `renderInlineRuns` 收敛为 `.runs()` 评估结论：**不做**（segment 间 fontFamily/fontSize/fontWeight 真实不同——代码段是 monospace 小字号，颜色 run 无法合并异字体段，重构手工 inline 布局 > 半天，见设计文档登记）。

## [Pet-Anime] 2026-09-23 —— 二次元桌宠皮肤（真实美术素材包）
分支 `feat/pet-anime`（基线 ad5c94d，含 merge master f6d0ac3）。**304/304 全绿**（+1 内置美术皮肤断言）。

### feat

- **内置美术皮肤 `anime-girl`**（用户调研指名的「二次元/女性角色」）：OpenGameArt
  "Anime Girl"（Michona）by Alicahordana，**CC0** 可再分发（满足素材纪律 R7，调研报告
  §3 渠道红线）。素材为 221px sprite strip（blink/walk/jump 三组），帧提取后按全局统一
  缩放规整到 128×128（脚踩底边、内容居中，保纵横比），四态映射：blink(5)→idle、
  jump(13)→poked、walk(11)→typing、闭眼帧(1)→sleep，共 30 帧 + pet.json 落
  `assets/pets/anime-girl/`（~600KB）。
- **assets 分发机制**：顶层 CMake POST_BUILD 拷 `assets/pets/` → build 目录（沿用
  fonts 同款机制）；运行期 `skinRootCandidates()` 的 cwd/assets/pets 候选零改动即发现，
  皮肤下拉/右键换肤菜单自动出现 anime-girl。

### docs

- `assets/pets/README.md`（新）：皮肤包结构 + 手动安装第三方素材指南（含 itch.io
  "Desktop Pet Sprite"（tatsu-san）的下载/解包/四态映射表——其页面为口头许可无正式
  开源协议，按「下载后放置」模式不入仓库）。
- 桌宠设计文档 §4.3 补记内置美术皮肤与分发机制。

### test

- `pet: 内置美术皮肤 anime-girl（CC0 四态断言）`：从源码树真实加载（__FILE__ 相对
  定位），断言四态帧数/128×128/全帧 PNG 结构/license 含 CC0/皮肤名合法。

### 调研结论（候选素材实测）

- itch.io "Desktop Pet Sprite"（tatsu-san，免费）——34MB PyInstaller exe，帧已提取
  （walk/run/eat/sleep/grab，128×128），许可为口头声明 → 不入仓库，走手动安装
- itch.io "Lacey Animated Pixel Doll"（kyoii011，调研报告 §3 候选）——**$5 付费**，放弃
- OpenGameArt "Anime Girl"（Michona，CC0）——**采纳内置**
- itch.io "School Girl Sprite Pack"（chibi785）——CC-BY-NC-ND 4.0，触碰红线，放弃

## [Final-Verify] 2026-09-23 —— 终验发现问题收尾批（12 项）

分支 `fix/final-verify`。基线 290/290 → **291/291 全绿**（+1 设置页布局断言；EUI-NEO 侧另加 1 个单测）。

### fix

- **模块隔离**：module_pet 直 include module_settings 的 SettingsStore（违反 module_* 互依禁令）——petSkin/petX/petY/petClickThrough 四字段从 config.json 迁到 `petsDir()/pet_config.json`，module_pet 自有 `PetConfigStore` 读写（存量旧 pet 坐标一次性丢失，桌宠回默认位置）。
- **逾期弹窗溢出**：高度封顶 640 但行全量渲染，≥13 条画到窗外——行循环按 (640-96)/44=12 条截断，提示行标注「仅显示前 N 条」。
- **设置页效果下拉溢出**：固定 x=680 在 1200 最小窗下超出外观卡 60px——字号/效果两列改右缘自适应（宽窗原位不变），补 1200 宽断言。
- **键盘流 spec 对齐（§5.2）**：Ctrl+B/I 再按去包裹（选区两端恰为 **/* 时移除）；Tab 缩进 2→4 空格（反缩进每行去 1 Tab 或 ≤4 前导空格）；顺带修 applyTab 偏移修正的坐标混用（多行反缩进选区端点偏 1）。
- **overdueDays 改本地日历日差**：昨天到期今天看 = 1 天（原秒差向上取整在 23:59 到期/次日 00:01 查看时虚报 2 天）。
- **桌宠 petX/petY 单边 -1 哨兵**：原 x/y 任一边未设置就两边一起重置——改单边独立；删除死代码 `discoverSkins()`。
- **桌宠菜单文案**：「隐藏桌宠（设置页可重新开启）」→「本次会话；重启后恢复」（设置页开关本就需重启生效）。
- **md 切笔记清空 tagInput_**；空态文案「+ 新建」→「+ 笔记」。
- **FA 残留清扫**：设置页降级提示 ⚠、Todo 弹窗标题 ⚠/卡片勾 ✓/详情按钮 ✓↩ 全部换 FA icon 通道。

### test

- Todo 拖拽防误触断言去空洞化：直接调回调时 onClick 从不派发、旧断言恒真——改驱动 EUI-NEO `InteractionState` 真实时序（press→超阈值 drag→release 回卡内 = clicked=false；对照组原地 release = clicked=true）。
- EUI-NEO `tests/unit/exit_request.cpp`（上游 main 本地 commit）：requestExit/consumeExitRequest 恰好一次语义 + requestWindowClose(nullptr) 守卫。

### docs

- KNOWN_LIMITATIONS 重复 #12 重新编号（12-16 连续）；CHANGELOG 补登波次 3+4 各批（见下节）；CLAUDE.md 测试数 137→291。

## [Wave-3+4] 2026-09-22/23 —— 波次 3+4 补登（自主时段批次汇总）

对照 `docs/superpowers/plans/2026-09-22-backlog.md` §二补记此前漏登的批次（Module-Gating / Provider-Config 已有专节，不重复）。EUI-NEO 侧上游 commit 均在 `../EUI-NEO` main（本地领先 origin，未 push）。

### 磨砂全链 Phase A–D（spec `2026-09-22-eui-window-frost-design.md`）

- Phase A：标题栏深浅主题联动（见 [W8] 条目）。
- Phase B/C（EUI-NEO 上游）：透明管线 + 运行时 `app::setClearColor`（覆盖主窗口 + 全部子窗口 + requestFullPaint）；DWM backdrop `setWindowEffect/activeWindowEffect` 运行时 API。
- Phase D（`4f6fbac`）：设置页「外观」卡窗口效果三档（off/translucent/frosted）——档位→参数纯函数（clearColor alpha + backdrop 请求 + 降级提示矩阵）、`applyWindowVisuals` 三处共用接线、透明 hint 常开三档运行时切换不重建窗口；顺手根治 KNOWN_LIMITATIONS「切主题底色需重启」（244/244）。

### EUI-NEO 上游批（DevDesk 消费）

- 拖拽取消 click（`InteractionState.dragMoved`：超阈值拖动后松手不派发 click）——DevDesk 删 Todo 卡片本地抑制逻辑（`3dba229`）。
- toast action 按钮（单按钮 + 右上 ✕）——Markdown 外部修改二选一从 dialog 改 toast action（`97db502`：重载磁盘 / 保留本地，duration(0) 强制显式决定）。
- `input.onKeyEvent` passthrough——Markdown 键盘流的通道 A（见下）。
- 按钮 disabled 自动置灰、托盘 UTF-16 乱码修复。

### Markdown 布局修复（`8cbc2ba`）

- Split 双栏 `midW` 公式笔误 + 三栏「column 铺底 + overlay stack」双占位结构导致双栏各窄 128px、预览溢出屏——改单 stack 铺底 + 内容绝对定位，补 headless layout 回归（235/235）。
- 默认视图改编辑态（左树 + 右编辑两栏，三态循环 Edit→Split→Preview）；目录树根组标题常显置顶 + 空目录空态同显；新建文件夹入口 + 新建笔记子路径提示（240/240）。

### Todo 功能补完（`a6f3742`，W7 计划 P2 三件套）

- 搜索框（真实 `service->search`，命中列照常渲染只显示命中卡，Enter/Esc 清空）；逾期视图（badge=listOverdue 数量、0 置灰，弹窗四列数据、行点击跳详情）；tags 编辑（详情弹窗回车添加/× 删除，卡片前 2 个 chip + 溢出 +N）；列色点 + 右键改色（6 预设，`setColumnColor` 落库）。纯函数 `columnDotColor/overdueDays/filterBySearch` 落 `todo_ui_logic.h`（241/241）。

### Markdown 标签系统（`d58379d`，spec §8.1）

- 服务层 `markdown_tags/markdown_note_tags` 表首次接线：`setTags` 全量替换（事务 + 孤儿清理）、`tagsForNote/listAllTags/notesWithTag`；UI 手动标签最简形态（编辑器顶栏 chip 行 + 左栏筛选，点击过滤树）+5 用例（245/245）。

### Markdown 键盘流（`9b7558f`，spec §5.2 通道 A）

- `md_keyboard` 纯函数（EditBuffer 字节偏移语义）+ `handleEditorKey` 接线（IME 预编辑防御 → 纯函数 → 写回 InputState 含 undo/textRevision/onChange）：Tab 缩进、Enter 列表续行（前缀继承/空项退出）、Ctrl+B/I 包裹、Ctrl+K 链接骨架，+15 单测；顺带修反缩进误删非前导空格。
- FA 收尾（`97db502`）：Markdown/Todo 全量 emoji→FontAwesome（码点对 FA7 Free-Solid cmap 离线核对）；`#FFFFFF` 硬编码 → `t.colors.onPrimary`。

### 桌宠 MVP（`bbac916`，spec `2026-09-22-desktop-pet-design.md` M1）

- module_pet（非 Router 页面模块）：pet.json 皮肤解析 + 内置占位皮肤 clay-cat 程序生成（PNG 未压缩 deflate 写出器零第三方依赖）；帧状态机（Idle 循环/poke 播一遍）；`app::openWindow` 子窗口（无边框/置顶/128×128/不抢焦点/不进任务栏/全透，EUI-NEO G1-G5 扩展）+ 拖拽 setWindowPos + 右键菜单（穿透/隐藏/退出）+ 持久化 + 工作区钳制；`kModuleIdPet` 门控（默认关）+ `core::paths::petsDir()`，+10 用例（285/285）。子窗口视觉呈现因企业水印层无法自动截图，人工点验。

### 打磨批（`426584a`）

- 调用时间列跨天显示（`formatCallTime`：今日 HH:MM/昨天/更早 MM-DD）；Home/Claude/设置页 emoji→FA；设置页卡片区套 scrollView（4 卡总高 580 > 小窗口可视 552，第 4 卡被裁）；`onPrimary` token；CLAUDE.md 架构图 4 模块改 5（237/237）。

### 托盘图标（`049a4f4`，波次 2 批次补记）

- `.tray` 托盘 + 四模块快速跳转菜单（EUI-NEO tray-menu PR 已合 main）。

## [Module-Gating] 2026-09-22 —— 模块插拔方案 A（编译期内置 + 运行时注册门控）

设计文档 `docs/superpowers/specs/2026-09-22-module-plugin-architecture-design.md`（推荐方案 A 为权威）；分支 `feat/plugin-gating`。最终基线 **275/275 全绿**（259 基线 + 16 新增）。

### feat

- **enabledModules 配置**：`config.json` 新增 `enabledModules` 字符串数组（缺省全启用，老配置零迁移）；`home`/`settings` 恒启用（落地页 + 开关 UI 宿主，sanitized 强制补齐），`markdown`/`todo`/`claude_proxy` 可关；未知 ID 忽略 + WARN。为此给 `platform::json` 补了最小数组读写能力（`size()/operator[]` 实装 + `makeStringArray`，原先是返回 0 的桩）。
- **注册门控**：`main.cpp getRouter()` 按 `isModuleEnabled()` 决定三个工具模块是否 `registerModule`——不注册 = onStart/compose/onKeyEvent/侧边栏入口全部不可达（结构性零执行，不靠模块自觉）；Claude 模块关闭时代理整体下线（不创建 ClaudeService、8888 不监听、`proxy_token.txt` 启动时清理陈旧文件），与 `claudeProxyAutoStart` 正交（模块开关优先）。
- **Router 加固**：新增 `isRegistered()`；`navigateTo()` 目标页未注册 → WARN + 回退 Home（不进"无页面"空白页；Home 也未注册的极端防御分支保持当前页）。Home 卡片、托盘菜单、文件打开分发（`.md`→Markdown 等）的残留跳转入口全部经此兜底。
- **Home 仪表盘降级**：统计卡/快捷按钮/最近编辑按 Router 注册状态隐藏（宽度按剩余数量现算，n=1..5；三模块全关出「所有工具模块已关闭」空态文案）；不改 HomeService SQL，隐藏纯在渲染层。
- **设置页「模块」卡**：三个工具模块各一行开关（名称 + 描述 + switch），描述注明「关闭模块后需重启应用生效」（注册是启动期行为，重启生效是设计 §5.4 的一期决策）；claude 关 → 代理卡整卡隐藏，markdown 关 → 目录卡整卡隐藏（不显示故障态）。

### test（16 新增）

- config 语义 6 用例：默认全启用/恒启用项/往返/未知 ID 过滤/空数组/损坏回退（`test_module_gating.cpp` + `test_settings.cpp`）。
- Router 4 用例：`isRegistered`/未注册导航回退 Home/负载作废/全未注册防御（既有 payload 用例补注册桩适配新语义）。
- 注册门控 1 用例：按 config 注册后关闭模块不在 `modules()`。
- Home 降级 3 用例：三模块全关/部分关/全开等价（scoped appRouter 注入注册状态的无头 compose）。
- 设置页 3 用例（含既有 openUp 用例适配 5 卡新布局改窗口高度）。

### 已知边界

- Claude 模块关闭期间不执行崩溃补还原（`recoverPendingSettingsRestore` 随 service 一起下线）：若上次退出时处于接管状态且本次以关闭 claude 模块启动，`settings.json` 保持托管值直至重新启用。见 KNOWN_LIMITATIONS。

## [Provider-Config] 2026-09-22 —— Claude 供应商完整配置（Phase A–E 全量落地）

设计文档 `docs/superpowers/specs/2026-09-22-claude-provider-config-design.md`；分支 `feat/provider-config-bc`（A/B/C，已合 master）+ `feat/provider-config-de`（D/E）。最终基线 **233/233 全绿**（226 基线 + 7 新增）。

### feat（Phase A/B/C，已随 master 合入，此处补记条目）

- **数据模型 v3**：`claude_profiles` 增 `models_config`（6 模型槽位 × 实际名/显示名）+ `extra_env`（自定义键值）两个 JSON 文本列，存量数据零迁移；上游 key 仍全走 Keychain 不落数据库。
- **代理默认不启动**（硬性需求 1）：`claudeProxyAutoStart` 配置默认 false，设置页「随应用启动代理」开关；`startProxy` 收敛为手动入口（Claude 页按钮 / 设置页重生成 token）。
- **settings.json 接管通道**（硬性需求 2+3）：代理启动 / 运行中切 profile → `buildProxyEnv()` 生成的托管 env 块合并写入 `~/.claude/settings.json`（BASE_URL→本地代理、AUTH_TOKEN→代理 token、模型映射、行为开关）；非 env 字段与用户自有 env 键原样保留；首份快照存 sidecar，停止代理 / 应用退出按键级还原；原子写 + 崩溃启动补还原（备份轮换不实现，降级取舍见 KNOWN_LIMITATIONS）；managed/project 层同名 env 键检测 → 警告提示不代改。

### feat（Phase D，本批）

- **Profile 编辑弹窗升级 tabs 三页**（760×600）：基础（名称/Base URL/API Key）+ 模型映射（6 槽位 × 上游模型名/显示名，显示名留空自动剥 `[1M]` 等标记，placeholder 实时渲染派生预览）+ 高级 env（键值行编辑器 + cc-switch/settings.json 粘贴导入）。
- **粘贴导入**：`parseImportedEnv()` 纯函数解析（整段 settings.json 或裸 env 对象两种形状；BASE_URL/AUTH_TOKEN 归基础页、模型键归槽位、其余进 env 表；值必须字符串、键名白名单、4KB/64 键上限）；解析后先预览（识别槽位数 / 别名映射 / 未识别键警告角标），确认才应用到表单；`AUTO_COMPACT_ENABLED` 等非官方别名自动映射官方变量。
- **接管状态可视**：设置页 + Claude 页详情面板显示「● 已接管 <settings.json 路径> / ○ 未接管」+ 上层覆盖警告行；Claude 页头部代理未启动时一次性引导条。
- **模型映射摘要**：详情面板「模型映射」字段改一行式摘要（默认模型 + 已配 N/6），列表卡片 model 行显示 default 槽位（未配回落旧 model 列）。

### test

- `parseImportedEnv` 纯函数 4 用例（用户真实模板全量分布 / 裸对象 / 非法输入整体拒绝 / API_KEY+AUTH_TOKEN 同现）。
- 无头 compose 3 用例：表单 tabs 槽位/env 填写→保存→DB 往返→重开回显（经 input.hit 的 `onTextInput` 驱动，等价真实键盘路径）；粘贴导入解析→预览→应用→保存全链路；设置页接管状态行随 startProxy/stopProxy 切换 + 覆盖警告命中。
- 回归：全量 233/233 全绿（226 基线 + 7 新增）。

### 已知边界（详见 KNOWN_LIMITATIONS）

- `CLAUDE_CONFIG_DIR` 自定义 `.claude` 目录一期不识别，只接管默认 `~/.claude`。
- 接管期间用户手工改托管键（BASE_URL/AUTH_TOKEN/模型键）会在下次同步被覆盖（托管键语义归 DevDesk）。
- env 热切换（运行中的 Claude Code 会话保存即生效）已有官方文档背书，真机验证留给用户人工点验。

## [W8] 2026-09-22 —— 设置页三项修复（下拉裁剪 / 日志保留即时生效 / token 居中）

分支 `fix/settings-ux`（`0efd83e`），链内基线 180/180（177 + 3 新增）。

### fix

- **下拉框被窗体裁剪**：诊断为 EUI-NEO dropdown 组件只向下展开——「调用日志保留」是设置页最后一张卡，1200×720 最小窗下 6 项弹层（≈216px）超出窗体下缘。EUI-NEO 侧新增 `openUp(true)` 构建器选项（负 y 偏移 + transformOrigin 锚底边，默认 false 完全向后兼容，已合入 EUI-NEO main `2d818eb`）；DevDesk 侧按字段绝对 Y + 弹层高度估算动态决定展开方向，大窗口行为不变。
- **日志保留切档即时生效**：`Database::pruneCallLogs` 逻辑提为自由函数 `core::pruneCallLogs(SqliteDb&, int)`（成员版委托，`main.cpp` 启动清理零改动）；设置页切档立即经 `core::appDatabase()` 共享连接清理，副标题反馈「已清理 N 条历史日志」，0=永久跳过并提示。另加 `DEVDESK_CONFIG_PATH` 环境变量覆盖供测试隔离真实 config.json。
- **token 文本框不居中**：代理认证 token 与笔记目录路径两处框内文本补 `verticalAlign(Center)`（W7 侧边栏同款缺陷）。

### test

- 720/1000 两窗口尺寸下拉弹层无头 layout 出界断言；切档 onClick 无头触发后 DB 行数 + 副标题断言；verticalAlign 断言。

## [W8] 2026-09-22 —— 代理安全加固（W8 期间并行任务）

分支 `fix/proxy-hardening`（`67aa88a` / `30fddc8` / `8dd4d3d`），链内基线 182/182（177 + 5 新增）。

### fix

- **错误 JSON 未转义（6 处）**：`proxy_router.cpp` 手工拼接的错误响应在 message 含引号/反斜杠/控制字符时产出非法 JSON；抽 `jsonEscape()`（RFC 8259）+ `writeErrorJson()` 统一出口，缓冲 4 处 + 流式 2 处全换。
- **请求体无上限（DoS 面）**：`Server` 构造设置 `set_payload_max_length(kMaxBodyBytes=10MB)`，httplib 读包阶段单点拦截回 413（spec §10.4 承诺），所有路由含 SSE 生效。
- **deleteProfile 孤儿密钥**：调序为先清 Keychain（失败中止、SQLite 行保留可重试）再删行；keychain 新增 `deleteSecretsNotIn(service, keep)`，`ClaudeService::cleanupOrphanKeychainEntries()` 启动时清理孤儿密钥（`claude_module` onStart 接线）。

### test

- 错误 JSON 转义往返解析（缓冲+流式）；10MB 边界通过 / 10MB+1 回 413；Keychain 删除失败中止+行保留+重试恢复；孤儿清理只删无主密钥+幂等。

## [W8] 2026-09-22 —— Todo 卡片拖拽真 drop 语义 + 拖动视觉反馈

基线：**178/178 测试全绿**（177 基线 + 1 新增纯函数用例；1 个既有拖拽用例改写为新语义）。

### feat

- **Todo 卡片拖拽重构为真 drop 语义**：旧实现 onDrag 阈值（横向拖过 60% 列宽）即落库，一次拖拽可连环触发多列移动且无中间态。新实现：`onPress` 记锚点（按下时卡片 bounds + 源列/卡序）→ `onDrag` 只更新悬停列（视觉反馈）→ `onRelease` 按指针落点一次性落库（`computeAppendPosition` 追加到目标列尾）。落点换算是纯函数 `ui::dragDropTarget`（todo_ui_logic.h）：以按下卡片 bounds 为锚，dpi 用 `bounds.width / compose 卡宽` 探测、板面原点用卡片左缘回推，同时消掉指针像素空间与 compose 空间的缩放和偏移差；列内滚动偏移一并还原（命中框跟随滚动 transform）。落点在列间隙 / 板外 / Cancel / 源列 → 取消，卡片回原位。顺带消灭旧缺陷：落库只依赖稳定 taskId，不再使用 reloadAll 后会过期的列/卡索引。
- **拖动视觉中间态**：被拖卡片整体半透明（opacity 0.45，含优先级条 / 标题 / chip / 打勾）；指针悬停列边框高亮（2px primary）。拖拽松手回落在本卡上不误开详情弹窗（onClick 抑制，运行时 onClick 先于 onRelease 派发）。
- **删除死代码 `core::drag::DragController`**：审计确认无任何调用方；其"平行拖拽框架"需要应用层指针接线，而 EUI-NEO 元素级 `onPress`/`onDrag`/`onRelease` 回调已直接提供同等数据，无法复用，删除 .h/.cpp 与 CMake 条目。

### test

- **`todo_ui_drag_hit_pure`（新增）**：`dragDropTarget` 纯函数——命中/列间隙/板外/板上下方/HiDPI（dpi=2 全坐标翻倍）/列内滚动偏移/退化输入。
- **`todo_ui_move_appends_to_end`（改写）**：真实 layout 帧驱动完整拖拽序列（press → drag → release），断言中间态（被拖卡 opacity 0.45 + 目标列 border 2px + 源列不亮）、松手落库追加列尾、拖出板外取消、落回源列取消、拖拽松手不开详情弹窗（dialog 常驻 compose，断言 panel opacity）。

## [W8] 2026-09-22 —— Markdown 左栏重构（二级目录树 + 大纲浮动面板）

最终基线：**184/184 测试全绿**（W5 后 177 基线 + 7 新增）。分支 `feat/md-tree-outline`（worktree `wt-w8-md`）。

### feat

- **`4019b85` 左栏二级目录树**：新增 `md_tree.{h,cpp}` 纯函数 `buildTreeRows`——顶层 = 一级子目录（字母序），二级 = 组内文件（mtime 倒序），根目录散文件单独一组排最后，更深层文件挂在一级目录下显示剩余子路径（二级树不递归）。文件夹可展开/收起（内存态，不持久化）；搜索时树展平为命中文件列表。用户原话：基本 UI 就是左侧目录树（二级，文件夹-文件）、右侧展示编辑区域，勾选大纲后浮动展示大纲可点击。
- **`4019b85` 大纲浮动面板**：移除左栏「文件/大纲」页签，改为顶栏 switch 开关；面板 264px、`positioned + zIndex(10)` 浮在编辑/预览区右侧，不挤压布局；沿用 Phase 1 大纲逻辑（md_line_scanner 扫标题、当前节高亮、点击跳转 InputState.cursor + followCaret）；✕ 可关闭。
- **`4019b85` 文件变化自动感知**（spec §8.1 承诺的 5 秒轮询）：`MarkdownService::pollChanged()`（轻量 mtime/size 快照比对，不读内容不写库不打日志）+ `fileMtime()`；模块层 5 秒节流轮询，有差异才 `rescan()`——新文件自动出现在树里、删除自动消失，「↻ 重扫」按钮随之移除。当前笔记被外部修改：无本地编辑 → 静默重载；有未保存编辑 → dialog 二选一（重载磁盘版本 / 保留本地；EUI-NEO toast 无按钮能力，选择类交互用 dialog）；外部删除 → toast 提示（`lastKnownMtime=-1` 标记防 5 秒重复弹，dirty 时保存会重建文件）。
- **`4019b85` 新建笔记路径校验**：`isSafeNewNotePath` 拒绝绝对路径、盘符（`X:`）、`..` 逃逸、反斜杠；模块层前置校验给友好提示，`MarkdownService::create` 同规则兜底（trust boundary 在服务层）。

### fix

- **`4019b85` 左栏/大纲行渲染 position 静默失效（W7 已知坑实锤）**：文件行/大纲行的 rect 原是叶子元素，行内 icon/name 的 `position()` 在 scrollView column 流式布局中被忽略，图标和名字会被流式排成独立行。改为整行包 `ui.stack`（行内元素 position 定位）——与 W7 Todo 列同一修法，无头 layout 级测试断言缩进与行内坐标。

### test

- md_tree 纯函数 2 例（分组/收起/深层文件）；`isSafeNewNotePath`/`create` 兜底/`pollChanged+fileMtime` 3 例；无头 compose+layout 2 例（目录树展开收起与缩进 `frame.x` 断言、大纲面板 zIndex/右侧浮层/编辑区不挤压）；既有 Phase 1 / W7 用例同步到新 UI（页签→开关、树行 id）。
- 踩坑登记：`REGISTER_TEST` 宏按逗号切参，测试体里的 vector 初始化列表会破坏宏展开（报 "passed N arguments"），需用 `push_back`。

## [W8] 2026-09-22 —— 标题栏深浅主题联动（磨砂 Phase A 先行）

最终基线：**178/178 测试全绿**（177 基线 + 1 主题联动）。

### feat

- **标题栏深浅主题联动（EUI-NEO 上游 + DevDesk 接线）**：EUI-NEO 新增 `core/platform/window_effect.{h,_win,_stub.cpp}`（`TitleBarAppearance` + `applyTitleBarAppearance`，Windows 走 `DWMWA_USE_IMMERSIVE_DARK_MODE`，Win11 22000+，老系统静默降级返回 false）、`DslAppConfig::darkTitleBar(bool)` 启动快照、`app::setTitleBarAppearance/currentTitleBarAppearance` 运行时 API（即时生效，覆盖主窗口 + 全部存活子窗口 + 之后新开子窗口）+ `examples/window_effect.cpp` 演示 + 单测，已合回 EUI-NEO main（feat 34b21f2，本地未 push）。DevDesk 侧 `core::theme` 增加变更回调（core 不依赖 EUI-NEO 的架构铁律下由 main.cpp 注入 `app::setTitleBarAppearance`），启动主题恢复与设置页运行时切换走同一路径；`dslAppConfig` 补 `.darkTitleBar(theme::current().isDark)` 快照。测试 +1（theme 变更回调 mock 断言）。设计文档：`docs/superpowers/specs/2026-09-22-eui-window-frost-design.md` Phase A。

## [W5] 2026-09-22 —— 遗留项清零（三链并行 + 集成）

最终基线：**137/137 测试全绿**（124 基线 + 6 Router payload + 2 Claude UI 冒烟 + 5 字号缩放/Router 集成）。

### feat

- **`c3901a8` 跨模块跳转负载（w5-payload 链）**：`Router::navigateTo` 增加可选 payload（默认空，既有调用零改动兼容），`consumePendingPayload(PageId)` 一次性消费语义（取出即清空、页面隔离、纯导航不清挂起负载）；Home「+ 新建笔记 / + 新建任务」带 `action:*` 负载在目标页触发既有新建弹窗，「最近编辑」行点击带 `open:<路径>` 负载直接打开指定笔记。测试 +6。
- **`843cb2c` Claude 模块操作按钮全量接线（w5-claude-ui 链）**：claude_module.cpp 从 0 个 onClick 到全量真接 `ClaudeService`——新建/编辑（dialog 表单，编辑预填、Key 留空=不换）、启用（setActiveProfile）、删除（确认 dialog + Keychain 清理）、测试连通（testConnection + toast）、代理开关（startProxy/stopProxy）。测试 +2（无头 compose 冒烟）。
- **`90d5792` 侧边栏导航 + 全局字号缩放 + UTF-8 argv + clearColor 跟随主题（w5-nav-global 链）**：main.cpp 自绘常驻侧边栏（260px，5 模块入口，当前页高亮），移除 T11 浮动齿轮桥接；`core::theme` 增加 `fontScale`（钳制 [0.5,2.0]，token 读取层统一施加、重建后重放不叠加），设置页字号档位改写全局缩放；`file_open_win.cpp` 改 `GetCommandLineW + CommandLineToArgvW` 宽字符 argv 转 UTF-8（非 ASCII 路径不再乱码，链接补 shell32）；`clearColor` 构造时从持久化主题快照（运行中切换主题窗口底色仍需重启，EUI-NEO 无运行时 API，见已知限制）。测试 +5。
- **`317bcf6` / `68182cc` / `0a20a65` 三链合并集成**：w5-claude-ui、w5-nav-global、w5-payload 依次合并回 master，合并后全量 137/137 全绿。

### docs

- **（本波收尾提交）**：KNOWN_LIMITATIONS 已修条目移入「已解决」存档小节（含修复 commit）；USER_GUIDE 导航改为侧边栏描述、代理配置流程改为全 UI 可完成；恢复计划追加 W5 段；CLAUDE.md 缺口表同步。

## [W3] 2026-09-22 —— 剩余需求（T9–T12 + P16）

最终基线：**124/124 测试全绿**（109 基线 + 2 Home + 10 Settings + 3 JSON）。

### feat

- **`883d681` P12 Home 真实数据仪表盘**：新增 `HomeService`（今日笔记 / 待办 / 逾期 / 今日 API 调用 / 最近编辑前 5，只读聚合 markdown_index / todo_tasks / claude_call_logs）；4 张统计卡 + 最近编辑列表接真数据并可点击跳转；快捷按钮接线（新建笔记 / 新建任务 / 切换 Profile，跨模块禁依赖故降级为导航）；`Database::pruneCallLogs` 启动时滚动清理调用日志；`core::Router` 增加 `registerAppRouter/appRouter()`。测试 109 → 111。
- **`a18c8a8` P13 设置页**：新增 `module_settings`——主题切换（下一帧全页生效）、字号缩放档位（85–130%，当前仅作用于设置页自身，见已知限制）、代理 token 只读显示 + 重新生成（同端口重启代理并重写 `proxy_token.txt`）、Markdown 目录只读展示、日志保留天数（0–3650，0=永久）；`SettingsStore` 读写 `%APPDATA%\DevDesk\config.json`，文件缺失 / 损坏 / 越界一律回退默认或钳制。设置入口为 main.cpp 顶层浮动齿轮按钮（应用无侧边栏）。测试 +10。
- **`750b7fa` W3 三链集成接线**：调用日志清理天数由硬编码 30 改为读 `config.json` 的 `logRetentionDays`（默认 30，0=永久时跳过清理——`pruneCallLogs(0)` 会删光日志，必须短路）。隔离 APPDATA 种子验证：retention=7 清掉 40 天前日志、retention=0 旧日志完好、Home 五项统计与种子吻合、objdump 零外部 DLL、启动冒烟五模块齐启。

### fix

- **`50ff663` 启动顺序理顺**：`startClaudeProxy()` 挪到 `getRouter()` 之前，模块 `onStart` 时 `ClaudeService::global()` 不再为 null，消除启动 WARN；代理只由 main 启动一次；删除 `proxy_router.cpp` 的 `#if 0` getHeader 死代码（全仓库零引用）。
- **`1f273f9` platform::json 实装**：`parse()` 此前是无条件返回 null 的桩、`stringify()` 一律输出 `{}`；实装 yyjson 递归解析（失败返回 JsonParseError）+ 自写 RFC 8259 序列化（yyjson 共享库被 EUI-NEO 以 `YYJSON_DISABLE_WRITER=1` 编译，writer 不可用）。测试 +3。

### docs

- **T12（本提交）**：USER_GUIDE + KNOWN_LIMITATIONS + README + CHANGELOG + P16 维护评估；恢复计划收官。

## [W2] 2026-09-21/22 —— 平台架构 + P14（T6–T8）

### feat

- **`a534f5d` Windows DPAPI keychain**：`CryptProtectData/CryptUnprotectData`（用户级，禁弹 UI）+ 密文落盘 `%APPDATA%\DevDesk\keychain\<service>\<account>.key`；T4 记录的 19 个连锁失败（keychain 2 + claude_service 10 + proxy_router 7）**全部转绿**，103 例基线全绿。

### refactor

- **`14a2373` 平台适配接口隔离**：keychain / paths / time / file_open 四族平台 API 按平台拆文件（`*_win.cpp` / `*_macos.cpp` / `*_posix.cpp`），CMake `target_sources` 按平台选择；`core::time::localTime` 统一 3 处散布的 `#ifdef`；业务代码零 `#ifdef _WIN32` / `__APPLE__`（grep `src/` *.cpp/*.h 零命中）。纯重构零行为变化（103 例 84/19 与 T4 基线一致）。

### test

- **`458cf50` P10b 流式压测**（6 例，103 → 109）：
  - 10MB 流式 e2e：44ms、1440 次 receiver 调用，全串比对 + FNV-1a 哈希 + 160 块块号三重校验一致（**吞吐实测约 240–320 MB/s**）；
  - 10MB 缓冲路径：48ms 字节一致；
  - soak 60s：1200/1200 chunk（**0% 丢失**，验收口径 < 5%），seq 严格递增；
  - soak 300s：`DEVDESK_SOAK=1` 门控，常规套件打印 `[SKIP]`；
  - 断连恢复：上游中途 `closesocket`，流以 End 干净收尾，已收字节与上游已发完全一致（直接层 + 端到端两场景）；
  - 内存采样：20 轮 × 10MB 流式，增量 12,660 KB（阈值 102,400 KB），无泄漏趋势。

### docs

- **`0a76935` W2 验收**：spec §16 P14 升必选 + 发布形态约束（单 exe 零外部 DLL）；计划文件 T7 / T8 附实测数字；CLAUDE.md 缺口表更新。

## [W1] 2026-09-21 —— 止血收编 + 发布形态（T1–T5）

### feat

- **`a1a106a` 收编 macOS 中断现场 P9/P10a**：流式代理（SSE chunk 透传）全部修改 + `streaming_proxy.*` + 3 个新测试文件 + Windows 迁移现场快照（含平台迁移现状文档与本恢复计划）。

### fix

- **`90f2d77` / `642c257` Windows 测试基建**：`filesystem::remove` 抛异常 + sqlite 句柄未关导致的套件崩溃修复，test_main 容错（单测异常只挂本用例不炸套件）；补提 claude_service / proxy_router / streaming_proxy 共 32 例。
- **`5460487` Windows 干净编译**（T2）：`json.h` 补 `<cstdint>`；platform 链接 `ws2_32/crypt32`；`CMAKE_CXX_STANDARD` 升 20（designated initializers）；`proxy_router.cpp` const 引用绑临时修复；`-Werror` 收窄到 DevDesk 自己的 target（不再压 EUI-NEO，曾产生 517 个第三方假错）；FTS5 检测补 SQLite3 库链接（修假阴性，CJK 搜索测试全过）。验收：删 `build/` 全新 configure **零 workaround flag**。
- **`ff3ce3d` / `fefc6ab` / `e4c2cb3` P10a 流式修复**（T5）：缓冲转发路径也走注入的 profileProvider；上游截断（`CURLE_PARTIAL_FILE`）按流干净结束而非报错；背压改为 write callback 内阻塞（取消不再跨线程 `curl_easy_pause`）。14 个 streaming 测试全绿。
- **`11d474c` mock 上游 SSE 服务器**：补 releaser 兜底 + `is_connection_closed` 双保险，client disconnect 用例真实可观测。

### build

- **`95c59d5` 单 exe 零外部 DLL**（T3）：`DEVDESK_STATIC`（默认 ON，仅 MinGW）——SQLite3 / CURL / OpenSSL / zlib / C++ 运行时全静态，libcurl 传递依赖闭包按 GNU ld 单遍序补齐（ssh2/nghttp2/nghttp3/brotli/zstd/…）；Release 摘除 EUI-NEO 强加的 `-Os` 规避 GCC 16.2 constexpr std::string C4 mangle bug。验收：`objdump` 仅 16 个 Windows 系统 DLL（Debug+Release 双验证）、无 MSYS2 PATH 干净目录可跑、**Release 15.4 MiB（16,152,064 字节）**。

### test

- **`8dfc8a9` 测试入口增强**：devdesk_tests 支持按子串过滤（`./devdesk_tests.exe stress`）与 `--log` Debug 级日志。

### docs

- **`3700ce1` W1 验收**：T1–T5 完成标记与实测数字归档。
