# macOS 三项能力补齐（托盘 styled 菜单 / 磨砂 / 桌宠三项）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: 用 superpowers:executing-plans 或 subagent-driven-development 按任务逐条执行；步骤用 `- [ ]` 复选框跟踪。

> **执行状态补记（2026-09-28）**：本计划的 M0-1 / M1 / M2 / M3 已实装并落 EUI-NEO 本地 main（M0-2 的 `__APPLE__` 分支曾在 `7164d55` 被顺带删除，恢复进行中），**§10.2 的 Mac 实机验收未执行**；本文件的 `- [ ]` 复选框**未回填**，实施状态以 spec `2026-09-28-macos-three-features-design.md` 文首状态行为准。另：本计划成稿于 Windows 本机，文中「本机无 mac SDK」一类口径自 2026-09-28 起已失效（项目已搬回 macOS 本机）——macOS 侧构建是否通过、测试基线多少，见 `DevDesk/TODO.md`「macOS 构建环境」段。

**Goal:** 补齐 DevDesk 在 macOS 的三项平台能力：**M1** 托盘左右键分流 + styled 自绘菜单（与 Windows 一致，原生 NSMenu 仅降级）；**M2** 窗口磨砂三档（off/translucent/frosted）可用、失败保留 Transparent 降级；**M3** 桌宠三项 mac 等价能力（不进 Dock/⌘Tab、原生右键菜单、sleep/wake 电源事件）。全部走 **EUI-NEO 上游线**（修改 `../EUI-NEO/`）；DevDesk 侧除降级文案平台化与收尾版本 bump 外零改动（D-1、D-4）。

**Architecture:** 四层（main → modules → core → platform → EUI-NEO）。本批在 **EUI-NEO** 侧补平台真实现：M0 打通两处 macOS 原生句柄供给（GLFW `glfwGetCocoaWindow`）；M1 在 `tray_bridge.c` 的 `EUI_TRAY_APPKIT` 段（**唯一 mac 后端**，`3rd/tray` 的 `TRAY_APPKIT` 分支 EUI 构建不编译，§13-6 已关案）给状态栏按钮挂 action + `sendActionOn:` 分流，钩子 `eui_tray_menu_requested` 复用既有 DSL→DevDesk 登记链；M2/M3 新建四个 `*_macos.mm`（ObjC++ 平台实现，接口签名与 stubs 完全一致，stub 收窄给非 Apple 平台），顶层 CMake 四族 `if(WIN32)/else()` 拆 `elseif(APPLE)` + `if(APPLE)` 守卫下 `enable_language(OBJCXX)`。DevDesk 侧：`settings_config.cpp` 降级文案平台化（§5.3 案 B 默认，零跨仓依赖）+ 收尾 `DEVDESK_EUI_NEO_MIN_REV` bump 两处同步。

**Tech Stack:** C++20 / CMake 4.3.3 + Ninja + MSYS2 MinGW64 + ccache（成稿时的本机 = Windows 侧验证环境；2026-09-28 起本机是 macOS/arm64，见文首补记）/ Objective-C++（`.mm` ×4 首进 EUI 编译链，仅 Apple 平台编译）/ AppKit（`NSVisualEffectView`、`NSStatusItem`+`NSStatusBarButton`、`NSMenu popUpMenuPositioningItem:atLocation:inView:`、`NSWorkspace` 通知）/ GLFW 3.4 内置 `glfwGetCocoaWindow`/`glfwGetCocoaView`（`3rd/glfw` 已随 `glfw` target 提供头文件，两个 M0 文件已 include `<GLFW/glfw3native.h>`）。

**Spec:** `docs/superpowers/specs/2026-09-28-macos-three-features-design.md`（**V2**，commit `8a5a8b2`，265 行）—— 计划与 spec 逐条一致；两者冲突以 spec 为准并回报。关联 spec：`2026-09-20-devdesk-design.md`（权威）、`2026-09-22-eui-window-frost-design.md`（磨砂全链）、`2026-09-22-desktop-pet-design.md`（桌宠）。

## Global Constraints

- **执行方式**：本批每个 M 的 EUI-NEO 改动走「隔离 worktree 产 patch → 非隔离线 apply + 编译（已有隔离线证据时免重编）+ 原子 commit」；DevDesk 侧改动（文案、收尾 bump）走 DevDesk 侧 worktree 线。**主工作树禁跑构建**；涉及编译/测试的子任务必须 `isolation: "worktree"`。
- **命令**：DevDesk 构建 + 定向测试用 `bash scripts/build-test-run.sh --no-run --filter <子串>`（worktree 首建自动 configure，`EUI_NEO_DIR` 自动解析；禁全量重建、禁全量套件）。EUI-NEO 侧本机可编译的文件（M0 两个 `.cpp`、M1 的 `tray_bridge.c` Win 分支）以隔离线 eui-mock 全量编译 + 定向用例为验证；**mac 专属段（`__APPLE__` 分支 / `.mm`）成稿时（Windows 本机）无 mac SDK、编译不到，编译正确性只留实机验证**（spec §12-5），不得谎称本机已编译。（2026-09-28 补记：本机已切回 macOS，「无 mac SDK」的前提失效——macOS 侧可否编译以本机构建结果为准，见 `DevDesk/TODO.md`「macOS 构建环境」段。）
- **不抛 C++ 异常**：本批不引入任何新的 C++ 异常路径；`.mm` 内 `@try/@catch` 仅用于兜底 AppKit/ObjC 异常（Tahoe 风险门），不是 C++ 异常。
- **平台纪律**：平台实现按文件拆分（`*_macos.mm`），业务代码零平台宏；新实现接口签名与 stubs 逐字一致。
- **代码注释一律中文**（技术专有名词如 `NSVisualEffectView` 保留英文）；行尾 LF。
- **范围纪律**：只做 spec §8 清单内的改动；`3rd/tray/tray.h` **不改**（§4.3）；不引任何 vibrancy/crate（D-2）；不做 Linux 对等实现、不做 mac CI（D-7、§9）。
- **同一文件同一时刻只有一个写者**：`EUI-NEO/CMakeLists.txt` 被 M2 与 M3 共用——两者串行执行（M2 合入后再起 M3），禁止并行同写。
- **显式 pathspec**：一切 commit 用 `git add <精确文件清单>`，禁 `git add -A`。
- Spec §13-1（降级文案案 A/B）待用户拍板：本计划**默认实施案 B**（中性文案一行、零跨仓依赖、DevDesk 线可独立先行）；用户拍板案 A 时改走「EUI 先加 hint 接口 → DevDesk 接线」串行线（见 M2 任务注）。

## 文件结构一览（改动落点）

### EUI-NEO 侧（`D:/project/gui/EUI-NEO/`，全部经 EUI 补丁线）

