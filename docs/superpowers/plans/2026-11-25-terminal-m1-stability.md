# 终端 M1（核验 + 稳定性）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让终端在三处「不可用」变「可用」——外接屏/小字号白屏、256 色塌缩、Windows Ctrl+Shift 死键，并把网页端待验收项收口。

**Architecture:** 四处改动都在既有分层内完成，不新增模块：显示层补 256 色查表（`terminal_grid_widget.cpp`），键盘层放开 Ctrl+Shift 的 C0 分支并抽出「剪贴板裁决」纯函数（`terminal_keyencoder.*`），mac 桥接层扩容快照镜像并加尺寸钳制（`swift-bridge/CortaBridge.swift` + `swift_bridge.*`），网页端只做验证与收口（CSP 单一源已落地）。

**Tech Stack:** C++20、EUI-NEO（submodule）、自研迷你 doctest（`REGISTER_TEST`）、libvterm、Swift（Corta 桥）、`scripts/build-test-run.sh` 唯一构建入口、GitHub Actions（`windows-2025` 跑 Windows 侧 doctest）。

**Spec:** `docs/superpowers/specs/2026-11-25-terminal-completion-design.md`

## Global Constraints

- 构建/测试唯一入口 `bash scripts/build-test-run.sh`；研发期带 `--dev` + 定向 `--filter`，禁止手敲 `cmake`。
- 业务代码禁止 `#ifdef _WIN32` / `__APPLE__`；平台差异拆 `*_win.cpp` / `*_macos.cpp`。
- 不抛 C++ 异常，错误走 `Result<T>`（`src/core/result.h`）。
- 测试框架是迷你 doctest：只有 `DOCTEST_CHECK/EQ/NE/TRUE/FALSE/REQUIRE*`、`CHECK_MESSAGE`、`SKIP`，**没有 `CHECK_LE`**；用例体以 `return true;` 收尾。
- 先红后绿：每个修复必须先在修复前跑出失败。
- 记忆与体积预算：快照镜像扩容后常驻增量 ≤ 4 MB，主二进制 ≤ 60 MB（五维预算，2026-11-22 口径）。
- Windows 侧本计划只用 CI（`windows-2025`：Release 构建 + doctest 全量 + headless 探针 + 网页 E2E）；不宣称真机验收。
- 提交前缀 `feat:` / `fix:` / `refactor:` / `docs:`；过程用 `wip:`。

## Review Focus

- **4K 外接屏 + 12px 字号**：列数约 550，超过旧上限 200 → 必须正常渲染，不得静默丢帧。
- **256 色边界**：索引 16 / 196 / 231 / 232 / 255 与主题切换 → 颜色不得随主题漂移（16–255 恒用标准表）。
- **Ctrl+Shift 组合**：mac 上 Ctrl+Shift+C 的既有语义（发 `\x03`）不得因修复而丢失；Cmd 语义不受影响。
- **超限窗口尺寸**：用户把窗口拖到超过新上限 → 钳制 + 告警，不崩、不白屏、不循环告警。
- **网页严格 CSP**：页面无内联样式被拦、favicon 不 404、`frame-ancestors` 生效。

---

### Task 1: 事实核验（只读，产出核验表）

**Files:**
- Modify: `docs/superpowers/specs/2026-11-25-terminal-completion-design.md`（§三「待核」改为结论）
- Test: 无（只读核验，不产生代码）

**Interfaces:**
- Produces: 核验表（每条断言的真/假/部分 + `file:line`）；本计划后续 Task 的开工前提。

- [ ] **Step 1: 核 #508 容量上限**

Run: `grep -n "cellCapacity\|maxCols\|maxRows" src/platform/terminal/swift-bridge/Sources/CortaBridge/CortaBridge.swift src/platform/terminal/swift_bridge.cpp`
Expected: 两侧均为 200×60=12000；Swift 侧 `snapshotCells` 在 `rows*cols > cellCapacity` 返回 false。

- [ ] **Step 2: 核 #509 颜色映射**

