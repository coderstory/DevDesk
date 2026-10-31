# EUI-NEO 上游 PR 方案

> **过期横幅（2026-10-30 时效审计）**：内容基线为 2026-09-21 的能力盘点，其后 EUI-NEO 已新增 `trayMenu`/`onTrayMenu` 等自定义托盘菜单 API——文内托盘相关条目已被上游实现部分兑现，动手前必须先对照 `EUI-NEO/include/eui/app.h` 现状重审，不得按本文直接开工。审计出处：`docs/superpowers/research/2026-10-30-docs-freshness-audit.md`。

> **目的**：基于 DevDesk 实际使用 EUI-NEO 的经验，识别 EUI-NEO 上游可以改进的点，准备提交 PR 反哺社区。
> **调研依据**：[`docs/EUI-NEO-CAPABILITIES.md`](EUI-NEO-CAPABILITIES.md) 中识别的 5 个硬限制 + 5 个隐藏陷阱。
> **生成日期**：2026-09-21

---

## 0. 优先级评估总览

| 候选改进 | 投入产出 | 上游易接受 | 通用性 | DevDesk 收益 | 工作量 | **选定** |
|----------|----------|------------|--------|--------------|--------|----------|
| **fontWeight runtime 切换** | ⭐⭐⭐⭐⭐ | ✅ 小改动 | ✅ 所有 UI 用户 | ⚠️ 间接（去 hack） | S（半天） | ✅ **P0** |
| **全局 OS hotkey** | ⭐⭐⭐ | ✅ 中等 | ⚠️ 切工具类应用 | ⚠️ 间接 | M（1-2 天） | ✅ **P1** |
| **OS 文件拖入** | ⭐⭐ | ⚠️ 大改动 | ❌ 仅文件类应用 | ⚠️ 间接 | M-L（2-3 天） | ❌ 暂缓 |
| drop target API | ⭐⭐⭐ | ⚠️ 大改动 | ⚠️ 拖拽应用 | ❌ DevDesk 自实现 | L（3-5 天） | ❌ 暂缓 |
| macOS 27 Liquid Glass | ⭐⭐ | ❌ 平台依赖 | ⚠️ macOS 用户 | ❌ 不急 | XL（1-2 周） | ❌ 太重 |
| 自定义托盘菜单 | ⭐⭐ | ⚠️ 4 后端 | ⚠️ 工具类 | ❌ DevDesk 不急 | M | ❌ 暂缓 |
| Toast 队列 | ⭐⭐ | ✅ 小改动 | ✅ 所有用户 | ❌ 不急 | S | ❌ 不急 |
| Tab 焦点循环 | ⭐ | ✅ 中等 | ✅ 表单应用 | ❌ 不急 | M | ❌ 不急 |

**选定 2 个**：`fontWeight runtime 切换`（P0）和`全局 OS hotkey`（P1）。

---

## 1. P0 — `fontWeight()` runtime 真正切换字重 face

### 1.1 问题描述

**现状**（[EUI-NEO-CAPABILITIES.md §0 隐藏陷阱](../EUI-NEO-CAPABILITIES.md)）：
- `ui.text(...).fontWeight(700)` 在代码层面接受 100..900 数值
- 但 runtime **不真正切换字重 face** —— 只能渲染打包的那一份字重（默认 `JingNanJunJunTi-Bold-2.ttf` 是 Bold）
- 即使打包多份 TTF，**源码层面就不支持** —— `resolveFontPath` 的 `fontWeight >= 600` 分支和 `else` 分支**返回相同路径**

**影响**：
- DevDesk 的页面标题、正文、按钮文字**全都是 Bold**，无法做字重层级
- 所有 EUI-NEO 用户用 Bold/Regular/Medium 字重区分配色，**完全做不到**
- DevDesk 只能通过 `opacity()` / 颜色对比做层级

**用户场景**：
```cpp
// 现在：都是 Bold（即使设 400）
ui.text("h1").fontSize(48).fontWeight(800);   // Bold
ui.text("body").fontSize(15).fontWeight(400); // 还是 Bold（应该 Regular）
ui.text("sub").fontSize(13).fontWeight(300);  // 还是 Bold（应该 Light）
```

### 1.2 方案设计