| 任务 | 文件 | 动作 |
|------|------|------|
| M0 | `core/app/glfw_app_main.cpp` | 改：`nativeWindowHandle`（L252-259）`#else` 拆 `#elif defined(__APPLE__)` 返 `glfwGetCocoaWindow(window)`（§3 M0-1） |
| M0 | `core/window/window_backend.cpp` | 改：GLFW 变体 `nativeWindowInfo`（L831-843）加 `__APPLE__` 分支填 `result.platformWindow`（§3 M0-2；SDL2 变体 L502-525 已有 `__APPLE__`，不动） |
| M1 | `core/platform/tray_bridge.c` | 改（M1 唯一实作点，§4.1）：`EUI_TRAY_APPKIT` 段——`EUITrayTarget` 加 `trayButtonClicked:` 方法（左右键分流 + 坐标翻转 + 调钩子 + 0 时 `popUpStatusItemMenu:`）；`eui_tray_init` 按钮挂 target/action + `sendActionOn:(NSLeftMouseUpMask\|NSRightMouseUpMask)`（L275 后插 3 行）；§4.2 预备改法 = 移除 L234/L276 两处 `setMenu:` |
| M2 | `core/platform/window_effect_macos.mm` | **新建**：`applyWindowEffect` / `applyTitleBarAppearance`（NSVisualEffectView，§5.2） |
| M2 | `CMakeLists.txt` | 改：`project()` 后加 `if(APPLE) enable_language(OBJCXX) endif()`；L266-270 window_effect 选源拆 `elseif(APPLE)` |
| M3 | `core/platform/window_style_macos.mm` | **新建**：`applyWindowStyleFlags`（collectionBehavior 等价，§6.1） |
| M3 | `core/platform/context_menu_macos.mm` | **新建**：`showContextMenu`（NSMenu popUp，§6.2） |
| M3 | `core/platform/power_events_macos.mm` | **新建**：`setSystemPowerHandler` / `installSystemPowerNotifications` / `uninstallSystemPowerNotifications`（NSWorkspace sleep/wake，§6.3；不做 IOKit 电池，§9） |
| M3 | `CMakeLists.txt` | 改：L273-294 三族选源各拆 `elseif(APPLE)`（与 M2 同文件——串行执行或合并派单） |
| — | `3rd/tray/tray.h` | **不改**（EUI 构建不编译 `TRAY_APPKIT` 分支，§4.3） |

### DevDesk 侧（`D:/project/gui/DevDesk/`，全部走 DevDesk 侧 worktree 线，§7-6）

| 任务 | 文件 | 动作 |
|------|------|------|
| M2 | `src/module_settings/settings_config.cpp` | 改：`windowEffectDegradedHint` frosted 分支文案平台化（L215，§5.3 案 B 默认；案 A 待 §13-1 拍板） |
| 收尾 | `CMakeLists.txt` + `CLAUDE.md` | 改：`DEVDESK_EUI_NEO_MIN_REV` bump 两处同步（当前 `269a8c4`，CMakeLists.txt:53 + CLAUDE.md「常用命令」段） |

M1 预期 DevDesk 侧零改动（`main.cpp` L566-575 `.onTrayMenu` 左键 ShowMainWindow / 右键 `showTrayStyledMenu`（L389-408，styled 调用在 L400）已就绪，mac 端接上分流后同一回调路径自动生效）。

## 执行时序（依赖图）

```
M0（EUI 补丁线 ①） —— 串行先行，M2/M3 硬前置
M1（EUI 补丁线 ②） —— 可与 M0 并行（不同文件）
M2（EUI 补丁线 ③ + DevDesk 文案线） —— 依赖 M0；与 M3 串行（共享 CMakeLists）
M3（EUI 补丁线 ④） —— 依赖 M0；与 M2 串行
收尾（DevDesk 线）—— M0-M3 全部合入 EUI-NEO main 后：bump MIN_REV + Win 全量回归
```

每个任务完整走 §「跨仓库交付线」后并入 EUI-NEO main；M0 未合入前不得起 M2/M3 的 EUI 线（实现依赖句柄非空才不降级）。

---

## Task M0: macOS 原生句柄供给（EUI 补丁线，串行先行）

**Files:**
- Modify: `EUI-NEO/core/app/glfw_app_main.cpp`
- Modify: `EUI-NEO/core/window/window_backend.cpp`

**Interfaces:** 零 API 变化（纯内部句柄供给修复；`NativeWindowInfo::platformWindow` 语义不变）。DevDesk 零改动。

- [ ] **Step 1: `glfw_app_main.cpp` `nativeWindowHandle` 加 `__APPLE__` 分支**

`EUI-NEO/core/app/glfw_app_main.cpp:252-259`（当前源码）：

```cpp
// ============ 标题栏外观联动（磨砂设计文档 Phase A）============
// 非 Windows 平台 nativeWindowHandle 返回 nullptr，
// core::platform::applyTitleBarAppearance 内部静默降级。
void* nativeWindowHandle(GLFWwindow* window) {
#if defined(_WIN32)
    return glfwGetWin32Window(window);
#else
    (void)window;
    return nullptr;
#endif
}
```

改为（`#else` 拆 `#elif defined(__APPLE__)`，`#else` 分支字节不动）：

```cpp
// ============ 标题栏外观联动（磨砂设计文档 Phase A）============
// macOS 返回 NSWindow*（M0：macos-three-features 设计 §3）；
// 其余平台返回 nullptr，core::platform::applyTitleBarAppearance 内部静默降级。
void* nativeWindowHandle(GLFWwindow* window) {
#if defined(_WIN32)
    return glfwGetWin32Window(window);
#elif defined(__APPLE__)
    // GLFW 官方 macOS 原生窗口查询 API（返回 NSWindow*，见 3rd/glfw 的
    // glfwGetCocoaWindow）；M2 磨砂 applyWindowEffect 与标题栏外观消费此句柄
    return glfwGetCocoaWindow(window);
#else
    (void)window;
    return nullptr;
#endif
}
```

`<GLFW/glfw3native.h>` 已在本文件 L15 include（`glfwGetWin32Window` 同源声明），无需新增 include。

- [ ] **Step 2: `window_backend.cpp` GLFW 变体 `nativeWindowInfo` 加 `__APPLE__` 分支**

`EUI-NEO/core/window/window_backend.cpp:831-843`（当前源码）：

```cpp
NativeWindowInfo nativeWindowInfo(Handle window) {
    NativeWindowInfo result;
    result.handle = window;
#if defined(_WIN32)
    // platformWindow = HWND（对齐 SDL2 路径的填充语义）：原生菜单/DWM 等
    // core::platform 调用需要原生句柄，此前 GLFW 路径只回 GLFW 句柄，调用
    // 方无从取 HWND
    if (window != nullptr) {
        result.platformWindow = glfwGetWin32Window(static_cast<GLFWwindow*>(window));
    }
#endif
    return result;
}
```

改为（`#endif` 前插 `#elif defined(__APPLE__)` 分支）：