Run: `sed -n '234,255p' src/module_terminal/terminal_grid_widget.cpp`
Expected: `ansiIndexedColor` 只查 16 项表，`index > 15` 回落 `kDark[7]/kLight[7]`。

- [ ] **Step 3: 核 OSC / bell / 滚动 / #465 / #383 / 中文字距**

Run: `grep -n "bell\|OSC\|osc" src/platform/terminal/terminal_vterm_core.cpp | head -20`；`grep -n "ensureBackend\|onPageEnter" src/module_terminal/module_terminal.cpp`；`grep -n "workDir\|启动目录" src/module_terminal/terminal_settings.h`；`sed -n '360,372p' src/module_terminal/terminal_grid_widget.cpp`
Expected: bell 为 `nullptr`；OSC 标题零消费；启动目录单值；宽字按固定 2 格。

- [ ] **Step 4: 核网页待验收项**

Run: `sed -n '14,30p' src/module_terminal/web/web_routes.cpp`；`bash tests/e2e/run-e2e.sh`
Expected: CSP 已是唯一响应头源、index.html 无 meta CSP；E2E 结果记录（全绿或具体失败步）。

- [ ] **Step 5: 写核验结论并提交**

把 §三「待核」9 条逐条改成「真 / 假 / 部分 + 证据」，判「假」的写明处置（关闭 / 改范围）。

```bash
git add docs/superpowers/specs/2026-11-25-terminal-completion-design.md
git commit -m "docs: terminal 事实核验结论（M1 Task 1）"
```

---

### Task 2: 256 色索引查表（#509）

**Files:**
- Modify: `src/module_terminal/terminal_grid_widget.cpp:234-255`（`ansiIndexedColor`）
- Test: `tests/test_modules/test_terminal_frame.cpp`（新增 `frameIndexed256`）

**Interfaces:**
- Consumes: `core::Color ansiIndexedColor(int index, bool darkTheme)`（`terminal_grid_widget.h:37`）
- Produces: 同签名，行为扩展到 0–255；16–255 恒用标准表，不随 `darkTheme` 变化。

- [ ] **Step 1: 写失败测试**

在 `tests/test_modules/test_terminal_frame.cpp` 追加：

```cpp
REGISTER_TEST(frameIndexed256,
              "terminal frame: 256 色索引表（色立方 + 24 级灰）", {
    // 色立方：16→黑、196→纯红、231→纯白；灰阶：232→8、255→238
    DOCTEST_CHECK(colorNear(terminal::ansiIndexedColor(16, true), 0.0f, 0.0f, 0.0f));
    DOCTEST_CHECK(colorNear(terminal::ansiIndexedColor(196, true), 1.0f, 0.0f, 0.0f));
    DOCTEST_CHECK(colorNear(terminal::ansiIndexedColor(231, true), 1.0f, 1.0f, 1.0f));
    DOCTEST_CHECK(colorNear(terminal::ansiIndexedColor(232, true), 8 / 255.0f, 8 / 255.0f, 8 / 255.0f));
    DOCTEST_CHECK(colorNear(terminal::ansiIndexedColor(255, true), 238 / 255.0f, 238 / 255.0f, 238 / 255.0f));
    // 16–255 不随主题漂移（两套表只覆盖 0–15）
    auto a = terminal::ansiIndexedColor(196, true);
    auto b = terminal::ansiIndexedColor(196, false);
    DOCTEST_CHECK(colorNear(a, b.r, b.g, b.b));
    return true;
})
```

- [ ] **Step 2: 跑测试确认红**

Run: `bash scripts/build-test-run.sh --dev --no-run --filter "frameIndexed256"`
Expected: FAIL（196 实际回落成灰）。

- [ ] **Step 3: 实现查表**

在 `ansiIndexedColor` 的 16 项表之前加：索引 16–231 按 `n=index-16`，`r=n/36`、`g=(n%36)/6`、`b=n%6`，各分量 `v==0 ? 0 : 55+40*v`；索引 232–255 三通道同值 `8+10*(index-232)`；`index<0 || index>255` 回落 `kDark[7]/kLight[7]`。

