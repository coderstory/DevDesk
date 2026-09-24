# DevDesk UI 修复 + 功能批量化设计（2026-09-25）

## 范围

本 spec 覆盖一批 UI bug 修复 + 小功能，经 brainstorming 编排为 6 个可执行任务（A/E/G/H/B/F）。两个延后项单独标注：C（待用户视觉细节）、D（全量重构，architectural，另行 spec）。

**全局约束（所有任务绑定）**
- 工作树：DevDesk 主工作树（**不**开 worktree——会话已累积大量未提交改动，新 worktree 会丢）。子代理只实现+测试+报告，**不自动 commit**；commit 由用户发话分批提交。
- 平台：Windows 11 + MSYS2 MinGW64 + CMake Ninja。构建：`bash scripts/build-test-run.sh`（增量，内部处理 PATH/杀进程/启动）。
- EUI-NEO 上游优先：G/H 改 `../EUI-NEO/components/styledmenu.h`（本地 main 领先 origin），属上游能力，改完可 PR upstream；DevDesk 侧不 workaround。
- 模块隔离：`module_*` 互不依赖；`core` 不依赖 EUI-NEO/platform。改 styledmenu.h 不动 DevDesk 模块隔离。
- -Werror：删函数/数据必删干净（A 删生成器函数要全删，否则 unused 报错）。
- TDD：每个任务先写/改失败测试（RED）→ 实现 → 绿（GREEN）。UI 视觉点（字体粗细、字形）headless 测不了，标"真机点验"。
- 测试框架：自制 doctest 迷你框架（`REGISTER_TEST` + `DOCTEST_CHECK`），`./build/tests/devdesk_tests.exe <子串>` 过滤。

---

## Task A — 桌宠删皮肤（机械）

**需求**：内置皮肤只留 `robot`、`ghost`；删 `clay-cat`、`duck`、`slime`。默认皮肤改 `robot`。`tatsu-pet` 是用户自下载皮肤（`petsDir()/tatsu-pet/`，不在仓库），不受代码删除影响。

**根因/现状**：
- `src/module_pet/pet_skin.cpp` L935-939 `kBuiltinSkins[]` 注册 5 款（clay-cat/duck/slime/robot/ghost）。
- 生成器函数：`generatePlaceholderSkin`(clay-cat)、`generateDuckSkin`、`generateSlimeSkin`、`generateRobotSkin`、`generateGhostSkin`，匿名 namespace 内。删注册表项后这 3 个函数变 unused → -Werror 报错，必须连函数体一起删。
- `src/module_pet/pet_skin.h` L66 `kDefaultSkinName = "clay-cat"`（注释列 5 款）。
- `src/module_pet/pet_config.cpp` L40 `out.skinName = kDefaultSkinName`（自动跟随）。

**改法**：
1. `pet_skin.cpp`：`kBuiltinSkins` 删 clay-cat/duck/slime 三项，留 `{"robot",generateRobotSkin},{"ghost",generateGhostSkin}`。删 `generatePlaceholderSkin`/`generateDuckSkin`/`generateSlimeSkin` 三个函数体（含其上方的注释块和 JSON 字面量）。
2. `pet_skin.h`：`kDefaultSkinName` 改 `"robot"`；L66 注释改"robot + ghost"。
3. `tests/test_modules/test_pet.cpp`：`pet_builtin_skins` 用例从 5 款改 2 款——`generated.size()` 5→2、`found.size()` 5→2、`kNewBuiltinSkins[]` 改 `{"robot","ghost"}`、删 clay-cat/duck/slime 的 load/assert（L759,765,774,783-788,826,830-840 等 ~10 处）、默认皮肤断言（L326 sanitized=clay-cat → robot、L894/902/922/933/942/1253/1313 clay-cat → robot）。
4. `tests/test_modules/test_menu_beautify.cpp`：**不动**（它用任意皮肤名 `duck/slime/robot` 测菜单结构，不调 `ensureBuiltinSkins`，不依赖注册表）。

**TDD**：先改 `test_pet.cpp` 期望（RED：`pet_builtin_skins` 断言 5 款但注册表还是 5 → 改期望为 2 款后 RED，因注册表仍 5）→ 删注册表+函数（GREEN：2 款）→ 全量 ctest 绿。

**验收**：`./build/tests/devdesk_tests.exe pet` 全绿；`menuBeautify` 组全绿；启动 app 桌宠默认 robot、换肤菜单只列 robot/ghost（+本地 tatsu-pet 若有）。

---

## Task E — md 编辑器 tab 压文本（bug）