```cpp
NativeWindowInfo nativeWindowInfo(Handle window) {
    NativeWindowInfo result;
    result.handle = window;
#if defined(_WIN32)
    // platformWindow = HWND（对齐 SDL2 路径的填充语义）：原生菜单/DWM 等
    // core::platform 调用需要原生句柄，此前 GLFW 路径只回 GLFW 句柄，调用
    // 方无从取 HWND
    if (window != nullptr) {
        result.platformWindow = glfwGetWin32Window(static_cast<GLFWwindow*>(window));
    }
#elif defined(__APPLE__)
    // platformWindow = NSWindow*（M0：macos-three-features 设计 §3）：M3 的
    // applyWindowStyleFlags / showContextMenu 经 nativeWindowInfo(handle)
    // .platformWindow 取原生窗口（桌宠 pet_window.cpp → app::openWindow 链路）
    if (window != nullptr) {
        result.platformWindow = glfwGetCocoaWindow(static_cast<GLFWwindow*>(window));
    }
#endif
    return result;
}
```

`<GLFW/glfw3native.h>` 已在本文件 GLFW 段 L730 include。同文件 SDL2 变体 `nativeWindowInfo`（L502-525）已含 `__APPLE__`（`SDL_SYSWM_COCOA`），**不动**（DevDesk 走 GLFW 后端，SDL2 变体不影响本批）。

- [ ] **Step 3: 实机诊断钩子（可选，验证用）**

在 `applyWindowEffectToWindow`（glfw_app_main.cpp:269-276）入口加一行 `__APPLE__` 专属诊断日志（实机清单 M0 ① 用；实机确认非空后由收尾任务移除）：

```cpp
core::platform::WindowEffect applyWindowEffectToWindow(GLFWwindow* window, core::platform::WindowEffect desired) {
#if defined(__APPLE__)
    // 临时诊断（macos-three-features 计划 M0 Step 3）：实机确认 nativeWindowHandle 非空，
    // 验证完成后由收尾任务移除
    std::fprintf(stderr, "[eui] mac nativeWindowHandle: %p\n",
                 static_cast<void*>(nativeWindowHandle(window)));
#endif
    if (core::platform::applyWindowEffect(nativeWindowHandle(window), desired)) {
```

- [ ] **Step 4: 隔离线验证（Windows 回归面为零的实证）**

按「跨仓库交付线」第 1-2 步走：隔离 worktree agent 在 eui-mock 副本改上述两文件（Win 分支字节不动），`EUI_NEO_DIR=<worktree>/eui-mock` 跑 `bash scripts/build-test-run.sh --no-run --filter <定向子串>`（本批无新增用例，跑既有 `settings`/`menu`/`tray` 相关定向子串 + 确认 `Total` 无新增失败）→ 产 `M0-eui.patch`。预期：两文件为纯分支追加，Windows 编译路径零变化，全绿。

- [ ] **Step 5: 非隔离线 apply + commit**

按「跨仓库交付线」第 3 步：非隔离 agent 在 `D:/project/gui/EUI-NEO` 主树 `git apply --check M0-eui.patch` → `git apply` → 原子 commit（显式 pathspec）：

```bash
git -C D:/project/gui/EUI-NEO add core/app/glfw_app_main.cpp core/window/window_backend.cpp
git -C D:/project/gui/EUI-NEO commit -m "feat(macos): GLFW 路线原生句柄供给（DevDesk 需要 macOS 窗口效果/桌宠原生能力）
- nativeWindowHandle 加 __APPLE__ 分支返 glfwGetCocoaWindow（NSWindow*）
- nativeWindowInfo GLFW 变体加 __APPLE__ 分支填 platformWindow
- 纯分支追加，Windows/Posix 路径字节不变；M2/M3 的硬前置（macos-three-features 设计 M0）"
```

- [ ] **Step 6: 主会话审计**

读 diff 逐行核对：`#else` 仍返 nullptr、`__APPLE__` 仅两处、无夹带；`git -C D:/project/gui/EUI-NEO log -1 --stat` 范围正确；EUI-NEO main tip 推进、DevDesk 工作树零牵连。

---

## Task M1: 托盘 styled 菜单（EUI 补丁线）

**Files:**
- Modify: `EUI-NEO/core/platform/tray_bridge.c`（**M1 唯一实作点**；`3rd/tray/tray.h` 不改）

**Interfaces:** 零 API 变化（复用既有 `eui_tray_set_menu_requested_fn` / `eui_tray_menu_requested(x, y, left_button)` 钩子，L122-131；DSL 侧 `platform.cpp:747-763` 已注册，DevDesk `main.cpp:566-575` 已实现左右分流消费）。新增内部 ObjC 方法 `EUITrayTarget trayButtonClicked:`。

- [ ] **Step 1: `EUITrayTarget` 加方法声明**

`tray_bridge.c` L145-147（`EUI_TRAY_APPKIT` 段内）：

```objc
@interface EUITrayTarget : NSObject <NSApplicationDelegate>
- (void)itemSelected:(id)sender;
- (void)trayButtonClicked:(id)sender;   // M1：状态栏按钮 action（左右键分流，macos-three-features §4.1）
@end
```

- [ ] **Step 2: 实现 `trayButtonClicked:`（分流 + 坐标翻转 + 调钩子 + 原生降级）**

`@implementation EUITrayTarget` 块内（`itemSelected:` 方法之后）追加——action 必须是 target 对象上的方法，故用 ObjC 方法而非 C 静态函数（spec §4.1「新 static 函数」在本文件结构下的落地形态）：

```objc
// M1（2026-09-28 macos-three-features 设计 §4.1）：状态栏按钮点击分流。
// sendActionOn: 已把左右键都纳入 action；这里取当前事件类型判左右：
//   - eui_tray_menu_requested 返回非 0 → 上层（DSL handler → DevDesk onTrayMenu）
//     已接管本次菜单展示（styled 菜单），原生弹出被跳过——与 Win32 tray.h:
//     251-253 分流语义一致；
//   - 返回 0 / 未注册 → 显式弹出当前菜单对象（原生降级路径）。
// ⚠️ 坐标：Cocoa 屏幕坐标原点在主屏左下；EUI/上层坐标为左上原点 → y 翻转
//   （设计 §12-3：翻转方向为设计推断，实机验证后必要时微调一行）。
- (void)trayButtonClicked:(id)sender {
    (void)sender;
    NSEvent* event = [NSApp currentEvent];
    const BOOL is_right = (event != nil && [event type] == NSRightMouseUp);
    const NSPoint mouse = [NSEvent mouseLocation];
    const NSRect screen = [[NSScreen mainScreen] frame];
    const int x = (int)mouse.x;
    const int y = (int)(screen.size.height - mouse.y);
    if (eui_tray_menu_requested(x, y, is_right ? 0 : 1) == 0) {
        [g_status_item popUpStatusItemMenu:g_menu];
    }
}
```

- [ ] **Step 3: 按钮挂 target/action + sendActionOn**

`eui_tray_init` 内 `[button setToolTip:@"EUI NEO"];`（L275）之后、`[g_status_item setMenu:g_menu];`（L276）之前插 3 行：