同时把既有 `frameAnsiPalette` 的越界用例从 `ansiIndexedColor(99, true)` 改为 `ansiIndexedColor(256, true)`——99 在新口径下是合法灰阶，原断言会因需求变更而红（这是预期内的测试更新，不是回归）。

- [ ] **Step 4: 跑测试确认绿**

Run: `bash scripts/build-test-run.sh --dev --no-run --filter "file:test_terminal_frame"`
Expected: PASS（含既有 `frameAnsiPalette`）。

- [ ] **Step 5: 真机验证配色并提交**

在 `vim`、`htop`、`tmux` 各开一次，对照系统终端；截图入 `evidence/`。

```bash
git add src/module_terminal/terminal_grid_widget.cpp tests/test_modules/test_terminal_frame.cpp
git commit -m "fix: 终端 256 色索引补全（bug509）"
```

---

### Task 3: Ctrl+Shift 键与剪贴板裁决（#510）

**Files:**
- Modify: `src/module_terminal/terminal_keyencoder.cpp:101`（`mods.control && !mods.shift` → `mods.control`）
- Modify: `src/module_terminal/terminal_keyencoder.h`（新增纯函数声明）
- Modify: `src/module_terminal/terminal_keyencoder.cpp`（新增纯函数实现）
- Modify: `src/module_terminal/terminal_page.cpp:571-604`（`onKeyEvent` 先问裁决函数）
- Test: `tests/test_modules/test_terminal_keyencoder.cpp`

**Interfaces:**
- Produces:
  - `bool encodeKey(eui::InputKey, const eui::KeyModifiers&, std::string&)` — 行为变更：Ctrl+Shift+字母 也走 C0 分支。
  - `enum class CtrlShiftAction { None, Copy, Paste };`
  - `CtrlShiftAction ctrlShiftAction(eui::InputKey key, const eui::KeyModifiers& mods, const platform::TerminalKeyConventions& conv, bool hasSelection);`
- Consumes: `platform::TerminalKeyConventions`（`terminal_backend.h:95-103`，win 侧 `ctrlCIsCopy/ctrlVIsPaste=true`，mac 侧均为 false）。

- [ ] **Step 1: 写失败测试（编码 + 裁决）**

在 `tests/test_modules/test_terminal_keyencoder.cpp` 追加：

```cpp
REGISTER_TEST(keyencoderCtrlShiftC0,
              "terminal keyencoder: Ctrl+Shift+字母 走 C0 分支", {
    std::string out;
    DOCTEST_CHECK(terminal::encodeKey(eui::InputKey::A, mods(true, false, true), out));
    DOCTEST_CHECK(out == "\x01");
    out.clear();
    DOCTEST_CHECK(terminal::encodeKey(eui::InputKey::Z, mods(true, false, true), out));
    DOCTEST_CHECK(out == "\x1a");
    return true;
})

REGISTER_TEST(keyencoderCtrlShiftClipboard,
              "terminal keyencoder: Ctrl+Shift+C/V 剪贴板裁决随平台惯例", {
    platform::TerminalKeyConventions win;
    win.ctrlCIsCopy = true; win.ctrlVIsPaste = true;
    platform::TerminalKeyConventions mac;
    mac.ctrlCIsCopy = false; mac.ctrlVIsPaste = false;
    DOCTEST_TRUE(terminal::ctrlShiftAction(eui::InputKey::C, mods(true, false, true), win, true)
                 == terminal::CtrlShiftAction::Copy);
    DOCTEST_TRUE(terminal::ctrlShiftAction(eui::InputKey::V, mods(true, false, true), win, false)
                 == terminal::CtrlShiftAction::Paste);
    DOCTEST_TRUE(terminal::ctrlShiftAction(eui::InputKey::C, mods(true, false, true), win, false)
                 == terminal::CtrlShiftAction::None);
    DOCTEST_TRUE(terminal::ctrlShiftAction(eui::InputKey::C, mods(true, false, true), mac, true)
                 == terminal::CtrlShiftAction::None);
    DOCTEST_TRUE(terminal::ctrlShiftAction(eui::InputKey::C, mods(true, false, false), win, true)
                 == terminal::CtrlShiftAction::None);
    return true;
})
```

