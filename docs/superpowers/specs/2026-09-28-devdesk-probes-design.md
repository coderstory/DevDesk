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
| `DevDesk/tests/probes/probe_harness.h` | 新建：自含 scaffold（backend init / 参数 / pollOnce / composeModuleFrame / L2 扫描） |
| `DevDesk/tests/probes/settings_dropdown_z_probe.cpp` | 新建：MVP（§4） |
| `DevDesk/tests/CMakeLists.txt` | ① TIN-10 豁免 probes 目录：现 GLOB `*/*.cpp`（tests/CMakeLists.txt:67-68）会命中 `tests/probes/*.cpp` → 必须改 GLOB 排除或在该自检循环对 probes 路径 continue（否则 configure FATAL）；② 新增 probe 注册块（仿 EUI-NEO/CMakeLists.txt:698-717）：GLOB probes → 每个 `add_executable` + link `devdesk_module_*`（同 devdesk_tests 集合 tests/CMakeLists.txt:129-136）+ `devdesk_add_eui_includes` + include 集与 devdesk_tests 一致（tests/CMakeLists.txt:120-124）：`src/`、`tests/`、`src/module_claude_proxy`、`src/module_settings`——probe include `app_registry.h` 时其 `#include "settings_config.h"`（src/app_registry.h:19）落在 `src/module_settings/` 下，缺该目录直接编译失败 + `-mconsole`（MinGW 控制台子系统，退出码才有意义）+ `add_test(NAME ... COMMAND ... --frames 3)` + `LABELS probe`；不并进 devdesk_tests 二进制（probe 各带 main） |
| DevDesk 顶层 `CMakeLists.txt` | 不动（`enable_testing()` 已开 :124；`eui_neo_configure_app` 同 scope 可复用 :131 先例） |
| 依赖 EUI-NEO probe helper | **不依赖**——harness 自含；只消费 EUI-NEO 公共头（已随 `eui::neo`/include 路径就位）。上提共享 harness 记 follow-up |
| `scripts/build-test-run.sh` | 本轮不动；probe 跑批（`ctest -L probe`）接线放后续轮 |

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

## §8 流程

1. **审计**（本 spec）→ 用户过目拍板（MVP 选型、probes 目录放 `tests/probes/` 与 TIN-10 豁免方式、是否要 `--pixels` 档）。
2. **dev**：派单规则——worktree 隔离、只跑定向编译（probe exe）+ MVP 手动跑 `ctest -L probe`；完成后原子 commit 合 master、清 worktree。
3. **MVP 验收门禁**：probe 绿 + 反向证伪（§4 有效性验证：zIndex 改回 30 必须红）+ 真窗运行截图。
4. **增量**：按 §3 优先级 P1 → P2 → P3 逐批加 probe_*.cpp（每批同走审计→dev→验收）；sparkline 组件级断言同步在 EUI-NEO 侧评估（跨仓库，单独派单）。
5. 后续候选：`scripts/build-test-run.sh` 集成 `ctest -L probe`；harness 上提 EUI-NEO。