**根因**：`markdown_module.cpp` 顶层 `md` stack（compose L276 content 内调 `drawHeader` + `drawBody`）。EUI-NEO `ui.stack` 语义=子元素同位叠放、不流式挤占（见 L431 注释）。`md.header`（L321，size contentW×64，无 position）与 `md.body`（L404，size contentW×bodyH，无 position）同位叠放在原点 → body（含编辑器 input edY=48）盖住 header（0-64）→ segmented「编辑/双栏/预览」tab 压编辑器文本。

**改法**（二选一，TDD 验证取其一）：
- 方案 1（小改）：`drawBody` 的 `md.body` 加 `.position(0.0f, 64.0f + t.sizes.spacingLarge)`，把正文区下移到 header 下方。`bodyH` 已是 `contentH - 64 - spacingLarge`（L267），数学自洽。
- 方案 2（结构）：`md` 顶层 stack 改 `ui.column`（纵向流式），header 64 + body 自动堆叠。改动面大，弃。

**取方案 1**。

**TDD**：headless 测不了视觉叠放，但可断言 `md.body` 节点的 y ≥ header 高度（RED：当前 y=0 → 断言 y≥64 失败 → 改后 GREEN）。或断言 `md.editor.input` 的全局 y ≥ 64（编辑器不在 header 区内）。

**验收**：app 进 md 页，segmented tab 不再压编辑器文本，编辑器首行可见在 tab 下方。

---

## Task G — 托盘右击菜单：去粗 + 背景尺寸（bug, styledmenu.h）

**根因**：
1. 字体加粗：`styledmenu.h` L498 label `.fontWeight(400)` 已是 400（非 bold）。但 EUI-NEO 字体打包可能只含 Bold 一份字重（CLAUDE.md「fontWeight runtime 切换只打包 Bold」），400 渲染回退 Bold。**待 RED/真机确认**：先查 EUI-NEO 字体打包配置（CMake fonts），确认 Regular 字重是否打入；若缺，补打包 Regular。
2. 背景太小文字溢出：`styledmenu.h` L303-313 `panelWidth = max(220, 各行 rowNeed)`，`rowNeed = measureTextWidth(text,fontSize) + iconArea + padding`（L306-311）；`panelHeight = styledMenuPanelHeight(rows,style)`。溢出 = width 或 height 算少。**待审计** `measureTextWidth` 精度 + `styledMenuPanelHeight` 行数计数（是否漏算 GroupHeader/Separator 行）。

**改法**：
1. 字体：确认 EUI-NEO 字体打包含 Regular 字重；`styledmenu.h` label 显式用 Regular（若 fontWeight(400) 因打包失效，改用直接 Regular 字体句柄或补打包）。
2. 尺寸：审计 `styledMenuPanelHeight`——确保计入所有行（含 Separator 按 `separatorHeight`、GroupHeader 按 `rowHeight`）；`panelWidth` 的 `measureTextWidth` 加充足 padding（左右 `inset` + icon 区）。

**TDD**：
- panelWidth：构造一个长文本菜单项，断言 `panelWidth ≥ measureTextWidth(text) + padding`（RED：当前若算少 → 失败）。
- panelHeight：构造含 Separator + GroupHeader 的菜单，断言 `panelHeight == selectableRows*rowHeight + separators*separatorHeight + groupHeaders*rowHeight + 2*inset`（RED：若漏算 → 失败）。
- 字体粗细：headless 测不了，标"真机点验：托盘菜单文字非粗体"。

**验收**：托盘右击菜单文字非粗、不溢出背景、长菜单项不截断。

---

## Task H — 桌宠右击菜单：二级菜单默认折叠（behavior, styledmenu.h）

**根因**：`styledmenu.h` `flattenRecursive`（L84-106）对有 `children` 的项**总是**推 GroupHeader + 递归展平 children（L100-106）→ 换肤子组永远展开。无折叠态。本会话早先曾加 group folding 后因弄坏桌宠菜单布局 revert 了，现需重做且不重蹈。

**改法**：在 `styledMenu` 里加 group 折叠语义：
- `StyledMenuItem` 组（有 children）默认渲染为 GroupHeader 一行（不可选），右侧加展开 chevron 图标。
- 折叠态：flatten 只推 GroupHeader，不推 children。
- 展开：hover GroupHeader 或点击 chevron 切换展开态（per-group expanded set，会话内记忆）。
- 展开态：推 GroupHeader + 缩进 children（depth+1）。
- **不破坏现有布局**：`styledMenuPanelHeight`/`clampPosition` 仍按当前 flatten 行数算（折叠时行数少→面板矮，正常）。