```objc
        // M1（2026-09-28 macos-three-features 设计 §4.1）：按钮挂 target/action；
        // NSButton 默认只在左键按下时发 action，sendActionOn: 把右键一并纳入。
        // ⚠️ 挂 action 与 setMenu: 共存是 AppKit 已知争议点（§4.2，实机清单
        // M1 第 0 项）——若 action 不触发，切预备改法（Step 5 附录 A）。
        [button setTarget:g_target];
        [button setAction:@selector(trayButtonClicked:)];
        [button sendActionOn:(NSLeftMouseUpMask | NSRightMouseUpMask)];
```

- [ ] **Step 4: 保持菜单更新路径**

`eui_tray_menu_changed`（L227-236）**保持现状**：`eui_tray_rebuild_menu()` + `[g_status_item setMenu:g_menu]`（L234）——菜单内容仍随 `eui_tray_menu_changed`（DSL `setTrayMenu` → 桥内 `eui_tray_set_menu` → `eui_tray_menu_changed`）更新；分流职责全部在 `trayButtonClicked:`。**主路径下两处 `setMenu:`（L234 / L276）都保留。**

- [ ] **Step 5（附录 A，条件执行）：预备改法——挂 action 与 setMenu: 不共存**

仅当 Mac 实机验证第 0 项失败（点击只弹原生菜单、`trayButtonClicked:` 不触发）时切换，**一行级成本**：移除 L234 与 L276 两处 `[g_status_item setMenu:g_menu];`（menu 对象仍由 `eui_tray_rebuild_menu` 维护，仅不再自动悬挂），左/右键统一在 `trayButtonClicked:` 内分流，返回 0 时显式 `[g_status_item popUpStatusItemMenu:g_menu]`（Step 2 代码已含该调用，无需再改）。两者均不需 DevDesk/DSL 侧改动。

- [ ] **Step 6: 隔离线验证**

eui-mock 改 `tray_bridge.c` → DevDesk 定向构建 + 跑既有 tray 相关定向用例（Windows 编译 `EUI_TRAY_WINAPI` 段不受影响；APPKIT 段本机编译不到，留给实机）→ 产 `M1-eui.patch`。**预期单元**：Win 行为零变化（改动全在 `EUI_TRAY_APPKIT` 段内）。

- [ ] **Step 7: 非隔离线 apply + commit + 审计**

```bash
git -C D:/project/gui/EUI-NEO add core/platform/tray_bridge.c
git -C D:/project/gui/EUI-NEO commit -m "feat(macos): 托盘左右键分流（DevDesk 需要 macOS styled 托盘菜单）
- EUI_TRAY_APPKIT 段：状态栏按钮挂 target/action + sendActionOn 左右键
- trayButtonClicked: 分流：钩子非 0 跳原生弹出（styled 菜单路径），0 走
  popUpStatusItemMenu: 原生降级；坐标屏系翻转
- 仅改 tray_bridge.c；3rd/tray TRAY_APPKIT 分支 EUI 构建不编译、不动
- macos-three-features 设计 M1（§4.1/§4.2）"
```

---

## Task M2: 窗口磨砂（EUI 补丁线 + DevDesk 文案线）

> **前置：M0 已合入 EUI-NEO main**（`nativeWindowHandle` mac 返 NSWindow，否则恒降级 = 白做）。

**Files:**
- Create: `EUI-NEO/core/platform/window_effect_macos.mm`
- Modify: `EUI-NEO/CMakeLists.txt`（enable_language + window_effect 选源段）
- Modify: `DevDesk/src/module_settings/settings_config.cpp`（§5.3 文案——DevDesk 侧线，见 Step 6）

**Interfaces:** 签名与 `window_effect.h`（L41/L48）及 stub 逐字一致：

```cpp
bool applyTitleBarAppearance(void* nativeWindowHandle, const TitleBarAppearance& appearance);
bool applyWindowEffect(void* nativeWindowHandle, WindowEffect effect);
```

- [ ] **Step 1: 新建 `window_effect_macos.mm`（整文件逐字）**

```objc
// core/platform/window_effect_macos.mm —— macOS 窗口磨砂实现
//（2026-09-28 macos-three-features 设计 §5.2；磨砂全链见
// 2026-09-22-eui-window-frost-design.md）
//
// 不引任何 vibrancy crate（设计 D-2）：ObjC++ 直挂 NSVisualEffectView。
// Tahoe 风险门（设计 D-3）：挂载失败（异常/无效句柄）→ 返回 false → 上层既有
// 降级链（glfw_app_main applyWindowEffectToWindow → degradedWindowEffect →
// activeWindowEffect 回写）自动降级 Transparent，不崩、不阻塞。
#import <Cocoa/Cocoa.h>
#include <GLFW/glfw3native.h>

#include "window_effect.h"

namespace core::platform {

namespace {

// 已创建的材质视图（进程级复用；attach 时按目标窗口重挂）
NSVisualEffectView* g_effect_view = nil;

NSVisualEffectView* effectView() {
    if (g_effect_view == nil) {
        g_effect_view = [[NSVisualEffectView alloc] initWithFrame:NSZeroRect];
        // classic 材质组合（设计 §5.2；⚠️ 材质观感待 Mac 实机定，spec §13-2）：
        // HUDWindow 深色高对比 / UnderWindowBackground 通用背景二选一（可换）
        [g_effect_view setMaterial:NSVisualEffectMaterialHUDWindow];
        [g_effect_view setBlendingMode:NSVisualEffectBlendingModeWithinWindow];
        [g_effect_view setState:NSVisualEffectStateActive];
        [g_effect_view setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    }
    return g_effect_view;
}

// 把材质视图垫到 contentView 最底层（GL 渲染视图之下；GL 侧需透明帧缓冲/
// wantsLayer 才能透出材质——⚠️ 合成顺序待实机验证，spec §12-2）
void attachEffectView(NSWindow* nsWindow, bool enabled, float alpha) {
    if (nsWindow == nil) {
        return;
    }
    NSView* content = [nsWindow contentView];
    NSVisualEffectView* view = effectView();
    if (!enabled) {
        [view removeFromSuperview];
        return;
    }
    [view setAlphaValue:alpha];
    [view setFrame:[content bounds]];
    if ([view superview] != content) {
        // positioned:NSWindowBelow + relativeTo:nil = 插到 subviews 最底层
        [content addSubview:view positioned:NSWindowBelow relativeTo:nil];
    }
}

} // namespace

bool applyWindowEffect(void* nativeHandle, WindowEffect effect) {
    if (nativeHandle == nullptr) {
        return false;   // M0-1 未合入前恒发生：空句柄 → 走既有降级链
    }
    NSWindow* nsWindow = glfwGetCocoaWindow(static_cast<GLFWwindow*>(nativeHandle));
    if (nsWindow == nil) {
        return false;
    }
    @autoreleasepool {
        @try {
            switch (effect) {
            case WindowEffect::None:
                attachEffectView(nsWindow, false, 1.0f);
                return true;   // 「关掉 backdrop」本身是受支持操作
            case WindowEffect::Transparent:
                // 半透档：材质视图整体半透明（数值 ⚠️ 待 Mac 实机定，暂取 0.4）
                attachEffectView(nsWindow, true, 0.4f);
                return true;
            case WindowEffect::Acrylic:
            case WindowEffect::Mica:
            case WindowEffect::MicaAlt:
                // Frosted 语义 = Acrylic 档（DevDesk settings_config 映射）；
                // Mica 族同挂同一材质视图（观感差异并入实机复核项）
                attachEffectView(nsWindow, true, 1.0f);
                return true;
            }
        } @catch (NSException*) {
            // Tahoe 风险门：AppKit 异常（材质不受支持等）→ false → 既有降级链
            return false;
        }
    }
    return false;
}

bool applyTitleBarAppearance(void* /*nativeHandle*/,
                             const TitleBarAppearance& /*appearance*/) {
    // macOS 标题栏深浅由系统深色模式自动联动（NSWindow 无独立 API）；
    // customColor/titlebarAppearsTransparent 联动设计 §9 明确不做
    // → 恒 false（静默不刷日志，与既有 stub 语义一致）
    return false;
}

} // namespace core::platform
```