**目标 API**：
```cpp
ui.text("body").fontWeight(300);  // 实际渲染 Light/Regular weight
ui.text("h1").fontWeight(800);    // 实际渲染 Heavy/Bold weight
```

**实现策略**（最小改动、向后兼容）：

1. **新增 `resolveSystemUiFontPathForWeight(int fontWeight)`**
   - macOS: 优先 `SFNS-Bold.ttf` / `SFNS-Heavy.ttf` / `SFNS-Light.ttf` / `SFNS.ttf`（variable fallback）
   - Windows: 优先 `segoeuib.ttf` / `segoeuil.ttf` / `segoeui.ttf`
   - Linux: 优先 `NotoSans-Bold.ttf` / `NotoSans-Light.ttf` / `NotoSans-Regular.ttf` / VF fallback

2. **修改 `resolveFontPath()`**
   - `fontWeight >= 600` → `resolveSystemUiFontPathForWeight(weight)`
   - `fontWeight <  400` → `resolveSystemUiFontPathForWeight(weight)`
   - `fontWeight >= 400 && < 600` → 现有默认 UI 字体路径

3. **修改 `fontStackCacheKey()` 包含 `fontWeight`**
   - 当前：`fontPath + "#" + fontSize64`
   - 改后：`fontPath + "#" + fontWeight + "#" + fontSize64`
   - 这样切换 fontWeight 时触发字体重新加载

4. **新增 platform 特定的字重 fallback 路径**（在 `loadSharedFontStack` 里的 `addLazyFallback`）

5. **新增 `examples/font_weight.cpp`** 演示用法

### 1.3 修改文件

| 文件 | 改动类型 | 行数估算 |
|------|----------|----------|
| `core/render/text.cpp` | 修改 | ~80 行（新增 helper + 改 resolveFontPath + 改 cache key）|
| `core/render/text.h` | 修改 | ~5 行（如果公开 helper）|
| `tests/unit/font_weight_resolution.cpp` | 新增 | ~80 行（测试不同 weight 解析路径）|
| `docs/DSL.md` | 修改 | ~20 行（更新 fontWeight 说明）|
| `examples/font_weight.cpp` | 新增 | ~60 行（demo）|

**总计**：~245 行（含测试 + demo），核心改动只有 ~80 行。

### 1.4 影响范围（破坏性 vs 增量）

✅ **完全增量（向后兼容）**

- 默认行为不变：`fontWeight(400)` 或不设置 `fontWeight` 时仍用现有字体路径
- 已有应用不受影响：所有现有 `fontWeight` 调用都会被新的代码重新选择路径
- API 不变：没有删除或重命名任何公开方法
- 缓存策略更精细：现有 cache 命中率不会下降（`fontWeight=400` 默认仍命中默认 cache key）

⚠️ **可能的视觉差异**：
- 之前所有 `fontWeight >= 600` 渲染 Bold，现在切到 `segoeuib.ttf` / `SFNS-Bold.ttf` / `NotoSans-Bold.ttf` —— **视觉上还是 Bold**（因为 macOS/Windows/Linux 的系统 Bold 和 `JingNanJunJunTi-Bold-2.ttf` 都是 Bold），所以**对用户无感知**
- `fontWeight < 600`（如 400、300）之前和之后都是 default UI font —— 也无感知

✅ **所以是真正的"零影响升级"**

### 1.5 测试计划

**单元测试**（`tests/unit/font_weight_resolution.cpp`）：
1. `resolveFontPath("PingFang SC", 400)` 和 `resolveFontPath("PingFang SC", 700)` 返回不同路径
2. `resolveFontPath("monospace", 700)` 返回默认 UI 路径（monospace 没有 weight 变体）
3. `resolveFontPath("/path/to/MyFont-Bold.ttf", 400)` 直接返回该路径（用户显式指定文件）
4. `fontStackCacheKey("/path", 16, 700)` 和 `fontStackCacheKey("/path", 16, 400)` 返回不同 key
5. `measureTextMetrics("abc", "PingFang SC", 16, 700)` 的 width > `measureTextMetrics("abc", "PingFang SC", 16, 400)` 的 width（Bold 比 Regular 宽）

**集成测试**（`examples/font_weight.cpp`）：
- 渲染窗口显示同一文本的 4 个字重（300/400/700/900）并截图对比