**TDD**：
- 折叠态 flatten：构造含 1 个组（2 children）的菜单，默认 flatten 只出 GroupHeader（1 行），断言 `rows.size()==1` 且 `rows[0].kind==GroupHeader`（RED：当前展平出 3 行 → 失败）。
- 展开态：expanded set 含该组后 flatten 出 GroupHeader + 2 children（3 行）。
- 选择数学：`petMenuActionForPath` 对折叠组，path `{0}`（组头）→ None；展开后 path `{0,0}`/`{0,1}` → Skin。更新 `test_menu_beautify.cpp` 的 path 断言（折叠/展开两态）。

**验收**：桌宠右击菜单换肤项默认只显"换肤 ▸"，hover/点开才列皮肤；布局不乱（上次"离谱"不复现）。

---

## Task B — 设置 tab 化 + 日志查看（feature）

**需求**：settings 页太长，用 `components::tabs` 拆 4 tab；「日志」tab 加日志查看（读 logging.cpp 落盘文件 tail）。

**现状**：`settings_module.cpp` 现有分区：主题模式/窗口效果、字号缩放/编辑器字号、Claude 代理、Markdown 目录、日志保留天数+级别、模块管理、关于、桌宠。日志文件：`core/logging.cpp` `g_file`（ofstream，append），路径在「关于」卡已显示。

**改法**：
1. `settings_module.cpp` 外层套 `components::tabs(ui, "settings.tabs")`（已 include？确认），4 tab：
   - **通用**：主题模式 + 窗口效果 + 字号缩放 + 编辑器字号
   - **模块**：模块插拔开关 + 桌宠 + Claude 代理 + Markdown 目录
   - **日志**：日志级别 + 保留天数 + **新增日志查看**（`components::scrollView` 显示日志文件 tail ~500 行 + 刷新按钮）
   - **关于**：版本 + 配置/日志路径
2. tabs 色源走 `core::theme::componentTokens(t)`（唯一色源，禁直接 dark()）。tabs 无 `.position` → 包一层定位 stack（沿用 claude 切换器 `tabs.wrap` 写法）。
3. 日志查看：读 logging 落盘文件路径（从 logging 暴露一个 getter 或复用「关于」卡显示的路径），`scrollView` tail（不全量，防大文件卡顿）+ 刷新按钮重读。

**TDD**：
- tab 切换：点 `settings.tabs.hit.2`（日志 tab）→ 日志 pane 渲染、通用 pane 不渲染（断言 `settings.tab.log.scroll` 存在、`settings.theme.mode` 不存在）。
- 日志查看区：选日志 tab 后 `settings.tab.log.scroll` 存在 + 刷新按钮 `settings.tab.log.refresh` 存在。
- 现有各卡断言：tab 化后卡在对应 pane 内，`ui.find` 仍能找到（断言不破）。

**验收**：设置页 4 tab 切换正常；日志 tab 能看到日志 tail + 刷新；其他卡不丢。

---

## Task F — md 新建文件夹 + md 文件（feature）

**需求**：md 左栏树现在只有"+ 新建笔记"（建 md 文件）；加"新建文件夹"。

**现状**：`markdown_module.cpp` 树栏 `md.btn.new`（L379）建笔记；`md_service` 有文件 CRUD，目录创建 API 待确认/补。

**改法**：
1. `md_service`：补 `createFolder(parentPath, name)` API（`core::Result<path>`，已存在→ fail）。
2. `markdown_module.cpp` 树栏：`md.btn.new` 旁加 `md.btn.newfolder`（或并入 + 按钮菜单二选一），弹 dialog 输入文件夹名 → 调 `createFolder` → 树刷新。
3. 树视图：文件夹节点可展开/收起（若树已支持目录则复用；否则补）。

**TDD**：
- service：`createFolder` 已存在→fail；新建→目录存在 + 树能列出。
- UI：点 `md.btn.newfolder` → dialog → 确认 → 新文件夹节点出现在树（headless 断言节点存在）。

**验收**：左栏能新建文件夹和 md 文件，树正确显示。

---

## 延后项

- **C — todo 列头 icon "没居中"**：defer。代码层按钮垂直/水平都居中（列头 56 中心 28=按钮中心 28，button 组件内部居中图标），嫌疑是 `0xf044`(fa-edit) 字形不对称。待用户给视觉细节（水平偏/垂直偏/字形歪）或截图再定点修。
- **D — 全量项目梳理 + 可维护性 + OOP/设计模式**：architectural，**另行 brainstorming + spec**（一个 spec 装不下，按模块/分层拆子项目）。本批不做。倾向增量改（每块测试绿），不 big-bang；设计模式按实际痛点引入，不 cargo-cult。