- [ ] **Step 2: `CMakeLists.txt` 的 `enable_language(OBJCXX)`（必须 `if(APPLE)` 守卫）**

`project(EUI-NEO VERSION 0.7.0)`（L2）之后立即插入：

```cmake
if(APPLE)
    # 磨砂/桌宠平台实现的 *_macos.mm（ObjC++）首次进编译链（macos-three-features
    # 设计 §8.1）。仅 Apple 平台启用：Windows/Linux configure 不探测 ObjC++
    # 编译器（本机无 mac SDK；Windows 回归面为零，设计 §10.1/§12-5）
    enable_language(OBJCXX)
endif()
```

> 守卫理由：不守卫会让配置在 Windows 上尝试探测 OBJCXX 编译器（CMake 在无 Apple SDK 平台上通常报找不到编译器 → configure 失败），直接违反设计 §10.1「Windows 回归必须全绿」。这是对 spec「顶层 project() 之后加 enable_language(OBJCXX)」的落点细化，意图一致（仅 Apple 需要）。

- [ ] **Step 3: `CMakeLists.txt` window_effect 选源拆 `elseif(APPLE)`**

L265-270（当前源码）：

```cmake
# 标题栏外观（Phase A：深浅主题联动）。macOS/Linux 走 stub，Phase E 再分平台实现
if(WIN32)
    list(APPEND CORE_SOURCES core/platform/window_effect_win.cpp)
else()
    list(APPEND CORE_SOURCES core/platform/window_effect_stub.cpp)
endif()
```

改为：

```cmake
# 标题栏外观（Phase A：深浅主题联动）+ 窗口磨砂（Phase C/E）。
# Windows 走 DWM；macOS 走 NSVisualEffectView（macos-three-features 设计 §5.2，
# Tahoe 风险门：失败返回 false 由上层降级链收口）；其余平台走 stub
if(WIN32)
    list(APPEND CORE_SOURCES core/platform/window_effect_win.cpp)
elseif(APPLE)
    list(APPEND CORE_SOURCES core/platform/window_effect_macos.mm)
else()
    list(APPEND CORE_SOURCES core/platform/window_effect_stub.cpp)
endif()
```

- [ ] **Step 4: 隔离线验证**

eui-mock 内新建 `.mm` + 改 CMakeLists → `EUI_NEO_DIR=eui-mock` 跑定向构建（Windows 只验证 configure 仍绿——`.mm` 不在 Win 源列表；stub 未受影响）+ `--filter settings`/`window` 定向用例 → 产 `M2-eui.patch`。预期：Windows configure 新增 `if(APPLE)` 分支不执行、无任何新编译错误；`.mm` 本体仅语义/一致性审查（编译正确性留实机，§12-5）。

- [ ] **Step 5: 非隔离线 apply + commit + 审计**

```bash
git -C D:/project/gui/EUI-NEO add core/platform/window_effect_macos.mm CMakeLists.txt
git -C D:/project/gui/EUI-NEO commit -m "feat(macos): 窗口磨砂 NSVisualEffectView 实现（DevDesk 需要 macOS 磨砂三档）
- 新建 window_effect_macos.mm：applyWindowEffect 三档映射 + 材质视图垫底；
  applyTitleBarAppearance 恒 false（§9 不做联动）
- CMakeLists：if(APPLE) enable_language(OBJCXX) + window_effect 选源拆 elseif(APPLE)
- Tahoe 风险门：失败返 false 走既有降级链，不崩不阻塞
- macos-three-features 设计 M2（§5）"
```

- [ ] **Step 6: DevDesk 降级文案平台化（DevDesk 侧 worktree 线，§7-6；默认案 B）**

> spec §13-1 待用户拍板：**案 B**（本计划默认）= 中性文案去掉 OS 专属信息，一行改、零跨仓依赖、DevDesk 线独立先行；**案 A** = 新增平台接口 `windowEffectUnsupportedHint`（`*_win.cpp` 返回现文案 / `*_macos.cpp` 返回 mac 文案 / `*_posix.cpp` 通用），需 EUI 先加 hint 接口经补丁线合入再接线（串行依赖，另行派单；若拍板案 A，执行顺序为「EUI hint 接口补丁线 → DevDesk 接线」，EUI hint 接口可并入 M2 补丁或追加 M2.5）。

`DevDesk/src/module_settings/settings_config.cpp:210-218` frosted 分支，`:215` 当前文案：

```cpp
        if (active == WindowEffect::Transparent) {
            return "当前系统不支持磨砂（需 Windows 11 22H2+），已回退为半透";
        }
```

案 B 改为（唯一改动，其余分支已是中性文案不动）：

```cpp
        if (active == WindowEffect::Transparent) {
            // 中性降级文案（macos-three-features 设计 §5.3 案 B）：
            // 去掉「需 Windows 11 22H2+」——Win 专属信息在 mac 路径是误导
            return "当前系统不支持磨砂，已回退为半透";
        }
```

DevDesk 侧线执行：worktree 子任务原子 commit（显式 pathspec）→ 主会话审计合并。定向构建 `--filter settings` 验证。

---

## Task M3: 桌宠三项（EUI 补丁线）

> **前置：M0 已合入**（`nativeWindowInfo` mac 填 NSWindow，否则三项全部收 nullptr）。与 M2 串行（共享 `CMakeLists.txt`）。

**Files:**
- Create: `EUI-NEO/core/platform/window_style_macos.mm`
- Create: `EUI-NEO/core/platform/context_menu_macos.mm`
- Create: `EUI-NEO/core/platform/power_events_macos.mm`
- Modify: `EUI-NEO/CMakeLists.txt`（L273-294 三族选源各加 `elseif(APPLE)`）

**Interfaces:** 签名与三头文件及 stubs 逐字一致：

