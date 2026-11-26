# 终端 M2（桌面主力）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans（本批选择原生执行）。Steps use checkbox (`- [ ]`) syntax.

**Goal:** 让桌面终端成为主力：鼠标/选择/搜索/字号/右键可用，误关有拦截，标签可管理，多项目一键开，标题/提示/链接跟上。

**Architecture:** 先把「鼠标/选区/滚动」输入路由从 `terminal_page` 的 compose 巨函数里抽成独立组件（纯状态机，可单测），后续交互项都长在组件上；仿真层能力（鼠标上报、OSC、bell）补在 `terminal_vterm_core` 与后端接口；生命周期走既有 `app::setQuitGuard`。

**Tech Stack:** C++20、EUI-NEO（ContextMenu / MouseArea / window::clipboardText）、libvterm（已内置鼠标编码）、迷你 doctest、`scripts/build-test-run.sh`、Playwright（网页侧 M3 用）。

**Spec:** `docs/superpowers/specs/2026-11-25-terminal-completion-design.md`

## Global Constraints

- 同 M1：构建唯一入口脚本、禁业务 `#ifdef`、不抛异常、迷你 doctest 宏名 `DOCTEST_CHECK/EQ/NE/TRUE/FALSE`（实际宏为 `DOCTEST_CHECK_EQ` 等，见 M1 记录）、先红后绿。
- `terminal_page.cpp` 只减不增：抽缝后新功能加在组件/独立文件中；compose 内不再叠交互逻辑。
- 尺寸/内存预算：新增常驻 ≤ 2 MB；页切换 ≤ 20 ms；典型操作 ≤ 50 ms。
- 网页端不实现桌面独有项（见 spec 三端矩阵）。
- Windows 侧只经 CI（doctest），不宣称真机验收。

## Review Focus

- **抽缝等价性**：选区锚点、滚轮 3 行/档、滚动清选区、滚动条拖拽在抽缝前后行为一致（同一组断言）。
- **鼠标上报与本地手势冲突**：应用请求鼠标时 Shift 必须能强制本地拖选；未请求时滚轮仍滚回看。
- **快捷键优先级**：页面先于模块收到键；Cmd+F 不得被终端字节流吞掉，也不得在设置面板打开时误触。
- **退出/关标签确认**：只拦「运行中」；Idle/Exited/Failed 不打扰。
- **搜索三缺口**：向后搜要能回绕；子串高亮不能越出命中行；无命中时不得留旧高亮。

---

### Task 1: 输入路由抽缝（纯状态机）

**Files:**
- Create: `src/module_terminal/terminal_input_router.h` / `.cpp`
- Modify: `src/module_terminal/terminal_page.cpp`（`onScroll` 555-570、`onPress` 528-537、`onRelease` 546-554、拖动 538-545 改为委托）
- Modify: `src/module_terminal/terminal_page.h`（删选区四成员，改持组件）
- Test: `tests/test_modules/test_terminal_input_router.cpp`（新文件，加入 `tests/CMakeLists.txt`）

**Interfaces:**
- Produces:
  - `struct CellPos { int row; int col; };`
  - `void TerminalInputRouter::press(CellPos p);`
  - `void TerminalInputRouter::dragTo(CellPos p);`
  - `void TerminalInputRouter::release();`
  - `void TerminalInputRouter::clearSelection();`
  - `bool TerminalInputRouter::hasSelection() const;`
  - `CellPos TerminalInputRouter::anchor() const;` / `head() const;`
  - `int TerminalInputRouter::scrollBy(int notches, int maxOffset);`（3 行/档；滚动即清选区）
- Consumes: 无（纯状态机，不碰 UI）。

- [ ] **Step 1: 写失败测试**（选区锚点/方向反转/滚动清选区/滚轮 3 行/越界钳制）

- [ ] **Step 2: 跑测试确认红**

Run: `bash scripts/build-test-run.sh --dev --no-run --filter terminal_input_router` → FAIL（符号未定义）

- [ ] **Step 3: 实现组件并改页面委托**

`terminal_page.cpp` 的四个绑定只做「坐标 → CellPos」换算后委托组件；`terminal_page.h` 删除 `selARow_/selACol_/selHRow_/selHCol_/hasSelection_/selecting_`，改持 `TerminalInputRouter input_;`。

- [ ] **Step 4: 跑测试确认绿 + 终端探针**