- [ ] **Step 2: 跑测试确认红**

Run: `bash scripts/build-test-run.sh --dev --no-run --filter "file:test_terminal_keyencoder"`
Expected: FAIL（`ctrlShiftAction` 未定义 / Ctrl+Shift+A 无输出）。

- [ ] **Step 3: 实现**

`encodeKey` 的 Ctrl 分支条件去掉 `!mods.shift`。新增裁决函数：仅当 `mods.control && mods.shift && !mods.alt && !mods.super` 时生效；`C` 且 `conv.ctrlCIsCopy && hasSelection` → `Copy`；`V` 且 `conv.ctrlVIsPaste` → `Paste`；其余 `None`。`terminal_page.cpp` 的 `onKeyEvent` 在 `encodeKey` 前调用裁决：`Copy` → 复制选区 + return；`Paste` → 粘贴 + return。

- [ ] **Step 4: 跑测试确认绿**

Run: `bash scripts/build-test-run.sh --dev --no-run --filter "file:test_terminal_keyencoder"`
Expected: PASS（既有 `keyencoderBasicKeys/CtrlCodes/Arrows` 全绿）。

- [ ] **Step 5: mac 回归 + 提交**

mac 上确认 Ctrl+Shift+C 仍发 `\x03`（裁决函数在 mac 惯例下恒 `None`）、Cmd+C 复制不受影响。

```bash
git add src/module_terminal/terminal_keyencoder.h src/module_terminal/terminal_keyencoder.cpp \
        src/module_terminal/terminal_page.cpp tests/test_modules/test_terminal_keyencoder.cpp
git commit -m "fix: Win 终端 Ctrl+Shift+C/V 走剪贴板，Ctrl+Shift 字母恢复 C0（bug510）"
```

---

### Task 4: 快照容量扩容与尺寸钳制（#508）

**Files:**
- Modify: `src/platform/terminal/swift-bridge/Sources/CortaBridge/CortaBridge.swift:41-45`（`BridgeLimits`）
- Modify: `src/platform/terminal/swift_bridge.cpp:17-18`（`kCellCapacity` 镜像）
- Modify: `src/platform/terminal/swift_bridge.h`（新增 `clampGridSize` 声明）
- Modify: `src/platform/terminal/swift_bridge.cpp`（`spawn` / `resize` 前钳制 + 告警）
- Test: `src/platform/terminal/swift-bridge/Tests/CortaBridgeTests/BridgeFeedTests.swift`（容量边界）
- Test: `tests/test_platform/test_terminal_bridge_clamp.cpp`（新增，钳制纯函数；同时加入 `tests/CMakeLists.txt`）

**Interfaces:**
- Produces:
  - `BridgeLimits.maxCols = 640`、`maxRows = 240`、`cellCapacity = 153600`（Swift）
  - `constexpr int32_t kMaxCols = 640; constexpr int32_t kMaxRows = 240; constexpr int32_t kCellCapacity = kMaxCols * kMaxRows;`（C++ 镜像，同值）
  - `struct GridSize { int32_t rows; int32_t cols; bool clamped; };`
  - `GridSize clampGridSize(int32_t rows, int32_t cols);`

- [ ] **Step 1: 写 Swift 失败测试**

在 `BridgeFeedTests.swift` 追加：以 `BridgeLimits.maxCols × BridgeLimits.maxRows` 创建会话，`snapshotCells` 返回 true；再以 `maxCols+1` 列创建，返回 false 且 meta 保持 0（不部分拷贝）。

Run: `swift test --package-path src/platform/terminal/swift-bridge`
Expected: 若容量仍为 200×60，边界用例语义正确但与新目标不符——把断言写成新上限常量，红。

- [ ] **Step 2: 写 C++ 失败测试**

新增 `tests/test_platform/test_terminal_bridge_clamp.cpp`：