```cpp
// window_style.h
bool applyWindowStyleFlags(void* nativeWindowHandle, const WindowStyleFlags& flags);
// context_menu.h
std::optional<std::vector<int>> showContextMenu(void* nativeWindowHandle,
                                                const std::vector<ContextMenuItem>& items);
// power_events.h
void setSystemPowerHandler(std::function<void(SystemPowerEvent)> handler);
void installSystemPowerNotifications(void* nativeWindowHandle);
void uninstallSystemPowerNotifications();
```

- [ ] **Step 1: 新建 `window_style_macos.mm`（整文件逐字）**

```objc
// core/platform/window_style_macos.mm —— macOS 窗口样式
//（2026-09-28 macos-three-features 设计 §6.1：hideFromTaskbar 等价）
// macOS 窗口本就不进 Dock（无「任务栏」概念）；「任务栏隐藏」的等价语义 =
// 常驻全部 Space + 不参与 ⌘Tab 循环（mach 桌宠在换 Space / 应用循环时不可见）。
#import <Cocoa/Cocoa.h>
#include <GLFW/glfw3native.h>

#include "window_style.h"

namespace core::platform {

bool applyWindowStyleFlags(void* nativeHandle, const WindowStyleFlags& flags) {
    if (nativeHandle == nullptr) {
        return false;
    }
    NSWindow* nsWindow = glfwGetCocoaWindow(static_cast<GLFWwindow*>(nativeHandle));
    if (nsWindow == nil) {
        return false;
    }
    if (flags.toolWindow) {
        // 桌宠设计 G5（DevDesk pet_window.cpp:678 .hideFromTaskbar(true) → toolWindow）
        // ⚠️ 该组合是否满足「不进 Dock / ⌘Tab 不可见」预期待 Mac 实机复核
        //（设计 §13-4）
        [nsWindow setCollectionBehavior:
            (NSWindowCollectionBehaviorCanJoinAllSpaces |
             NSWindowCollectionBehaviorIgnoresCycle)];
    } else {
        [nsWindow setCollectionBehavior:NSWindowCollectionBehaviorDefault];
    }
    return true;
}

} // namespace core::platform
```

- [ ] **Step 2: 新建 `context_menu_macos.mm`（整文件逐字）**

```objc
// core/platform/context_menu_macos.mm —— macOS 原生右键菜单
//（2026-09-28 macos-three-features 设计 §6.2）
// 桌宠右键菜单由自绘 contextMenu 组件升级为原生 NSMenu 弹出；
// 命令 = 扁平命令路径表下标（对齐 context_menu.h / win 实现语义）。
#import <Cocoa/Cocoa.h>
#include <GLFW/glfw3native.h>

#include <optional>
#include <vector>

#include "context_menu.h"

namespace core::platform {

namespace {

// 弹出期间记录选中结果（popUp 同步返回，主线程使用安全）
const std::vector<std::vector<int>>* g_popup_paths = nullptr;
std::vector<int> g_selected_command_path;
bool g_selected_any = false;

// 选中回调目标：action 必须是对象方法（C 函数不能作 selector），
// 与 tray_bridge 的 EUITrayTarget 同模式
@interface EUIContextMenuTarget : NSObject
- (void)itemPicked:(id)sender;
@end

@implementation EUIContextMenuTarget
- (void)itemPicked:(id)sender {
    NSMenuItem* item = (NSMenuItem*)sender;
    const NSInteger idx = [item tag];
    g_selected_any = true;
    g_selected_command_path =
        (g_popup_paths != nullptr && idx >= 0 &&
         idx < (NSInteger)g_popup_paths->size())
            ? (*g_popup_paths)[(size_t)idx]
            : std::vector<int>{};
}
@end

EUIContextMenuTarget* g_context_menu_target = nil;

// 深度优先构建 NSMenu；可选中项分配顺序与 contextMenuCommandPaths 的 DFS
// 完全一致（分隔线不占命令位），两侧规则对齐（win 实现里同样的 debug 断言）。
NSUInteger g_next_command_index = 0;

void buildMenuItems(NSMenu* menu, const std::vector<ContextMenuItem>& items,
                    id target) {
    for (const ContextMenuItem& item : items) {
        if (item.text == "-") {
            [menu addItem:[NSMenuItem separatorItem]];
            continue;
        }
        NSString* title = [NSString stringWithUTF8String:item.text.c_str()];
        const bool leaf = item.children.empty();
        NSMenuItem* mi =
            [[NSMenuItem alloc] initWithTitle:(title != nil ? title : @"")
                                       action:leaf ? @selector(itemPicked:) : nil
                                keyEquivalent:@""];
        [mi setTarget:leaf ? target : nil];
        [mi setEnabled:item.disabled ? NO : YES];
        if (item.checked) {
            [mi setState:NSOnState];
        }
        [mi setTag:(NSInteger)g_next_command_index++];   // 扁平命令路径表下标
        if (leaf) {
            [menu addItem:mi];
            [mi release];
        } else {
            NSMenu* sub = [[NSMenu alloc] initWithTitle:(title != nil ? title : @"")];
            buildMenuItems(sub, item.children, target);
            [menu setSubmenu:sub forItem:mi];
            [sub release];
            [mi release];
        }
    }
}

} // namespace

std::optional<std::vector<int>> showContextMenu(
    void* nativeHandle, const std::vector<ContextMenuItem>& items) {
    if (nativeHandle == nullptr) {
        return std::nullopt;   // 句柄无效 → 调用方回退自绘菜单（与 stub 语义一致）
    }
    NSWindow* nsWindow = glfwGetCocoaWindow(static_cast<GLFWwindow*>(nativeHandle));
    if (nsWindow == nil) {
        return std::nullopt;
    }
    @autoreleasepool {
        // 命令路径表：复用头内 inline 纯函数（菜单树 → 扁平索引路径，DFS 同序）
        const std::vector<std::vector<int>> paths = contextMenuCommandPaths(items);
        g_popup_paths = &paths;
        g_selected_any = false;
        g_selected_command_path.clear();
        g_next_command_index = 0;
        if (g_context_menu_target == nil) {
            g_context_menu_target = [[EUIContextMenuTarget alloc] init];
        }

        NSMenu* menu = [[NSMenu alloc] initWithTitle:@""];
        [menu setAutoenablesItems:NO];
        buildMenuItems(menu, items, g_context_menu_target);

        // ⚠️ 坐标系（设计 §6.2 注记）：atLocation 是 contentView 视图坐标、
        // 原点左下。当前鼠标的屏幕坐标（同为左下原点）经
        // convertPoint:fromView:nil 一步换算成视图坐标即可——**不要**再做
        // 手动翻转（翻转仅适用于上层给的是左上原点逻辑坐标的场景）；
        // 也勿与托盘 mouseLocation 的屏系翻转（屏高 − y）混用，两套原点。
        NSView* content = [nsWindow contentView];
        const NSPoint screenPoint = [NSEvent mouseLocation];
        const NSPoint viewPoint = [content convertPoint:screenPoint fromView:nil];
        [menu popUpMenuPositioningItem:nil atLocation:viewPoint inView:content];

        [menu release];
        g_popup_paths = nullptr;
        if (!g_selected_any) {
            return std::nullopt;   // 点空白/取消 → nullopt（调用方回退）
        }
        return std::vector<int>(g_selected_command_path);
    }
}

} // namespace core::platform
```