Run: `bash scripts/build-test-run.sh --dev --no-run --filter terminal_input_router`；`cd build && ctest -R "terminal_scrollbar_probe|terminal_settings_panel_probe|ui_driver_terminal-input"` → 全绿

- [ ] **Step 5: 提交**

`refactor: 终端输入路由抽成 TerminalInputRouter（M2 Task 1）`

---

### Task 2: 日常交互补齐包

**Files:**
- Modify: `src/module_terminal/terminal_page.cpp`（搜索条 1143-1153、搜索钮 760-773、网格交互层 521-620、键位处理 571+）
- Modify: `src/module_terminal/terminal_search.h/.cpp`（向后搜 + 命中列区间）
- Modify: `src/module_terminal/terminal_input_router.*`（双击选词/三击选行/Shift+点击扩选、Shift+PageUp/Down 回看步进）
- Modify: `src/module_terminal/terminal_grid_widget.*`（子串高亮绘制函数）
- Test: `tests/test_modules/test_terminal_search.cpp`（扩）、`test_terminal_input_router.cpp`（扩）、`test_terminal_frame.cpp`（高亮区间纯函数）

**Interfaces:**
- Produces:
  - `bool collectMatchLines(..., int fromOrdinal, int direction, ...)` 增加 `direction = ±1`
  - `struct MatchCellRange { int row; int colBegin; int colEnd; };` 与 `std::vector<MatchCellRange> matchRanges(...)`
  - `void TerminalInputRouter::setWordBoundaryResolver(std::function<std::pair<int,int>(CellPos)>);`（双击选词用；三击 = 整行）
  - 页面：`Cmd/Ctrl+F` 开搜、`Cmd/Ctrl + = / - / 0` 字号、`Shift+PageUp/Down` 回看一页、`onContextMenu` → EUI `components::contextMenu`（复制/粘贴/清屏/搜索）
- Consumes: EUI `core/window/window_backend.h` `clipboardText(handle)`（读剪贴板，先例 `notepad_window.cpp:629-635`）；`ui_geometry.h:18-23` 菜单锚点。

- [ ] **Step 1: 写失败测试**：向后搜回绕、命中列区间（含中文宽字行）、双击选词边界（CJK/标点）、Shift+PageUp 一页步进
- [ ] **Step 2: 确认红**（`--filter "file:test_terminal_search"` / `terminal_input_router`）
- [ ] **Step 3: 实现**（高亮用 `MatchCellRange` 画底色；右键菜单四项接既有动作；字号写回 `TerminalSettings.fontSize` 并触发重排）
- [ ] **Step 4: 确认绿** + 真窗口 probe（搜索条/右键菜单/字号几何）
- [ ] **Step 5: 提交** `feat: 终端日常交互补齐（搜索三缺口/字号快捷键/右键菜单/选词选行/Shift 回看/选即复制）`

---

### Task 3: 退出保护

**Files:**
- Modify: `src/module_terminal/module_terminal.cpp`（`onStop` 134-146、模块 `onKeyEvent`）
- Modify: `src/app_assembly.cpp:97-100`（注册终端守卫，与 notepad 并列）
- Create: `src/module_terminal/terminal_quit_guard.h/.cpp`
- Test: `tests/test_modules/test_terminal_quit_guard.cpp`

**Interfaces:**
- Produces: `bool terminalQuitGuard();`（有运行中会话 → 打开三选弹窗「关闭全部/取消」，取消返 false）；`bool confirmCloseSession(int index);`（页面 × 与关闭其他共用）

- [ ] **Step 1: 失败测试**：会话状态矩阵（Idle/Running/Exited/Failed）→ 是否需确认；取消路径不改会话数
- [ ] **Step 2: 红** → **Step 3: 实现**（复用 notepad 弹窗形态；`setQuitGuard` 组合器：任一 guard 拒绝即拦截）→ **Step 4: 绿 + UI probe** → **Step 5: 提交**

---

### Task 4: 标签管理

**Files:**
- Modify: `src/module_terminal/terminal_page.cpp`（TAB 区 825-967）
- Test: `tests/test_modules/test_terminal_tabs.cpp`（标签顺序重排纯函数）

**Interfaces:**
- Produces: `int reorderTabIndex(int from, int to, int count);`（拖拽落位计算，纯函数）；双击标签 → `setSessionTitle`；右键「关闭其他/关闭右侧」