```cpp
REGISTER_TEST(bridgeClampGrid,
              "terminal bridge: 网格尺寸钳制到镜像上限", {
    auto a = platform::clampGridSize(80, 24);
    DOCTEST_EQ(a.rows, 24); DOCTEST_EQ(a.cols, 80); DOCTEST_FALSE(a.clamped);
    auto b = platform::clampGridSize(400, 900);
    DOCTEST_EQ(b.rows, 240); DOCTEST_EQ(b.cols, 640); DOCTEST_TRUE(b.clamped);
    return true;
})
```

在 `tests/CMakeLists.txt` 的 `test_platform/` 段加入该文件。
Run: `bash scripts/build-test-run.sh --dev --no-run --filter "bridgeClampGrid"`
Expected: FAIL（符号未定义）。

- [ ] **Step 3: 实现扩容与钳制**

Swift 侧改 `BridgeLimits`。C++ 侧 `swift_bridge.cpp` 的 `kCellCapacity` 改为 640×240；新增 `clampGridSize`（超限钳制并置 `clamped`）；`spawn()` 与 `resize()` 内先钳制，`clamped` 时 `LOG_WARN` 一次（用会话内 bool 防循环告警）。

- [ ] **Step 4: 跑测试确认绿**

Run: `swift test --package-path src/platform/terminal/swift-bridge`；`bash scripts/build-test-run.sh --dev --no-run --filter "bridgeClampGrid"`
Expected: 两条均 PASS。

- [ ] **Step 5: 真机验证 + 提交**

外接显示器（或把字号调到 12px 使列数 >200）打开终端，确认正常渲染、无「帧跳过」日志；截图入 `evidence/`。

```bash
git add src/platform/terminal/swift-bridge/Sources/CortaBridge/CortaBridge.swift \
        src/platform/terminal/swift-bridge/Tests/CortaBridgeTests/BridgeFeedTests.swift \
        src/platform/terminal/swift_bridge.h src/platform/terminal/swift_bridge.cpp \
        tests/test_platform/test_terminal_bridge_clamp.cpp tests/CMakeLists.txt
git commit -m "fix: mac 终端快照容量 640x240，超限尺寸钳制并告警（bug508）"
```

---

### Task 5: 网页端待验收项收口（#497 / #499）

**Files:**
- Modify: 仅在 E2E 失败时改（`src/module_terminal/web/*`）
- Test: `tests/e2e/run-e2e.sh`（既有，不新增）

**Interfaces:**
- Consumes: `applySecurityHeaders`（`web_routes.cpp:14-27`，CSP 唯一响应头源）
- Produces: E2E 全绿记录，写回 Spec §三。

- [ ] **Step 1: 跑 E2E**

Run: `bash tests/e2e/run-e2e.sh`
Expected: 全步骤通过（echo-hi / reconnect / resync）。失败则记录失败步与日志片段。

- [ ] **Step 2: 核 CSP 与 favicon**

Run: `curl -sI http://127.0.0.1:<port>/ | grep -i content-security-policy`；浏览器控制台无 CSP 拦截报错、无 favicon 404。
Expected: 只有一条 CSP（响应头），无 meta 重复。

- [ ] **Step 3: 失败则修，修完重跑 Step 1**

只改致因（回显端序、票据 TTL、重连前的状态重置），不改协议形状。

- [ ] **Step 4: 记录结论并提交**

Spec §三 中 #497/#499 结论写「E2E 全绿（日期）+ 命令」；E2E 日志入 `evidence/`。

```bash
git add docs/superpowers/specs/2026-11-25-terminal-completion-design.md
git commit -m "docs: 网页终端 E2E 收口（bug497/499）"
```

---

## 收口（M1 结束）

- [ ] 全量一次：`bash scripts/build-test-run.sh`（不带 `--dev`），确认 doctest + probe + 守卫链全绿。
- [ ] mac 真机走查：`vim` / `htop` / `tmux` 配色与鼠标（着色部分）+ 外接屏 12px 渲染。
- [ ] Windows 侧看 CI：`windows-2025` 的 Release 构建 + doctest 全量通过。
- [ ] 用一张 `feat:` 提交合并本批（或保留分条提交，视 review 结论）。