- [ ] **Step 3: 新建 `power_events_macos.mm`（整文件逐字）**

```objc
// core/platform/power_events_macos.mm —— macOS 系统电源事件
//（2026-09-28 macos-three-features 设计 §6.3）
// NSWorkspace 通知（WillSleep / DidWake），不依赖窗口句柄（参数忽略）；
// 枚举映射对齐 Windows WM_POWERBROADCAST 语义。IOKit 电池/低电量不做（设计 §9）。
#import <Cocoa/Cocoa.h>

#include <functional>

#include "power_events.h"

namespace core::platform {

namespace {

std::function<void(SystemPowerEvent)> g_handler;

id g_sleep_observer = nil;
id g_wake_observer = nil;

} // namespace

void setSystemPowerHandler(std::function<void(SystemPowerEvent)> handler) {
    g_handler = std::move(handler);
}

void installSystemPowerNotifications(void* /*nativeWindowHandle*/) {
    @autoreleasepool {
        if (g_sleep_observer != nil) {
            return;   // 进程级幂等（对齐 win 语义：首个安装生效）
        }
        NSNotificationCenter* center =
            [[NSWorkspace sharedWorkspace] notificationCenter];
        g_sleep_observer =
            [center addObserverForName:NSWorkspaceWillSleepNotification
                                object:nil queue:nil
                            usingBlock:^(NSNotification*) {
                                if (g_handler) {
                                    g_handler(SystemPowerEvent::Suspending);
                                }
                            }];
        g_wake_observer =
            [center addObserverForName:NSWorkspaceDidWakeNotification
                                object:nil queue:nil
                            usingBlock:^(NSNotification*) {
                                if (g_handler) {
                                    g_handler(SystemPowerEvent::Resuming);
                                }
                            }];
    }
}

void uninstallSystemPowerNotifications() {
    @autoreleasepool {
        NSNotificationCenter* center =
            [[NSWorkspace sharedWorkspace] notificationCenter];
        if (g_sleep_observer != nil) {
            [center removeObserver:g_sleep_observer];
            g_sleep_observer = nil;
        }
        if (g_wake_observer != nil) {
            [center removeObserver:g_wake_observer];
            g_wake_observer = nil;
        }
    }
}

} // namespace core::platform
```

- [ ] **Step 4: `CMakeLists.txt` 三族选源拆 `elseif(APPLE)`（L273-294）**

当前三族均为 `if(WIN32)/else()`；分别改为：

```cmake
# 窗口样式属性（桌宠设计 §2.6 G5：任务栏隐藏等）。Windows 走 WS_EX_TOOLWINDOW；
# macOS 走 NSWindowCollectionBehavior（macos-three-features 设计 §6.1）；其余走 stub
if(WIN32)
    list(APPEND CORE_SOURCES core/platform/window_style_win.cpp)
elseif(APPLE)
    list(APPEND CORE_SOURCES core/platform/window_style_macos.mm)
else()
    list(APPEND CORE_SOURCES core/platform/window_style_stub.cpp)
endif()
```

```cmake
# 原生右键菜单（小尺寸覆盖窗自绘菜单被窗口 bounds 裁剪——桌宠右键菜单）。
# Windows 走 TrackPopupMenu；macOS 走 NSMenu popUp（macos-three-features 设计
# §6.2）；其余平台走 stub（nullopt，调用方回退自绘）
if(WIN32)
    list(APPEND CORE_SOURCES core/platform/context_menu_win.cpp)
elseif(APPLE)
    list(APPEND CORE_SOURCES core/platform/context_menu_macos.mm)
else()
    list(APPEND CORE_SOURCES core/platform/context_menu_stub.cpp)
endif()
```

```cmake
# 系统电源事件（睡眠/唤醒通知宿主——DevDesk 桌宠「Sleep 零渲染」）。
# Windows 走 WM_POWERBROADCAST WndProc 子类链；macOS 走 NSWorkspace 通知
#（macos-three-features 设计 §6.3）；其余平台走 stub（回调永不触发，宿主安全降级）
if(WIN32)
    list(APPEND CORE_SOURCES core/platform/power_events_win.cpp)
elseif(APPLE)
    list(APPEND CORE_SOURCES core/platform/power_events_macos.mm)
else()
    list(APPEND CORE_SOURCES core/platform/power_events_stub.cpp)
endif()
```

- [ ] **Step 5: 隔离线验证**

eui-mock 新建三 `.mm` + 改 CMakeLists → 定向构建（Windows configure 仍绿）+ `--filter pet`/`menu` 定向用例 → 产 `M3-eui.patch`。预期：Windows 零行为变化；`.mm` 语义/一致性审查（编译正确性留实机，§12-5）。

- [ ] **Step 6: 非隔离线 apply + commit + 审计**

```bash
git -C D:/project/gui/EUI-NEO add core/platform/window_style_macos.mm core/platform/context_menu_macos.mm core/platform/power_events_macos.mm CMakeLists.txt
git -C D:/project/gui/EUI-NEO commit -m "feat(macos): 桌宠三项平台实现（DevDesk 需要 macOS 桌宠等价能力）
- window_style_macos.mm：hideFromTaskbar 等价 = CanJoinAllSpaces | IgnoresCycle
- context_menu_macos.mm：NSMenu popUp（同步弹出 + 命令路径表对齐 + 坐标换算）
- power_events_macos.mm：NSWorkspace willSleep/didWake → SystemPowerEvent
- CMakeLists：三族选源拆 elseif(APPLE)，stub 收窄给非 Apple 平台
- macos-three-features 设计 M3（§6）"
```

---

## 收尾任务: bump DEVDESK_EUI_NEO_MIN_REV（DevDesk 侧 worktree 线）

**Files:**
- Modify: `DevDesk/CMakeLists.txt`（L53 `set(DEVDESK_EUI_NEO_MIN_REV "269a8c4")` → 新 tip sha）
- Modify: `DevDesk/CLAUDE.md`（「常用命令」段「当前依赖 rev」记录两处同步）

**前置：** M0-M3 全部合入 EUI-NEO main 后，取 `git -C D:/project/gui/EUI-NEO rev-parse --short HEAD` 得新 rev。

- [ ] **Step 1**：两处 `269a8c4` 换新 rev（CMakeLists.txt:53 + CLAUDE.md「常用命令」段依赖记录行；CLAUDE.md 另有「环境与工具链」段的重复记录同改，保持全文一致）。
- [ ] **Step 2**：worktree 子任务原子 commit（显式 pathspec）→ 主会话审计。
- [ ] **Step 3**：移除 M0 Step 3 的临时诊断日志（若已加）——一并收尾 commit。
- [ ] **Step 4**：主会话跑一次 **Win 全量回归**（里程碑收尾关口唯一一次全量：`bash scripts/build-test-run.sh`，用例数以运行输出 Total 为准）+ `scripts/check-module-isolation.sh` 状态确认。