### 1.6 工作量估算

| 任务 | 时间 |
|------|------|
| 阅读现有 fontWeight 代码 + 设计 API | 30 分钟 |
| 改 `core/render/text.cpp` | 1.5 小时 |
| 写单元测试 | 1 小时 |
| 跑测试 + 编译验证 | 30 分钟 |
| 写 example demo | 1 小时 |
| 写 PR 描述 | 30 分钟 |
| **总计** | **~5 小时**（半天） |

---

## 2. P1 — 全局 OS hotkey（窗口失焦时收键盘事件）

### 2.1 问题描述

**现状**：
- 元素级 `onKeyEvent` 仅当**窗口聚焦**时触发
- 没有全局 hotkey API（窗口未聚焦时无法响应键盘）
- 跨平台实现需要用：`NSEvent addGlobalMonitorForEvents` (macOS) / `RegisterHotKey` (Win32) / `GTK StatusNotifier` / DBus (Linux)

**DevDesk 场景**：
- 切 Claude profile 的全局快捷键（Cmd+Shift+P）
- 打开 DevDesk 主窗口的全局快捷键（即使应用在后台）
- 全局搜索面板唤起

### 2.2 方案设计

**目标 API**：
```cpp
app::dslAppConfig()
    .globalHotkey("Cmd+Shift+P", [] { /* 触发 */ });
// 或
app::registerGlobalHotkey(app::Hotkey::Cmd | app::Hotkey::Shift | app::Hotkey::P, [] {
    openQuickSwitcher();
});
```

**实现策略**（3 平台）：

| 平台 | 实现 |
|------|------|
| macOS | `NSEvent addGlobalMonitorForEventsMatchingMask:handler:` + Carbon `RegisterEventHotKey`（仅 Cmd+Ctrl+Option+Shift） |
| Windows | `RegisterHotKey(hwnd, id, fsModifiers, vk)` + WM_HOTKEY 消息 |
| Linux | `org.freedesktop.portal.GlobalShortcuts` (DBus) + fallback 到 X11 `XGrabKey`（已废弃）/ `libei` (新) |

### 2.3 修改文件

| 文件 | 改动类型 |
|------|----------|
| `core/platform/hotkey_bridge.c` | 新增（~500 行 3 后端）|
| `include/eui/dsl_app.h` | 修改（新增 `globalHotkey()` 配置）|
| `core/dsl_runtime.h` + `core/runtime/app_config.cpp` | 修改（运行时注册）|
| `tests/unit/global_hotkey.cpp` | 新增 |
| `examples/global_hotkey.cpp` | 新增 |
| `docs/平台能力.md` | 修改（新增"全局热键"章节）|

**总计**：~700 行（含 3 平台实现）

### 2.4 影响范围

✅ **完全增量**
- 没有破坏性改动
- 现有应用不调用新 API 时完全不受影响
- 全局 hotkey 需要用户授权（macOS 辅助功能、Linux portal 权限），不能强行启用

⚠️ **需要权限**：
- macOS: 系统设置 → 辅助功能 → DevDesk
- Linux: GlobalShortcuts portal 自动弹窗
- Windows: 直接可用，无需授权

### 2.5 测试计划

**单元测试**：mock 平台 backend，验证注册/注销/重复注册/冲突检测

**集成测试**：在 demo 中注册 Cmd+Shift+G 触发打印日志，手动验证

### 2.6 工作量估算

| 任务 | 时间 |
|------|------|
| 设计 3 平台 API | 2 小时 |
| macOS 后端（Carbon + NSEvent）| 4 小时 |
| Windows 后端（RegisterHotKey）| 2 小时 |
| Linux 后端（Portal + fallback）| 6 小时（含 fallback 兼容性）|
| DSL AppConfig 集成 | 2 小时 |
| 测试 + example | 2 小时 |
| **总计** | **~2 天** |

### 2.7 优先级判断

**为什么选 P1 而不是 P0**：
- 实现工作量大（~2 天 vs 半天）
- 跨平台权限问题需要小心处理
- DevDesk 暂时可以"先聚焦窗口再触发快捷键"绕开

**但仍然值得做**：
- DevDesk 切 profile 的体验会大幅提升
- 对所有"全局工具型"应用（截图、剪贴板、自动化）都有用

