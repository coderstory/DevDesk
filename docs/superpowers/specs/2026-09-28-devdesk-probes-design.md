# DevDesk probes 测试方案（真窗 + Runtime 注入）

日期：2026-09-28 ｜ 状态：待审计 ｜ 范围：**只建测试基建，不改任何功能代码**

## §1 现状取证

### 1.1 DevDesk 无头 UI 测试（现状缺口）

- `tests/CMakeLists.txt` 单二进制 `devdesk_tests`（tests/CMakeLists.txt:1-60），自制 87 行迷你 doctest（`REGISTER_TEST` + fail-fast，用例总数以运行输出 Total 为准，现 646）；TIN-10 用例登记自检（tests/CMakeLists.txt:62-116）要求磁盘实况 == 登记表，否则 configure 期 FATAL。
- 无头模式：`ui.begin(pageId); mod->compose(ui, screen); ui.end(); ui.layout(w,h);` 然后 `ui.find(id)->frame` / 直接调 `onContextMenu(pe, rect)` 触发再重 compose（test_ui_modern.cpp:93-106、test_settings.cpp:724-777）。
- 无头能断言的：元素存在/几何/frame 边界（如 `settings_log_dropdown_popup_stays_in_window` 断言 popup frame ⊂ 窗、openUp 行为，test_settings.cpp:752-772）。
- 无头测不到的：真实窗口、Runtime 事件分发（当前是直接调回调，绕开命中/捕获/焦点链）、真实渲染、**交叉 z 序碰撞**（无头只查单一元素 frame，不扫「弹层 vs 非祖先其余元素」的相交判定——§2.2 弹层锚定变体）。

### 1.2 EUI-NEO probes 模式（可移植范式）

`EUI-NEO/tests/probes/*.cpp`，每个 probe 一个真窗程序 + 自动断言：

- **开窗**：`initializeWindowBackend()`（glfwInit/SDL_Init 按宏二选一，input_runtime_probe.cpp:30-37）→ `WindowCreateRequest{200,160,...}` → `core::window::createWindow`（:54-64）。
- **Runtime + compose**：`core::dsl::Runtime runtime; runtime.initialize(window); runtime.compose(pageId, w, h, [&](Ui& ui, Screen&){...})`（:75-78）。compose lambda 里可捕获 `Ui*` 留作断言句柄（`editorUi = &ui`，:179-183）。
- **注入输入**：`core::queuePointerButton / queuePointerMotion / queueScrollInput / queueKeyInput / queueTextInput`（core/input/input_state.h:199-241）→ `runtime.update(window, dt, 1.f, 1.f)` 驱动一帧（:125）→ 断言计数器/状态/元素 frame（:157-159、:205-238）。
- **结果判定**：`passed` 布尔累积，`std::cerr` 明细，退出码非 0（:281）；辅助 `--frames N` / `--manual` 参数（window_only_probe.cpp:36-47）。
- **渲染/像素级（更重的一档）**：render_clear/render_context_probe 走 `createRenderBackend + beginFrame/clear/present` 真画几帧；gpu_image_probe.cpp:250-279 `checkWindowPixel` 用 Win32 `PrintWindow+GetPixel`（Vulkan）或 `glReadPixels`（OpenGL，:324-327）做真窗像素断言。
- **CMake 注册**（EUI-NEO/CMakeLists.txt:688-717）：`EUI_BUILD_TEST_FIXTURES` 门控 + `enable_testing()` → `file(GLOB TEST_PROBE_SOURCES .../tests/probes/*.cpp)` → 每个 probe `add_executable` + link `eui::neo` + `eui_apply_compile_options` + `eui_neo_copy_assets/dependencies` + `add_test(NAME <probe> COMMAND <probe> --frames 3)` + `LABELS probe`。

### 1.3 z 序引擎事实（probe 断言的判据来源）

- 兄弟排序用 `subtreeMaxZIndex` 上界 + **稳定排序**（`std::stable_sort(orderedChildren..., subtreeMaxZIndex)`，core/dsl.h:1752-1778）——**等值 z 时保持 DOM 序，后绘制者盖先绘制者**；弹层逃逸靠子树取上界（:1775 注释「弹层逃逸」）。
- 即：等值 z + frame 相交 = 谁后 compose 谁盖谁，是确定性可断言行为，不是渲染玄学。
- dropdown 的 `.zIndex(zIndex_)` 落在**卡根 stack** 上（components/dropdown.h:82 setter、:105-107 `ui_.stack(id_).zIndex(zIndex_)`），弹层（`.popup` stack，dropdown.h:145）是卡子树内的子层——**抬的是整卡**：高 z 卡连同其弹层按子树取上界整体后置（core/dsl.h:1774 注释「弹层逃逸」+ :1775-1778 稳定排序），不是弹层元素自身抬 z（弹层自身 z0）。DevDesk settings 侧被抬到 40/30 两档（见 §3）；「弹层 z」的精确判据 = §2.2 定义的弹层链上界。

## §2 方案：DevDesk probes 架构

### 2.1 形态

与 EUI-NEO probe 同构，唯一区别是 compose 主体换成 DevDesk 模块：

```
init backend → createWindow(真实尺寸) → Runtime r → r.initialize(window)
→ r.compose(pageId, w, h, [&](Ui& ui, Screen& screen){
      screen.width/height 取真实窗逻辑尺寸;
      mod->compose(ui, screen);          // 模块 compose 是 (状态, screen) 纯函数
      capturedUi = &ui; })               // 断言句柄（EUI-NEO 惯例 input_runtime_probe.cpp:179）
→ 每帧循环：queue 输入 → r.update(...) 真分发一帧 → 立即重调 r.compose(...)（update 只派发事件不重建树，弹层靠 recompose 进/出 DOM，见 §4.2）→ 断言 → 清理 → 退出码
```

可行性依据：DevDesk 模块工厂 `create_<mod>_module()` 已导出（src/app_registry.h:22-25），无头测试已证明 `mod->compose(ui, screen)` 无需窗口可跑（module_interface.h:51）；probe 只是给它接上真窗 + Runtime 输入分发 + 交叉 z 序扫描。

### 2.2 三层断言机制