---

## 执行顺序

A → E → G → H → B → F（先清活 bug A/E/G/H，再做 feature B/F）。C 待细节，D 另行。

## 开发方法

subagent-driven-development（每任务 1 实现子代理 + 任务审查；机械 A 单代理批量；模型 A=haiku、E/G/H=sonnet、B/F=sonnet）。TDD 每任务 RED→GREEN。ledger 文件记进度。主工作树、不自动 commit。

---

## 审计裁决与修订（2026-09-25，独立审计后）

独立 skeptic 审计对照源码逐条证伪，原 spec 有多处根因/前提错误。以下裁决**取代**原各任务段的根因与修法，开发以本段为准。

### 全局修订
- 工作树已 clean（会话改动已提交 c0518cc on `feat/ui-fix-batch-2026-09-25`）。原"大量未提交改动"前提作废。**SDD 允许逐任务 commit**（tree 干净，BASE..HEAD review-package 有 diff）；不开新 worktree，在本分支跑。
- EUI-NEO 无 CMake 字体打包配置（原 G 前提错）；字体靠 `text.cpp` 运行时找 assets。

### Task A — 修订（可开发）
- **必加一步**：`ensureDefaultSkin`（pet_skin.cpp:965）内部调 `generatePlaceholderSkin`，且是 pet_window.cpp:599 兜底 + 8 处测试（L265/281/714/727/739/750/894/922）的活路径。**删 generatePlaceholderSkin 前先把它重定向到 `generateRobotSkin`**（保持兜底语义），否则 undeclared identifier 编译断。
- **删除范围全列**（连私有 helper + 色常量，否则 -Wunused-function + -Werror）：`kClayBody`(143)、`drawCatFrame`(236-265)、`generatePlaceholderSkin`(302-364)、`kDuckBody/kDuckWing/kDuckBeak`(366-368)、`drawDuckFrame`(371-399)、`generateDuckSkin`(401-457)、`kSlimeBody/kSlimeShine`(459-460)、`drawSlimeFrame`(464-492)、`generateSlimeSkin`(494-615)。保留 robot(617-712)/ghost(714-775) 及共用 `Eyes/Mouth/fillRect/drawZ/writeFrame/Canvas/kInk`。
- **行号订正**：`kDefaultSkinName` 在 `pet_skin.h:27`（非 L66）；L8 头注释同样列 5 款需改。
- **测试补漏**：`test_pet.cpp:368` `exists(petsRoot/"clay-cat"/"pet.json")` 删注册表后必红，要改；L14 文件头注释；L1313 用符号 `kDefaultSkinName` 无需改（原列入冗余）。
- **验收订正**：仓库自带 `assets/pets/anime-girl` 真实美术皮肤，被 `discoverSkins()`（pet_skin.cpp:1000-1003）发现 → 桌宠菜单实际列 `robot/ghost/anime-girl`（+本地 tatsu-pet 若有），**不是**"只列 robot/ghost"。

### Task E — 重开（不可盲开发）
- **根因 REFUTED**：`md.root` 是 `ui.column`（markdown_module.cpp:276），column 流式排布，header 在 body 上方，不叠放；`edY=48` 是 `md.editor` stack（L886）内部坐标，与 header 不同空间。原修法（md.body 加 .position）会被 `layoutColumn`（layout.h:596-611，不读 hasX_/x_/y_）静默忽略=空操作。
- **需用户取证**：截图或确认"tab"具体指哪个控件（候选：header 的 `md.mode` segmented、编辑器 stack 内标签 chip 行 `md.tags.chip.*`、`drawOutlinePanel` 浮动面板 L283）+ 压的是哪段文本。**未取证不开发**（同 C）。

