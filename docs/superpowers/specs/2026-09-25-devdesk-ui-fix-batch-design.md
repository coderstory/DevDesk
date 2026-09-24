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