- [ ] **Step 1-5**：测试重排映射含边界（首/尾/自身）→ 红 → 实现（`components::mouseArea` + `onDragStart/onDrag/onDragEnd`）→ 绿 + probe → 提交

---

### Task 5: 启动目录常用列表（#383）

**Files:**
- Modify: `src/module_terminal/terminal_settings.h`（`workDir` → `std::vector<std::string> recentWorkDirs` + 保留 `workDir` 兼容）
- Modify: `src/module_terminal/terminal_page.cpp`（设置面板目录段、新建标签下拉）
- Test: `tests/test_modules/test_terminal_settings.cpp`（持久化往返 + 去重/上限 8 条）

- [ ] **Step 1-5**：TDD 同上；迁移旧单值到列表首项 → probe → 提交

---

### Task 6: 终端信号补齐（OSC 标题 / bell / OSC 52）

**Files:**
- Modify: `src/platform/terminal/terminal_vterm_core.cpp`（`settermpropCbImpl` 271-273、回调表 137-152）
- Modify: `src/platform/terminal/terminal_vterm_core.h`（暴露 `title()`、`bellCount()`、`onBell(std::function<void()>)`）
- Modify: `src/module_terminal/terminal_session_controller.cpp`（标题回写 `setSessionTitle`）
- Test: `tests/test_platform/test_terminal_vterm_core.cpp`（扩：OSC 0/2 设标题、bell 计数、OSC 52 写剪贴板走注入缝）

**Interfaces:**
- Produces: `std::string title() const;`、`uint32_t bellCount() const;`、`void setBellHandler(std::function<void()>);`

- [ ] **Step 1-5**：喂 `\x1b]0;ssh host\x07` → title 变化；`\x07` → bellCount+1；`\x1b]52;c;<base64>\x07` → 剪贴板注入缝收到文本 → 红→绿 → 提交

---

### Task 7: 链接识别点击（app）

**Files:**
- Create: `src/module_terminal/terminal_links.h/.cpp`
- Modify: `src/module_terminal/terminal_page.cpp`（悬停/点击判定，Cmd/Ctrl+点击）
- Test: `tests/test_modules/test_terminal_links.cpp`

**Interfaces:**
- Produces: `std::vector<LinkSpan> findLinks(const std::string& lineText, int baseCol);`（`LinkSpan{colBegin,colEnd,std::string target,Kind{Url,Path}}`）；`bool openTarget(const std::string& target);`（走 `platform::openFileOrUrl` 既有能力）

- [ ] **Step 1-5**：URL/绝对路径/引号括号/行尾截断/中文相邻 → 红 → 绿 → probe → 提交

---

### Task 8: 鼠标上报（一次性做完）

**Files:**
- Modify: `src/platform/terminal/terminal_vterm_core.h/.cpp`（鼠标入口 + 模式跟踪 + `altscreen()` 只读 + `mouseReporting()`）
- Modify: `src/platform/terminal/terminal_backend.h`（后端接口加鼠标事件三件）
- Modify: `src/platform/terminal/terminal_backend_mac.cpp` / `_win.cpp`（实现桥接）
- Modify: `src/module_terminal/terminal_page.cpp`（交互层路由：上报优先，Shift 覆盖本地）
- Test: `tests/test_platform/test_terminal_vterm_mouse.cpp`（vterm 喂 DECSET 序列 → 上报字节断言）

**Interfaces:**
- Produces: `void mouseMove(int row,int col,bool buttonDown);`、`void mouseButton(int row,int col,int button,bool pressed);`、`bool mouseReporting() const;`、`bool altscreen() const;`

- [ ] **Step 1-5**：`\x1b[?1000h` + `\x1b[?1006h` 后按下 → 产出 `\x1b[<0;col;rowM`；未请求时零产出；1003 移动上报；滚轮在请求时发 `\x1b[<64;..M`；Shift 覆盖走本地选区的页面判定 → 红→绿 → 真机 vim/tmux/htop/claude 走查 → 提交

---

## M2 收口

- [ ] 全量 `bash scripts/build-test-run.sh`（记录与 M1 相同的 7 条存量红判定）
- [ ] 终端探针 + 新增 probe 全绿
- [ ] mac 真机：vim/tmux/htop/claude 鼠标与选择、搜索、右键、字号、退出确认、标签改名/拖拽、启动目录列表、链接点击