| 层 | 断言什么 | 实现 | 对应 bug 类 |
|----|----------|------|-------------|
| L1 frame/几何 | 元素 frame、包含关系、⊂ 窗/⊂ 父容器、多窗口尺寸矩阵 | `capturedUi->find(id)->frame`（无头同款 API） | 溢出、内边距、弹窗越界 |
| L2 z 碰撞扫描（弹层锚定） | 弹层 vs 非祖先其余元素相交时的高盖低不变量 | 与弹层 frame 相交的每个非祖先元素，其 `subtreeMaxZIndex` 必须 < **弹层链上界**（定义见下）；≥ → FAIL 打印 id 对与双方 z（判定式详见 §4.4b） | dropdown z 序、弹窗互盖 |
| L3 像素 | 真窗像素采样（颜色/曲线可见） | `glReadPixels` / Win32 `PrintWindow+GetPixel`（gpu_image_probe.cpp:250-279 惯例） | 双底色取证、sparkline 可见性（可选档） |

L1/L2 是安静的（不依赖像素时序）；L3 走 RenderBackend 真渲染，仅按需启用（`--pixels` flag），不进默认 `--frames 3` 快档。

**「弹层链上界」定义**（§4.4b L2 判定式的唯一基准）：弹层链上界 = max(弹层子树的 `subtreeMaxZIndex`, 弹层元素→根祖先链上每元素 `zIndex` 的最大值)。场景中即：外观卡 40 / 编辑器字号卡 30（revert 后均 30）——「弹层 z」不是弹层元素自身 zIndex（弹层自身 z0，被卡根 stack 抬层）。Element 无 parent 指针（core/dsl.h:106 起 Element 结构体只持 children），probe 须在 compose 后**自建父指针表**（单遍遍历 Ui 树 children 生成 id→父 映射），才能沿祖先链求值上界。

> **作用域注（为什么不是通用规则）**：L2 只在弹层锚定下成立，**不是「任意两元素 frame 相交 + 等值 z → FAIL」的通用不变式**。引擎对等值 z 只保证稳定排序下后绘盖先绘（core/dsl.h:1774-1778），卡 bg（z0）与卡内容（z0）等值相交是**有意设计**（内容必须画在衬底上）——逐对通用扫描必然误报，且「谁对谁错」无从判定。故 harness 的 `assertNoEqualZOverlap` 契约只比较「弹层 vs 非祖先其余元素」，互为底层/背景的配对（两个都不是弹层的元素）永不进入比较。

### 2.3 probe harness（自含，不依赖 EUI-NEO tests 源码）