### Task G — 重开（部分可开发）
- **panelHeight 不漏算**（REFUTED）：`styledMenuPanelHeight`(L193-199) 遍历全 rows，Separator 按 separatorHeight、GroupHeader 按 rowHeight，已被 test_menu_beautify:182-192 锁。
- **panelWidth 预算自洽**（绘制盒宽 ≥ textW，测量/渲染权重同 400）——看不出溢出来源。**需真机取证**：在 showStyledMenu 临时打 panelWidth/panelHeight/rows + 实测 label 宽，定位真因。
- **TDD 前置**：panelWidth 只在 `showStyledMenu` 内部算（L303-312，会 openWindow 开真窗，headless 不可达）。要 TDD 必先把宽度计算提取为导出纯函数 `styledMenuPanelWidth(rows, style)`，再断言 `≥ measureTextWidth + padding`。
- **字体粗（原机制写错）**：打包默认 UI 字体是 `JingNanJunJunTi-Bold`（text.cpp:37），CJK 字形恒从该 Bold 文件取（fallback 链 L762 首位），与 fontWeight 无关——**全局效应**。需先回答"为什么只有菜单看起来粗"（或主窗口中文其实也粗、用户先注意到菜单）。杠杆：改 fallback 顺序 / DevDesk 调 `TextPrimitive::setDefaultFontFiles()` / 改默认字体。**未取证不开发字体部分**。

### Task H — 修订（可开发，但有上次同源风险）
- **窗口尺寸定死**：`showStyledMenu` openWindow 时 `.windowSize(...).resizable(false)`（L365-374），`window_backend.h` 无 size setter → hover 展开子行画到窗外（上次"离谱"同因）。**修法必须**：展开 = `requestWindowClose` + 按新 rows 重算 panelHeight/clampPosition 重开；**或**开窗时按全展开预留高度（复用 `petMenuFullHeight` pet_window.cpp:179-186 的"宁高勿叠"语义）。spec 取后者更稳。
- **GroupHeader 无交互面**（L451 只 Item 挂 hit/onClick，且 onClick=requestDismiss）：要给 GroupHeader 造一条**不 dismiss** 的 hit 路径切展开态。
- **折叠→selectableCount==0 降级**（L130-138 只数 Item；L291 无可选→return false→降级原生，pet_window.cpp:297）：折叠态必须保证至少 1 可选行，或允许组头可选。
- **上游向后兼容**：`styledMenuFlatten` 默认行为变"默认折叠"破坏上游 API（EUI-NEO 约定 + DevDesk CLAUDE.md PR 清单要求不破坏）。**改 opt-in**：`StyledMenuItem` 加 `bool collapsedByDefault` 或 flatten 增 `ExpandedSet` 入参，旧签名/旧行为保持。
- **既有测试 4 条会红**（test_menu_beautify:165/173/178/189-190）：折叠默认化让 rows==6 等断言失败 → 改两态（折叠/展开）重写。

### Task B — 修订（可开发）
- **TDD 去自相矛盾**：原"点日志 tab→settings.theme.mode 不存在"+"现有各卡 ui.find 仍能找到"互斥。改：`test_settings.cpp:413-421` 现一次 compose 断多卡 → 改 per-tab（先 `selected=2` 切日志 tab，再断言日志卡存在、通用卡不存在）。
- **真实 id**：`settings.theme.mode` 是臆想，真名 `settings.card.appearance.theme.dd`（settings_module.cpp:748）。
- **contentKey 纳 tab 索引**（settings_module.cpp:579-582 现 key=代理卡高+C/M/p 显隐）：tab 化后切 tab 沿用上 tab 内容高→滚动错，key 加 `activeTab_`。
- **日志卡 openUp 游标**（L615-618 传 cardsTopY+cardY-offset，cardY 是 L591-622 硬编码卡高手算游标）：按 tab 过滤卡片后游标序列变，下拉方向会错，需重算。
- **日志读盘移出 compose**：scrollView content lambda 被 measure/paint 多次调；切日志 tab / 点刷新时读一次存成员变量，compose 只渲染字符串。
- **日志路径**：直接用 `core::paths::logFile()`（settings_module.cpp:1572 已在用），不在 logging.cpp 加 getter。
- **补 include** `components/tabs.h`（settings_module.cpp:86-92 现无）。

### Task F — 作废
- **已实现并已提交**（commit 393770c）：`createFolder`（markdown_service.h:146）、`md.tree.newfolder` 按钮（markdown_module.cpp:597）、`drawNewFolderDialog`（1673）、树文件夹展开/收起（664/687-718）、测试（test_markdown.cpp:615/195/227/256/1443）全在 HEAD。原 spec 前提"只有+新建笔记"为假。**整节作废**。
- **唯一可能真缺口**：`createFolder(name)` 只建一级目录，二级子目录创建缺失。**待用户确认是否要二级目录创建**；要则另立小任务。

### 修订后可开发顺序
A、B、H 可立即开发（SDD+TDD）。E、G 待取证（E 需截图/确认控件，G 需真机打日志或字体对照）。F 作废（待用户确认二级目录）。C、D 不变。