---

## 3. 不选的候选及理由

### 3.1 ❌ OS 文件拖入

**为什么不选**：
- 实现复杂：需要 `NSDraggingDestination` (macOS) / `IDropTarget` (Windows) / `XDND` (Linux)
- 仅对"文件型应用"有用（DevDesk 只是辅助）
- 已可绕开：DevDesk 有 `+ 新建` 按钮 + `openFileDialog`
- 工作量 ~2-3 天，ROI 较低

### 3.2 ❌ drop target API

**为什么不选**：
- DevDesk 的拖拽是**业务级**（Todo 卡片跨列），EUI-NEO 提供基础 `onDrag` 就够
- DevDesk CLAUDE.md §"EUI-NEO 上游贡献政策"明确：业务级拖拽 DevDesk 自己实现
- 如果上游做，也应该做更通用的"注册 drop 接收 + 接受 payload 类型"API，scope 更大

### 3.3 ❌ macOS 27 Liquid Glass / vibrancy

**为什么不选**：
- 平台依赖：只对 macOS 27+ 有用
- 工作量大：需要 Metal 后端 + NSVisualEffectView bridge
- DevDesk 配色方案 B（暖墨底 + Clay 橘）已定型，不需要 vibrancy

---

## 4. 后续步骤

### 4.1 立即可做（本次任务已覆盖）

- [x] 选定 2 个改进点（fontWeight + global hotkey）
- [x] 为每个写详细 PR 方案
- [x] 实际写 fontWeight 的 patch（在 EUI-NEO 仓库，已合入本地 main，commit `bb5c965`）
- [x] 写 PR 描述（[`docs/archive/PR-DESCRIPTION.md`](archive/PR-DESCRIPTION.md)）
- [x] 一次性 PR 准备脚本 `scripts/prep-eui-neo-pr.sh` 已删除（2026-09-28 平台审计 N5：锁定已完成的旧 PR + 跨仓库 `git add` 风险），推送 / 开 PR 改手动，见 §4.2

### 4.2 需要用户操作（不自动做）

1. **Fork EUI-NEO** 到用户 GitHub：
   - 浏览器打开 https://github.com/sudoevolve/EUI-NEO
   - 点 "Fork"
   - 记录 fork URL（如 `https://github.com/coderstory/EUI-NEO.git`）

2. **添加 fork 为 remote**：
   ```bash
   cd ../EUI-NEO
   git remote add myfork https://github.com/coderstory/EUI-NEO.git
   ```

3. **创建分支 + 推送 + 开 PR**（patch 已在本地 main，commit `bb5c965`，直接切分支推送）：
   ```bash
   cd ../EUI-NEO
   git checkout -b feat/font-weight-runtime
   git push -u myfork feat/font-weight-runtime
   ```
   浏览器开 PR：https://github.com/sudoevolve/EUI-NEO/compare/main...feat/font-weight-runtime

4. **PR 评审通过后**：
   - DevDesk 移除自己的 hack（如果之前写过）
   - 在 DevDesk CLAUDE.md 的 ❌ 表中删除对应行
   - 更新 `docs/EUI-NEO-CAPABILITIES.md`

### 4.3 P1（global hotkey）实施前置

P1 需要先在 EUI-NEO 的 GitHub 开 issue 讨论 API 形状，避免大改后被上游拒绝。脚本会自动提示。

---

## 5. 风险评估

| 风险 | 影响 | 缓解 |
|------|------|------|
| EUI-NEO 上游不接受 fontWeight 改动 | PR 关闭 | API 完全增量；提供 example + 测试；明确写"DevDesk 实际使用场景" |
| 上游对修改风格有偏好 | 评审轮次增加 | 严格按 EUI-NEO 现有代码风格（命名 / 缩进 / 注释）|
| EUI-NEO 已有 PR 在做同样改动 | 冲突 | PR 描述中说明"@sudoevolve 想确认是否已有相关 PR/issue" |
| fontWeight 改动导致现有用户视觉差异 | 用户看到不同字体 | 默认 400 完全不变；>=600 改用系统 Bold（视觉上还是 Bold）|
| macOS 系统没有 SFNS-Bold.ttf（理论上不会但） | fallback 到现有 | firstExistingPath 已经支持 graceful fallback |