- 新建 `DevDesk/tests/probes/probe_harness.h`（header-only，~100 行）：backend init/terminate、`--frames/--manual` 参数解析、pollOnce、`makeWindow(size)`、`composeModuleFrame(mod, w, h)` 单帧助手（update → recompose 成对，见 §4.2）、L2 扫描 `assertNoEqualZOverlap(ui, popupElement)`——契约只做「弹层 vs 非祖先其余元素」比较（弹层锚定，§2.2 作用域注），内部自建父指针表按 §2.2 定义求弹层链上界、跳过不可见元素（opacity 0）。
- **不** include EUI-NEO/tests/probes/*（该目录在 `EUI_BUILD_TEST_FIXTURES` 门内，且 DevDesk 以 `EXCLUDE_FROM_ALL` 挂入子目录（CMakeLists.txt:91），不构成可依赖目标）。仅依赖 EUI-NEO 公共头（window_backend/dsl_runtime/input_state），DevDesk 已 link `eui::neo`、`devdesk_add_eui_includes` 已就位（CMakeLists.txt:106-113）。
- 上提共享 harness 到 EUI-NEO（跨仓库改动）记 follow-up，不进本轮。

## §3 probe 清单（按用户本批 bug 逐类判定）

| # | bug 类 | 定位证据 | 该不该 probe | 断言（层） | 优先级 |
|---|--------|----------|--------------|------------|--------|
| 1 | **dropdown z 序碰撞**（settings 外观卡 vs 编辑器字号卡等值 z） | settings_ui_cards.cpp:63-69（zIndex(40) 抬层注释 + 回归 1379cc1：等值 z30 时后绘的编辑器字号卡盖住外观卡弹层）；引擎判据 dsl.h:1752-1778 | ✅ 必须 | L2：每开一个下拉，与 popup frame 相交的每个非祖先元素，其 `subtreeMaxZIndex` 必须 < **弹层链上界**（§2.2 定义）——≥ 即 FAIL（注释档 40 处上界 40 > 字号卡 30 绿；revert 回 30 后等值必须红）；L1：popup ⊂ 窗（无头同款：test_modules/test_settings.cpp:752-754） | **P0 = MVP** |
| 2 | **卡片几何溢出**（profile 详情非最大化溢出卡底） | profile-run 计划人工项：最小窗 1200×720 下文案/表完整无重叠（2026-09-28-profile-run-plan.md:1661）；profile 卡列表 claude_ui_list.cpp:400-475 | ✅ 必须 | L1：dialog/卡内子树 frame ⊆ 卡 frame ⊆ 窗；尺寸矩阵 {1200×720 最小, 1600×1000 默认} 两组各跑一遍 | P1 |
| 3 | **sparkline 渲染错位**（lineChart 32px chromeless 几何） | home_module.cpp:472-491（`home.stat.tokens.spark` lineChart chromeless）；组件几何 linechart.h:92-95（chromeless plot = 全盒）；空 values 会填 demo 数据（linechart.h:85-86，D-6 护栏 :472-478 已防空） | ✅（结构档）+ ⚠️ 组件级归上游 | L1：sparkline 根元素 frame == 调用点指定 rect；曲线点 x∈[0,chartW]、y∈[0,chartH]（扫描 chart 内部几何点）；传空 series 无 demo 数据泄漏（防 D-6 回归）。**组件级 demo-fill 断言放 EUI-NEO 侧 probe 更合适**，本方案只锁 DevDesk 侧接线 | P1 |
| 4 | **双底色**（profile 列表 card 套 bg） | claude_ui_list.cpp:422-430（2026-09-28 已修：去 card 组件骨架，仅留 `.bg` 唯一底，注释直引"用户反馈"） | ✅（回归防护） | L1 结构断言：卡片 rect 内恰好一层不透明 surface 底（遍历 frame 相等的 rect 底元素计数 == 1）；L3 像素可选取证「底层是 surface 非白底+阴影」 | P2 |
| 5 | **文本溢出弹窗**（长文案致弹出层越界/截断） | 托盘菜单 panel 尺寸预算审计（2026-09-25-devdesk-ui-fix-batch-design.md:60 待真机取证）；弹层越界已有无头同款可仿（test_settings.cpp:724-777 openUp） | 部分 | 越界半可 probe：L1 styledmenu/弹层 frame ⊆ screen；「截断观感」（省略号/滚动提示）属视觉判断 → 不 probe，留 check-consistency + 人工 | P2 |
| 6 | **卡片内边距/居中** | 与视觉 token 强耦合（示意间距 8px 网格、卡 padding，设计 spec §6.5） | 弱 | 只对**已修尺寸的回归位**做数值断言（同款：settings_path_text_vertically_centered，test_settings.cpp:781）；不新建纯审美断言 | P3 |

**不适合 probe**（明确划出）：纯颜色观感（色源对错的审美判断）、截断省略号样式、动画/拖拽手感 → check-consistency.sh（截图合成）+ 人工清单（2026-09-28-manual-verification-checklist.md 模式）。

## §3-probe-full 全 bug 类 probe 设计（综合 UI 审计 41 findings → 12 类 + 附 1）

数据源：2026-09-28 综合 UI 审计 6 页并行，41 findings（home 7 / markdown 4 / todo 9 / claude 6 / settings 9 / pet+sidebar 6）。每条根因与定位证据直接采信审计（file:行号）；本节只补「probe 怎么自动断言」。

**总规则（每类搭法共性，下文只写差异）**：骨架一律 = 真窗（§2.1 harness：backend init → createWindow → Runtime → 每帧 update+recompose 成对）+ CONFIG 隔离（`test_support::ScopedConfigPath`，与真机配置无关）+ 锚点 id 走 compose 后 `capturedUi->find(id)` 读 Element 字段（frame / opacity / hoverColor / border / shadow，均为 dsl.h 既有字段）。**全部断言走 L1/L2（frame/字段/状态），不进 `--pixels` 快档**；`elementStructure_` 比较与 `paintRequested_` 由 runtime 内部完成（runtime_lifecycle.h:59-63、:154-156），probe 直接读 `runtime.paintRequested()` / `composeRequested()`（:163-169）。

### 类 1 z-collision（2 组）

| 项 | 内容 |
|---|---|
| bug + 根因 | (a) settings 跨卡 z 碰撞（外观卡 40 vs 编辑器字号卡 30；settings_ui_cards.cpp:63-69，引擎判据 dsl.h:1752-1778）——**已有 MVP（§4），本节不重述**；(b) home Token 卡 sparkline 以 (0,0) 构建叠 label 文字带——chart lambda 未给 x/y，chromeless 下 plot 区 = 整盒（home_module.cpp:133；linechart.h:92-95） |
| 断言 | (b) L1：`home.stat.tokens.spark` 根 frame 与 `home.stat.tokens.*.label` text frame **不相交**（gap ≥ 2px；修复后 chart 进独立条带）；曲线多边形元素（id `home.stat.tokens.spark.segment.curve.N`，linechart.h:159-172）frame 亦不相交。demo-fill 防回归：注入空 series 时元素树曲线段数为 0（D-6 护栏 linechart.h:85-86 兜底 {0.0f}，span=0 不产 segment——断言 `find("...segment.curve.0") == nullptr`） |
| 文件 | (b) `home_probes.cpp` §A（新）；（a）`settings_dropdown_z_probe.cpp`（MVP） |
| 搭法 | home 页：CONFIG 隔离（`ScopedConfigPath`）→ `create_home_module()` + onStart → 开窗 1200×1000 → 空转 1 帧 → 读 spark 根与 label |
| 优先级 / 适合 | P1 / ✅ 必须 |

### 类 2 hover-flicker（3 组，1 组弱）

| 项 | 内容 |
|---|---|
| bug + 根因 | (a) Token 卡 0.5s onTimer 驱动运行时全局 composeRequested_+paintRequested_（home_module.cpp:128；runtime_update.h:400-404）→ 整页 2Hz 重绘、hover 0.1s 过渡反复打断（用户报「5 统计卡 hover 闪连带上个卡」）；(b) 浅色档 hover token(#F5F2EC) == surface(#F5F2EC)（theme.cpp:59,61）→ hover 反馈不可见；(c) 标签 chip × 热区 hoverColor 与 chip 主体无 hoverColor 两截色（markdown_module.cpp:1287-1294，弱） |
| 断言 | (a) **空转重绘检测**：无输入注入、固定窗，连跑 60 帧（≈2s @ 30fps），计数 `runtime.update(...)` 返回 true（= paintRequested_，runtime_lifecycle.h:154-156）的帧数 ≤ 1（静止 UI 结构不变不应请求重绘；修复前 onTimer 每 0.5s 请求一次 → 红）；(b) token 断言：读 `core::theme` 浅色档 tokens，`colors.hover != colors.surface`（组件卡 .press hover 态 home_module.cpp:81-83 与卡底 color 源同一 tokens）——该断言无头即可做（tests 侧同判据），probe 侧以「compose 后卡 rect 元素 `hoverColor != color`」声明值复锁；(c) L1 字段：chip 主体 `.bg` 元素 `hoverColor != nullptr`（有）且与 `.del` 的 `hoverColor` 相等（修复后统一） |
| 文件 | `home_probes.cpp` §B（a/b）；`markdown_probes.cpp` §C（c） |
| 搭法 | (a) home compose 后不注入任何输入跑 60 帧计数；(c) markdown 编辑态 + 一页带标签文档 |
| 优先级 / 适合 | (a)(b) P1 / ✅；(c) P3 / ⚠️ 弱——观感主体留人工，probe 只锁字段一致性 |

### 类 3 double-bg（3 组）

| 项 | 内容 |
|---|---|
| bug + 根因 | (a) todo 卡 components::card 底被卡内 .bg 不透明白色整层覆盖（todo_ui_board.cpp:453）；(b) markdown 编辑器列壳 bg + input 整面 surface 双层近白卡（markdown_module.cpp:1033）；(c) home barChart 无 chromeless 自画面板成「卡中卡」（home_module.cpp:565；barchart.h:96-102） |
| 断言 | (a)(b) **同 frame 底计数**：遍历元素树，取 frame 与卡 frame **相等**（±1px）且 `opacity >= 0.99` 的 bg 类元素（id 含 `.bg` 或组件卡根背景）计数 ≤ 1；(c) barchart 嵌入卡时其根（`home.api.chart` 内 bar 面板）`bg` 元素不存在（chromeless 修复后），或内层面板底 `color == cardsurface` 时仍允许（二选一以修复落点为准） |
| 文件 | `todo_probes.cpp` §D（a）；`markdown_probes.cpp` §A（b）；`home_probes.cpp` §C（c） |
| 搭法 | (a) todo 页 1 列 2 卡；(b) markdown 编辑态单栏；(c) home 页 API 卡空转 1 帧 |
| 优先级 / 适合 | P2 / ✅ 必须（结构计数，无像素时序） |

### 类 4 component-misuse（2 组）

| 项 | 内容 |
|---|---|
| bug + 根因 | (a) home barChart 128px wrap → plotHeight=16px、8px 地板假条、条形区与轴标签间 35px 空带（home_module.cpp:541；barchart.h:84-89、:128 `max(8, v*plotHeight)`）；(b) settings 三个按钮只 .colors() 不 .theme() → EUI-NEO 默认深色主题泄漏：#3871E0 蓝描边 + 黑 0.22 重阴影（settings_ui_cards.cpp:647「应用」、:533「打开目录」、:938「刷新」） |
| 断言 | (a) L1：所有 `home.api.chart.bar.N` 元素的 y∈[plotY, bottomY] 且 bar 区总高 plotY..bottomY ≥ 36px（修复后 chartCardH 提升）；注入全零 values 时 bar 元素数 == 0（修掉 8px 地板条）或全零高度；(b) L1 字段：按钮元素 `border` 颜色 == `t.colors.primary`（#D97757 暖）非 EUI 默认蓝 #3871E0，`shadow` alpha ≤ 0.10（浅色档 token）非 0.22（元素字段 border/shadow，dsl.h:882-915） |
| 文件 | `home_probes.cpp` §D（a）；`settings_cards_probes.cpp` §C（b） |
| 搭法 | (b) settings 页 9 卡 + 深浅两主题各跑一遍（切主题 = 模块 onStart 配置或 api 注入） |
| 优先级 / 适合 | P1（a）/ P2（b）/ ✅ 必须 |

### 类 5 padding-centering（2 组）

| 项 | 内容 |
|---|---|
| bug + 根因 | (a) home API 分布卡 title/wrap 在已含 inset 的 content 坐标里二次 position(16,..) → 左 32 右 0（home_module.cpp:547；card.h:102-106 content 已含 inset）；(b) settings 代理开关 wrap y=12、刷新按钮 y=14 欠顶边距口径 16（settings_ui_cards.cpp:592、:939） |
| 断言 | (a) **对称断言**：`home.api.chart` 卡内 title/wrap frame 左右间隙相等（`title.x - card.x == card.x+card.w - (title.x+title.w)`，±1px）；(b) 开关 wrap 顶 y ≥ 15（修复 16；当前 12 红） |
| 文件 | `home_probes.cpp` §E（a）；`settings_cards_probes.cpp` §D（b） |
| 搭法 | 同各页骨架；无需特殊状态 |
| 优先级 / 适合 | P3 / ✅（数值断言；不建纯审美断言） |

### 类 6 text-overflow（4 组）

| 项 | 内容 |
|---|---|
| bug + 根因 | (a) home 统计数字 48px（~27px/位仅容 5 位）+ badge（154.85px > 134 可用）无 clip 无 wrap 溢出压邻卡（home_module.cpp:100；dsl.h:124 clip 默认 false、:149 wrap 默认 false）；(b) claude 删除确认弹窗正文固定高 60，>~50 CJK 字名换行到第 4 行与按钮行重叠（claude_ui_form.cpp:1030）；(c) 共享配置绑定行文本溢出 pane 右缘（claude_ui_detail.cpp:113）；(d) 窗口效果降级提示行窄窗可用宽 224px < 文案 275px（settings_ui_cards.cpp:215） |
| 断言 | (a) **最小窗矩阵** 1200×720：填 6 位+数字与长 badge 文案 → `home.stat.*.number` / `.badge` text frame ⊆ 所在卡 frame；dirty 数据枚举（若模块探针可注入 stats 快照；否则用最大字面量档）。(b) 构造长名 profile（≥60 CJK 字）开删除弹窗 → 正文 text frame.bottom ≤ 按钮行 frame.top；(c) 长片段名 → 文本 frame.right ≤ pane frame.right；(d) 窄卡宽（最小窗）→ 降级提示 text frame ⊆ 卡 frame |
| 文件 | `home_probes.cpp` §F（a）；`claude_probes.cpp` §C（b/c）；`settings_cards_probes.cpp` §E（d） |
| 搭法 | (a)/(b) 需构造数据：home 用模块 stats 注入（探针直连 `core::proxy_stats` 写入）；claude 经 `ClaudeService` 测试造数路径（与无头同款）；(d) 空转即可 |
| 优先级 / 适合 | P1（a）/ P2（b、c、d）/ ✅ 必须 |

### 类 7 layout-overflow（2 组）

| 项 | 内容 |
|---|---|
| bug + 根因 | (a) todo 列宽无下限无横向滚动，≥6 列时固定宽 pill/chip 越出卡边、列头挤压（todo_ui_board.cpp:218）；(b) claude 共享配置区块（粘性脚注）整段盖列表 virtualList 视口底带，≥5 profile 时末卡永不可见、点击误开片段编辑（claude_ui_list.cpp:486；视口用 scrollH L404-406 vs listH L485-487） |
| 断言 | (a) **最小窗矩阵** {1200×720} + 造 8 列数据：每列卡内优先级 pill / 截止 chip frame ⊆ 卡 frame；列头标题 text frame ⊆ 列头（修复后 colW ≥ 180 或横向滚动）；(b) 造 5 profile + ≥2 共享片段：virtualList 视口 frame.bottom ≤ 共享区块 frame.top（修复后 listH 让位）；「滚到底」序列后末张卡 frame ⊆ 区块上方区域 |
| 文件 | `todo_probes.cpp` §B（a）；`claude_probes.cpp` §A（b） |
| 搭法 | 造数：(a) todo 8 列经模块 DB 测试造数路径；(b) claude 5 profile + 片段（复用无头造数，test_claude_ui 同款） |
| 优先级 / 适合 | P2（a）/ **P1**（b，major 且点击语义也错）/ ✅ 必须 |

### 类 8 truncation（5 组）

| 项 | 内容 |
|---|---|
| bug + 根因 | (a) markdown 状态栏左区窄窗仅 44px，文件名+保存态被 .clip() 裁不可读（markdown_module.cpp:1365）；(b) todo 逾期弹窗「逾期N天」固定 48px 宽截断（todo_ui_dialogs.cpp:541）；(c) 详情弹窗标签放不下静默断行无 +N（todo_ui_dialogs.cpp:769）；(d) claude Profile 卡 name/URL/model 不裁剪被徽标/角标盖、调用记录 mp 文本溢出列区（claude_ui_list.cpp:100、claude_ui_detail.cpp:377）；(e) pet 气泡固定 32px 高小皮肤被裁（pet_window.cpp:472）；(f) settings 皮肤下拉弹层被卡片 scrollView 视口 clip 裁掉末项（settings_ui_cards.cpp:443，最小窗 popup 底 706 > 视口底 680） |
| 断言 | 统一骨架：**文本 frame 与裁剪方（clip 容器/后绘徽标/弹窗边）互不咬合或显式截断指示**——(a) 状态栏文本 frame ⊆ 其 .clip() 容器；且保存态元素可见（width > 0）；(b) 「逾期N天」日数 2 位时 text frame ⊆ 行 frame；(c) 构造 6 标签放不下 → 出现 `+N` chip 元素；(d) name 文本 frame 与 ACTIVE 徽标 frame 不相交或 name 元素 clip=true；mp 文本 frame ⊆ 行；（e）小皮肤（frameSize 高 < 38）时气泡元素不存在或气泡文本 ⊆ 气泡；(f) 最小窗 + ≥3 皮肤：皮肤下拉 popup frame ⊆ tab 视口 frame（修复后 openUp 或 up 朝上，见类 9 组 (c)） |
| 文件 | `markdown_probes.cpp` §B（a）；`todo_probes.cpp` §C（b、c）；`claude_probes.cpp` §B（d）；`pet_probes.cpp` §D（e）；`settings_cards_probes.cpp` §A（f） |
| 搭法 | (a) 最小窗双栏态；(e) pet 窗建小 frameSize 皮肤（fixtures 构造）；(f) `test_support` 造 ≥3 skin（同 MVP 配置隔离） |
| 优先级 / 适合 | P1（f，major）/ P2（d、e、b）/ P3（a、c）/ ✅；「省略号样式观感」本体不 probe（走 check-consistency + 人工） |

### 类 9 menu-position（4 组 + 附 1 状态组）

| 项 | 内容 |
|---|---|
| bug + 根因 | (a) todo 任务右键菜单「移动到」项数随列数增长超出最小窗屏高，底部删除项不可达（todo_ui_dialogs.cpp:216）；(b) claude 表单「备用 Profile」下拉恒向下越出 600 弹窗底（claude_ui_form.cpp:491）；(c) settings 皮肤下拉无 openUp 防护（settings_ui_cards.cpp:443，同类 8 (f) 根因）；(d) pet 右键菜单 y——已修，锁回归；(e) pet 退出确认菜单锚点重取光标位 → 菜单跳变 + disabled 行死区（pet_window.cpp:200）；附：settings tab 切换不清 openDropdown_ → 回 tab 自动重开（settings_module.cpp:611） |
| 断言 | (a) 15 列 + 开任务右键菜单：菜单最底项 frame.bottom ≤ screen.h（修复后折叠/滚动）；(b) 5 候选 + 开备用下拉：popup frame ⊆ 弹窗 frame 或 `.openUp` 计算为真（组件已支持 openUp）；(c) 同 (b) 判据应用皮肤下拉：最小窗下 popup frame ⊆ 视口（openUp(up) 修复后上弹）；(d) pet 右键触发 → 菜单窗 frame.y == 光标 y + 锚点偏移 且菜单 ⊂ 屏（锁已修行为）；(e) 触发退出确认 → 确认菜单 frame 左上 == 主菜单锚点（复用传入，不随光标）；附：A tab 开下拉 → 切 B → 回 A → popup 元素 `opacity == 0` 且 `disabled`（不自动重开） |
| 文件 | `todo_probes.cpp` §E（a）；`claude_probes.cpp` §D（b）；`settings_cards_probes.cpp` §A（c、附）；`pet_probes.cpp` §B（d）、§C（e） |
| 搭法 | (a) 造 15 列；(d)(e) pet 窗 + 注入右键/左键序列（多窗见 §5 备注，P3 前置） |
| 优先级 / 适合 | P1（c）/ P2（b、d、附）/ P3（a、e——(a) 需 15 列数据量大，(e) 锚点断言需菜单窗 frame 读口）/ ✅ 必须 |

### 类 10 init-only scroll（1 组）

| 项 | 内容 |
|---|---|
| bug + 根因 | markdown 双栏态每帧把 computeSyncOffset 喂 scrollView .offset()，但上游 syncOwnedScrollState 是 init-only 语义（runtime_state_bindings.h:87-97：仅 !initialized 时用声明值），渲染侧读 instance.offset（scrollTransformForElement :157-168）→ 预览滚动冻结（markdown_module.cpp:1495；模块只对 'md.live.scroll' 调 setScrollOffset :1787） |
| 断言 | **程序化同步生效**：双栏态 compose → 注入编辑器滚轮（到 'md.live.scroll' 视口）→ 过 2 帧 → 断言 `md.preview.scroll` 元素 `onScrollOffsetChanged` 被调用（计数器包装：compose 后 swap 该元素 std::function 为带计数包装，跑完还原——probe 拥有 Element* 属测试期注入，不改模块代码）且记录值 == 编辑器滚动预期同步偏移（修复后模块调 ui.setScrollOffset("md.preview.scroll", computed)，消费链 dsl.h:1547-1555 → runtime_update.h:503-511 → :470-492 触发 onScrollOffsetChanged）。当前 bug：0 次调用 → 红 |
| 文件 | `markdown_probes.cpp` §D |
| 搭法 | 双栏态 + 大纲跳转/滚轮两条路径各跑；计数器还原于清理段 |
| 优先级 / 适合 | **P1**（major）/ ✅ 必须（这是唯一 probe 才能抓的类：无头断言元素声明值全绿而视觉失效） |

### 类 11 drag-insert（1 组）

| 项 | 内容 |
|---|---|
| bug + 根因 | todo 拖拽占位条画在 scrollView 内容 column 里，column 布局忽略 .position()（layoutColumn 只认 ignoreLayout），条永远贴列顶 + 整列被下推 16px 跳动（todo_ui_board.cpp:406） |
| 断言 | 注入完整拖拽序列（press 卡 → move 至目标列 hoverRow 卡缝 y → 悬停 2 帧）→ (a) 占位条元素 frame.y == 目标卡缝 y（hoverRow 换算，±4px）；(b) 悬停期间列内容首卡 frame.y 不变（修复前被 16px 下推 → 红，修复后 ignoreLayout 不挤流） |
| 文件 | `todo_probes.cpp` §A |
| 搭法 | 2 列 2 卡数据；拖拽注入序列经 Runtime queuePointerButton/Motion 真分发 |
| 优先级 / 适合 | **P1**（major）/ ✅ 必须 |

### 类 12 click-away（1 组）

| 项 | 内容 |
|---|---|
| bug + 根因 | pet 右键/托盘 styled 菜单唯一关闭机制是失焦自关，Windows 点桌面不换前台窗 → 菜单常驻；macOS 分支恒有焦点（pet_window.cpp:407；window_backend.cpp:936-947）——**已修 EUI-NEO ca40ff2（全窗 dismiss rect）**，probe 锁回归 |
| 断言 | 修复后结构 + 行为双锁：(a) 菜单窗 compose 树内存在全窗 dismiss 元素（id 含 ".dismiss"、size == 窗尺寸、z 最低——仿 contextmenu.h:166-173 结构）；(b) 注入菜单窗内非行区点击（dismiss 层命中）→ 菜单窗关闭/隐藏（多窗需 harness 扩展，见 §5） |
| 文件 | `pet_probes.cpp` §A |
| 搭法 | pet 窗 + showStyledMenu 触发 + 第二窗注入——**前置：harness 多窗支持（§5 备注），未就绪先降级为结构断言 (a)（单窗即可）** |
| 优先级 / 适合 | P3（结构 (a) P2 可先行）/ ✅ 必须但实现垫底 |

### 附：不适合 probe（明确划出，走 check-consistency / 人工 / 无头 / 上游）

| 项 | 归类 | 去向 |
|---|---|---|
| 截断省略号「…」样式观感、hover 过渡 0.1s 手感、拖拽动画手感 | 纯视觉 | check-consistency.sh + 人工清单 |
| todo 落位淡入 0.24s 闪暗 | 动画时序 | 人工（probe 最多锁「松手帧列容器 opacity == 1.0 无 0.6 谷值」弱断言，P3 不排期） |
| dropdown 项文案硬裁无省略号（EUI-NEO dropdown.h:193） | 上游组件级 | 按「EUI-NEO 上游优先」政策修 ../EUI-NEO/ + 上游单测，不进 DevDesk probe |
| markdown chip hover 两截色观感 | 微观感 | 人工（probe 字段一致性断言可选，P3） |
| done 卡 chip 未淡化、优先级 pill 全强度、hover 3 态色差 | 配色/视觉层级 | 人工 |
| 死代码 4 项（priorityColor todo_ui_logic.h:308、托盘 navigate 四函数 app_assembly.cpp:234、dispatchFileOpen 门控提示 app_assembly.cpp:173、sidebar tooltip 注释 app_sidebar.cpp:7） | 静态代码/行为 | 代码修剪 + 无头行为断言（dispatchFileOpen 门控走无头），非 UI probe |
| 字号缩放文案承诺不符（settings_ui_cards.cpp:97） | 文档口径 | 人工/文案修改 |

## §4 MVP probe：dropdown z 序 PoC

- 文件：`DevDesk/tests/probes/settings_dropdown_z_probe.cpp`
- 搭法：
  1. **配置隔离**：`test_support::ScopedConfigPath` + `test_support::tempConfigFile`（tests/test_support.h:299-314、:451-455）把 `DEVDESK_CONFIG_PATH` 指到临时 config.json，再 `create_settings_module()` + `mod->onStart()`（test_modules/test_settings.cpp:726-731 同款）——onStart 吃真机配置会让卡片增减/几何漂移，**探针必须与机器本地配置无关**；harness 开窗 1200×1000（默认窗）；切到含外观卡/字号卡的 tab（复用 headless 的 tab 点击逻辑，最小版内联进 probe）。
  2. **每帧循环（注入与重绘成对）**：queue 输入 → `runtime.update(window, dt, ...)` → **立即重调 `runtime.compose(mod->compose)`**。update 只派发事件、不重建元素树——弹层在 compose 时才按 open 状态进/出 DOM，漏掉 recompose 则弹层永远不进树、断言全部落空。harness `composeModuleFrame` 即此单帧助手（仿 input_runtime_probe.cpp:186-189 `updateEditor` = update + 立即重 compose；点击 = press+release 排队 + 一帧，:190-195 `click` 同款）。
  3. 点**外观卡**字号下拉 field 中心 → 一帧（真命中/焦点分发，非直调回调）。
  4. 断言：
     - a. 弹层元素存在、frame ⊂ 窗（对齐无头同款判定 test_settings.cpp:752-754）；
     - b. **L2 扫描**：与弹层 frame 相交的每个非祖先元素（弹层自身及其子孙不计），其 `subtreeMaxZIndex` 必须 < **弹层链上界**（定义见 §2.2）；≥ → FAIL 打印双方 id + z 值（直接锁 1379cc1 回归）；扫描**显式跳过不可见元素**（`opacity == 0`，如关闭态下拉的自身 popup），只扫可见元素；
     - c. 再点 field 关闭下拉 → 一帧 → 弹层进入**不可见态**（判据见下「不残留定义」）。
  5. 失败 `std::cerr` 明细 + 非 0 退出码（input_runtime_probe 惯例）。

**「不残留」判据**：dropdown 关闭态 popup **仍 compose 在树里**（`visible = open_ ? 1.0f : 0.0f` 只控 opacity 隐没，components/dropdown.h:98、:145-148），因此**不得用「元素存在」断言**开关。闭合断言读开合状态三选一：模块 `openDropdown_` 复位（settings_module.cpp:242-247 Esc 置 -1；:668-673 `setDropdownOpen` → `applyDropdownOpen`）；popup hit `.disabled(!open_)`（dropdown.h:168）；popup 根 `opacity == 0`（dropdown.h:148）。frame 类断言（a）对开/关两态等价——关态 frame 仍在树中、只是不可见，这正是存在性断言会误判的原因。
- 有效性验证（证伪，MVP 验收门禁）：临时把外观卡 zIndex(40) 改回 30（git stash 手法）→ 弹层链上界 40 掉到 30，与下邻编辑器字号卡 subtreeMaxZIndex 30 **等值** → probe 必须红；改回 → 必须绿。只绿不红 = 断言没抓住等值 z 本质，不算完成。
- 与无头分工：`settings_log_dropdown_popup_stays_in_window` 已盖「弹层在窗内 + openUp」；probe 补「跨卡 z 碰撞」这个无头盲区，两者并存不重叠。

## §5 改动清单

| 文件/位置 | 动作 |
|-----------|------|
| `DevDesk/tests/probes/probe_harness.h` | 新建：自含 scaffold（backend init / 参数 / pollOnce / composeModuleFrame / L2 扫描 / **尺寸矩阵参数化** `--size WxH` 多跑 / **元素回调包装工具** swapOnScrollCallback(id, counter) 供类 10 / **多窗 stub**：第二窗 create + 独立 Runtime + 跨窗注入，P3 pet 前置） |
| `DevDesk/tests/probes/settings_dropdown_z_probe.cpp` | 新建：MVP（§4，P0） |
| `DevDesk/tests/probes/home_probes.cpp` | 新建：类 1(b) spark z / 类 2(a) 空转重绘 / 类 2(b) hover token / 类 4(a) barChart 几何 / 类 5(a) API 卡对称 / 类 6(a) stat 溢出（P1） |
| `DevDesk/tests/probes/markdown_probes.cpp` | 新建：类 3(b) 双层卡 / 类 8(a) 状态栏 / 类 10 双栏同步滚动 / 类 2(c) chip hoverColor 弱断言（P1 类 10，余 P2/P3） |
| `DevDesk/tests/probes/todo_probes.cpp` | 新建：类 11 占位条 / 类 7(a) 8 列溢出 / 类 8(b)(c) 逾期·标签 / 类 3(a) 卡 double-bg / 类 9(a) 菜单超屏（P1 类 11，余 P2/P3） |
| `DevDesk/tests/probes/claude_probes.cpp` | 新建：类 7(b) sharedH 盖列表 / 类 8(d) 卡文本·mp 截断 / 类 6(b)(c) 弹窗溢出 / 类 9(b) 备用下拉 openUp（P1 类 7(b)，余 P2） |
| `DevDesk/tests/probes/settings_cards_probes.cpp` | 新建：类 8(f)/9(c) 皮肤下拉 clip·openUp / 类 4(b) 按钮 theme 泄漏 / 类 5(b) 顶边距 / 类 6(d) 降级提示行 / 附 tab 弹层状态（P1 类 8(f)，余 P2/P3） |
| `DevDesk/tests/probes/pet_probes.cpp` | 新建：类 12 click-away 回归 / 类 9(d) 右键锚点回归 / 类 9(e) 退出确认锚点 / 类 8(e) 气泡（P2 结构 (a)，P3 行为与其余——多窗前置） |
| `DevDesk/tests/CMakeLists.txt` | ① TIN-10 豁免 probes 目录：现 GLOB `*/*.cpp`（tests/CMakeLists.txt:67-68）会命中 `tests/probes/*.cpp` → 必须改 GLOB 排除或在该自检循环对 probes 路径 continue（否则 configure FATAL）；② 新增 probe 注册块（仿 EUI-NEO/CMakeLists.txt:698-717）：GLOB probes → 每个 `add_executable` + link `devdesk_module_*`（同 devdesk_tests 集合 tests/CMakeLists.txt:129-136）+ `devdesk_add_eui_includes` + include 集与 devdesk_tests 一致（tests/CMakeLists.txt:120-124）：`src/`、`tests/`、`src/module_claude_proxy`、`src/module_settings`——probe include `app_registry.h` 时其 `#include "settings_config.h"`（src/app_registry.h:19）落在 `src/module_settings/` 下，缺该目录直接编译失败 + `-mconsole`（MinGW 控制台子系统，退出码才有意义）+ `add_test(NAME ... COMMAND ... --frames 3)` + `LABELS probe`；不并进 devdesk_tests 二进制（probe 各带 main） |
| DevDesk 顶层 `CMakeLists.txt` | 不动（`enable_testing()` 已开 :124；`eui_neo_configure_app` 同 scope 可复用 :131 先例） |
| 依赖 EUI-NEO probe helper | **不依赖**——harness 自含；只消费 EUI-NEO 公共头（已随 `eui::neo`/include 路径就位）。上提共享 harness 记 follow-up |
| `scripts/build-test-run.sh` | 本轮不动；probe 跑批（`ctest -L probe`）接线放后续轮 |

**分批实现顺序**（§8 流程）：P0 MVP（settings_dropdown_z）→ **P1**（home_probes + markdown 类 10 + todo 占位条 + claude sharedH + settings 皮肤下拉）→ **P2**（todo 溢出/截断/double-bg、claude 截断/弹窗/openUp、settings 按钮/边距/tab、pet 结构档）→ **P3**（markdown 状态栏/chip 弱断言、todo 菜单超屏、pet 行为档 + 多窗 harness 扩展）。probe 总数：**7 个可执行文件**（1 MVP + 6 模块聚合），覆盖 12 类 32 处断言组。

## §6 与无头 / check-consistency 分工

```
无头单测（快/稳/CI 绿）── 状态·数据·事件回调·单元级几何（弹层⊂窗等）
      │
probes（真窗/秒级/需显示）── Runtime 注入·交叉 z 序·多窗尺寸矩阵·结构性色源断言
      │
check-consistency.sh（截图）── 像素观感·配色审美·肉眼验收
      │
人工清单 ── 动画·拖拽手感·截断观感
```

- 判据一句话：**无头能断言的绝不上 probe**（无头不增构建面、CI 稳）；probe 只补「无头测不到 + 可机械断言」的缺口（z 碰撞、多尺寸矩阵、结构色源）；纯视觉留 check-consistency + 人工。
- ctest 分层：probe 独立 `LABELS probe`，`ctest` 默认全跑、无显示环境用 `ctest -LE probe` 跳过，不互污染。

## §7 风险

| 风险 | 缓解 |
|------|------|
| 真窗依赖显示环境（CI/无头机） | probe 独立 LABEL 可跳过；Windows 本机（当前开发环境）有显示 |
| 平台差异：GLFW/SDL2 双后端、mac 侧 OpenGL | MVP 先 Windows/GLFW 单后端落地；harness 沿用 EUI-NEO probe 的宏二选一（input_runtime_probe.cpp:6-16），mac 扩线后续 |
| 真窗事件时序 flake（焦点/轮询竞态） | 断言只读 frame 与同步注入队列（无异步等待）；固定窗尺寸 + `--frames` 可复现 |
| 构建时长 | 每 probe 一个 exe 目标，ccache 热缓存（95%+ 命中）下增量秒级；只跑定向编译，禁全量 |
| `-Wall -Wextra -Werror`（顶层要求） | probe 源码按应用同标准写，harness 头不产生 signed 比较等告警 |
| TIN-10 误伤 | §5 已列为必改项——不加豁免则 configure 直接 FATAL（这正是会拦下漏登记的机制，不能绕过它） |
| 权限/窗口焦点副作用（探针抢焦点） | probe 窗 `focusOnShow` 关（window_backend.cpp:783-785 有 hint），且测试期无人值守场景少 |
| **probe 数量与构建时长**（7 exe） | 分批落地（§5 分批顺序）：每批 1-2 个新 exe，ccache 热缓存（99%+ 命中）下单 exe 增量秒级；禁全量重建，只跑定向编译 + 该批 probe |
| **多窗注入（pet 类 12 / 类 9(d)(e)）** | harness 多窗是新增能力（EUI-NEO probe 无先例），P3 前置；未就绪时类 12 降级为单窗结构断言（dismiss 元素存在 + 尺寸全窗），点击关闭行为延后 |
| **类 10 断言依赖回调包装**（swap onScrollOffsetChanged 计数器） | 计数包装是测试期注入、跑完还原（RAII）；依赖"修复方向 = 模块调 ui.setScrollOffset"——断言与修复同向，修复未落地时该组红是预期（证伪门禁同 §4 手法） |
| **造数规模**（todo 8-15 列 / claude 5 profile / pet 小皮肤） | 复用无头测试既有造数路径（test_todo / test_claude_ui 同款）；造数成本高的组（15 列菜单超屏）排 P3 |
| **上游组件级缺陷**（dropdown 项文案硬裁，EUI-NEO dropdown.h:193） | 明确不进 DevDesk probe——按「EUI-NEO 上游优先」单独派单修 ../EUI-NEO/ + 上游单测，本 spec 只锁 DevDesk 侧接线 |
| **审计 41 findings 中非 UI-probe 项**（死代码/文档口径/纯观感） | 已在 §3-probe-full「附」表划出，不在本 spec 排期（走人工/无头/代码修剪） |

## §8 流程

**分批实现（MVP → P1 → P2 → P3）**，每批同走「审计 → dev（worktree 隔离 + 定向编译）→ 验收」：

1. **审计**（本 spec，已含 §3-probe-full 全 12 类）→ 用户过目拍板（MVP 选型、probes 目录放 `tests/probes/` 与 TIN-10 豁免方式、是否要 `--pixels` 档、分批顺序）。
2. **P0 = MVP**（§4 settings_dropdown_z_probe）：派单 worktree 隔离、只跑定向编译（probe exe）+ `ctest -L probe`；完成后原子 commit 合 master、清 worktree。**验收门禁**：probe 绿 + 反向证伪（zIndex 改回 30 必须红）+ 真窗运行截图。
3. **P1 批**（§5 批次）：home_probes + markdown 类 10（双栏同步）+ todo 类 11（占位条）+ claude 类 7(b)（sharedH）+ settings 类 8(f)（皮肤下拉）——全部 major 类，每文件一个子任务。验收门禁同 P0（各 probe 绿 + 反向证伪：类 10 撤掉 setScrollOffset 修复必须红、类 11 去掉 ignoreLayout 必须红）。
4. **P2 批**：todo 溢出/截断/double-bg、claude 截断/弹窗/备用下拉、settings 按钮 theme/顶边距/tab 弹层态、pet 结构档（click-away dismiss 元素 + 右键锚点回归）。无新增 harness 能力，只加断言组。
5. **P3 批**：markdown 状态栏/chip 弱断言、todo 菜单超屏（15 列）、pet 行为档——前置 harness 多窗扩展；sparkline 组件级断言同步在 EUI-NEO 侧评估（跨仓库，单独派单）。
6. 后续候选：`scripts/build-test-run.sh` 集成 `ctest -L probe`；harness 上提 EUI-NEO。