---

## 跨仓库交付线（每个 M 的 EUI 改动统一走此线）

1. **隔离 worktree agent**（`isolation: "worktree"`）改 EUI-NEO 源：把 EUI-NEO 源拷成 `<worktree>/eui-mock/`（`cp -r D:/project/gui/EUI-NEO/. <worktree>/eui-mock/`，排除 `.git`/`build`），在 eui-mock 内实施本任务改动；`EUI_NEO_DIR=<worktree>/eui-mock` 真跑 DevDesk 定向构建/定向单测验证（ccache 热缓存下 worktree 首建 ~35s；工具链由 `scripts/build-test-run.sh` 内部处理）。**隔离 agent 不得对 EUI-NEO 仓库执行任何 git 操作**（守卫会拒）。
2. 产 patch：`diff -ruN --exclude=.git --exclude=build D:/project/gui/EUI-NEO <worktree>/eui-mock > <worktree>/<M>-eui.patch`（与 git format-patch 等价物——跨仓库无 git 操作，符合反事故红线）；报告 patch 路径 + sha256 + 验证摘要（定向用例数/绿）。
3. **非隔离 agent**（不设 `isolation`）在 `D:/project/gui/EUI-NEO` 主树：`git apply --check <patch>` → `git apply` → 编译（隔离线已证编译面时免重编，只做 `--check` + 必要时 EUI-NEO 独立 configure 冒烟；**禁止在 DevDesk 主工作树跑构建**、禁止碰 DevDesk 任何文件）→ 原子 commit（显式 pathspec，commit message 注明需求来源「DevDesk 需要 X」）。
4. 主会话读 diff 审计：patch 内容与任务声明一致、无夹带；EUI-NEO main tip 推进；`git worktree remove` + `prune` 清理隔离 worktree，**不留残留**。
5. DevDesk 侧改动（M2 文案 / 收尾 bump）**不走本条线**：由 DevDesk 侧隔离 worktree 子任务执行（普通交付流水线：worktree 子任务 → 原子 commit → 主会话审计合并），红线禁止非隔离线 agent 碰 DevDesk（其工作目录就是主树）。
6. EUI-NEO 本地 main 领先 origin 的既有提交**永不 push**（2026-09-28 用户决策，origin/main 停在 v0.6.0）——本批继续落本地 main，与 push 无关（§13-3）。

## 测试

### 静态评审（本机可做，每任务）

- 实现后 codegraph/grep 复核改动点与 spec §2/§8 行号一致、无遗漏消费点（M0 两处句柄供给的消费链：`glfw_app_main.cpp:252-276`（含 L581 `installSystemPowerNotifications(nativeWindowHandle(window))` 第三消费点——mac 实现忽略该参数，无需处理）与 `window_backend.cpp:831-843`）。
- 双审（spec compliance + code quality）各任务各一次；改动非机械，不合并为一审。
- **Windows 回归（必须全绿，每任务）**：M0-M3 全部为 `__APPLE__` 分支追加或新建 mac 文件（`enable_language(OBJCXX)` 有 `if(APPLE)` 守卫），Win/Posix 路径字节不动；eui-tray / window_effect / pet 相关定向用例 + configure 冒烟。
- 隔离 agent 的 eui-mock 构建验证（§7-1）：证明消费 API 匹配（M0/M1 的 Win 编译段真编译；mac 专属段覆盖不到，见下）。

### Mac 实机验收清单（spec §10.2 原文；**截至 2026-09-28 未执行**——成稿时本机无 mac，现本机已是 macOS，待排期）

| 任务 | 验收项 |
|------|--------|
| M0 | ① `nativeWindowHandle`（glfw_app_main.cpp）/ `nativeWindowInfo` GLFW 变体（window_backend.cpp）在 mac 返回非空 NSWindow*（M2/M3 各验收项的前置；临时诊断日志确认后移除） |
| M1 | ⓪ **挂 action 与 `setMenu:` 共存**（AppKit 已知争议点：挂 menu 时点击自动弹菜单、action 可能不触发）——action 不触发则切预备改法（本计划 Task M1 Step 5 附录 A）：不挂 menu、左右键全走显式 `popUpStatusItemMenu:`；① 左键单击托盘 → 唤起主窗口；② 右键单击 → styled 菜单（与 Win 同款外观）；③ 菜单位于图标正下方、不超屏（坐标翻转正确）；④ 降级路径（styled 构建失败/0 返回）仍弹原生菜单；⑤ 菜单项点击回调正确 |
| M2 | ① 三档（off/translucent/frosted）切换视觉正确；② 磨砂材质深/浅主题均正常、主题切换保持；③ 设置页降级/生效文案平台化后正确；④ Tahoe 不生效时显示平台化降级文案、不崩 |
| M3 | ① 桌宠无边框置顶透明窗口正常开/关/拖；② 不进 Dock、⌘Tab 不出现（collectionBehavior）；③ 桌宠右键弹原生菜单、点项回调正确；④ 睡眠→唤醒后动画随 willSleep/didWake 暂停/恢复 |

## 验收

1. 每个 M 的 patch 与其 spec 章节逐条对应：M0 = spec §3 两处句柄供给 + 消费链无遗漏；M1 = §4.1/§4.2（分流 + 预备改法 + 不改 3rd/tray）；M2 = §5.2 `.mm` 整文件 + §5.3 文案（案 B 一行）/ CMake 两处；M3 = §6.1-§6.3 三文件 + CMake 三族。
2. 定向用例全绿（每任务 `bash scripts/build-test-run.sh --no-run --filter <子串>`）；收尾一次全量回归。
3. 隔离 worktree 全部清理（`git worktree list` 无残留）；EUI-NEO main 四个原子 commit（M0-M3）+ DevDesk 侧文案/bump commit，全部显式 pathspec。
4. `DEVDESK_EUI_NEO_MIN_REV` 两处同步为新 rev；M0 临时诊断日志已移除。
5. **未验证项（诚实声明，spec §12）**：① Tahoe 磨砂实际观感与 NSVisualEffectView 合成顺序（spec §12-1/12-2）；② 坐标翻转方向（托盘屏系 / 菜单视图系）为设计推断，实机后可能微调一行（§12-3）；③ 挂 action 与 setMenu: 共存为 AppKit 已知争议点（§12-4，M1 实机第 0 项）；④ `.mm` 首进编译链的 ObjC++ 编译正确性——本机（Windows）无 mac SDK，只能实机验证（§12-5）；⑤ 桌宠 collectionBehavior 组合是否符合「不进 Dock / ⌘Tab 不可见」预期（§13-4）。这些项在 Mac 实机清单（上节）完成前**不得**在报告/文档中称「已实现生效」。