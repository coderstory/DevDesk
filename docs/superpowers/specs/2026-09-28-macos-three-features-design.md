# macOS 三项能力补齐（托盘 styled 菜单 / 磨砂 / 桌宠三项）—— EUI-NEO 上游线（设计）

日期：2026-09-28 ｜ 状态：**设计（已实施，未实机验收）** ｜ 版本：**V2**（V1 = `1049285a`，2026-09-28 独立审计修订）｜ 关联：`2026-09-20-devdesk-design.md`（权威 spec，实现前先读）、`2026-09-22-eui-window-frost-design.md`（磨砂全链既有设计，本 spec 是其 macOS 落地）、`2026-09-22-desktop-pet-design.md`（桌宠设计）

> **实施状态补记（2026-09-28 当日回填；本文件早前的状态口径「设计（未实施）」已随之作废）**：四条任务线均已落 EUI-NEO 本地 main——**M0-1** 原生句柄（`EUI-NEO/core/app/glfw_app_main.cpp:262-268`，`b561dbe`）、**M1** 托盘左右键分流（`EUI-NEO/core/platform/tray_bridge.c`，`00eb794`）、**M2** 磨砂（`EUI-NEO/core/platform/window_effect_macos.mm`，`654e014`，merge `e115d25`）、**M3** 桌宠三项（`window_style_macos.mm` / `context_menu_macos.mm` / `power_events_macos.mm`，`6f771c3`）。两条未完成/未验证：① **M0-2**（`EUI-NEO/core/window/window_backend.cpp` 的 `__APPLE__` 分支填 `platformWindow`）曾在同日提交 `7164d55` 中被顺带删除，**恢复工作进行中**——在它合入前，M2/M3 在 macOS 上取不到 `NSWindow*`（本设计 §3 的阻塞性前置仍成立）；② 本文全部 ⚠️ 项与 §10.2 的 Mac 实机验收清单**尚未执行**（仓库内无验收记录），macOS 侧构建是否通过、测试基线多少，见 `DevDesk/TODO.md`「macOS 构建环境」段。**「已实施」只表示代码已落本地 main，不代表已在 macOS 上跑通或验收过。**

> **V2 修订要点（独立审计，2026-09-28）**：① 新增 §3 **M0「macOS 原生句柄供给」**——阻塞性前置，串行先行 M2/M3（不补这两处，M2/M3 收 nullptr 恒降级 = 白做）；② §8 路径纠正：`EUI-NEO/core/platform/CMakeLists.txt` **不存在**，实际 `if(WIN32)/else()` 选源在 `EUI-NEO/CMakeLists.txt` L265-294，并补列 M0 两文件；③ §4.2 挂 action 与 `setMenu:` 共存之争（AppKit 已知争议点）升为 **M1 Mac 实机验证第 0 项**，预备改法：不挂 menu、全走显式 `popUpStatusItemMenu:`；④ **§13-6 关闭**（静态定案）：macOS 走 tray_bridge 的 `EUI_TRAY_APPKIT` 段，`3rd/tray` 的 `TRAY_APPKIT` 分支 EUI 构建不编译 → M1 实作只改 `tray_bridge.c`，原 §3.1/§3.2 作废；⑤ §7 补 DevDesk 侧改动归属与串行依赖；⑥ §6.2 补 `popUpMenuPositioningItem:atLocation:` 坐标原点注记。**章节顺延**：原 §3-§12 顺延为 §4-§13，正文交叉引用已全部同步更新（原 V1 编号仅作历史标注保留）。

> **证据等级说明**：成稿时本机是 Windows 开发机（无 macOS 实机），凡 macOS 侧 API 行为 / 坐标方向 / ObjC++ 编译链均为**设计推断**，本文按 ⚠️ 标注；实施与验收以 §10 Mac 实机清单为准，不得把 ⚠️ 项当既定事实引用。**（2026-09-28 补记：项目当日已搬回 macOS 本机开发，M0-M3 代码已落本地 main，但 ⚠️ 项仍未做实机验收——上句的引用纪律不变；macOS 构建状态见 `DevDesk/TODO.md`「macOS 构建环境」段。）**

## 1. 需求与决策

DevDesk 目标平台含 macOS（macOS 主线 P0-P8 已完成；2026-09-28 起本项目已搬回 macOS 本机开发，当前开发平台就是 macOS；平台适配代码按 `*_win.cpp` / `*_macos.cpp` / `*_posix.cpp` 拆文件）。三项 macOS 能力补齐（输入为已完成预研结论，本 spec 用 codegraph / Read 逐条核实 file:line，见 §2）：

| # | 需求 | 现状缺口 |
|---|------|----------|
| **R1** | 托盘菜单与 Windows 一致：左右键分流，右键/左键弹 **styled 自绘菜单**（round 角/主题/图标），原生 NSMenu 仅作降级 | macOS 托盘左右键都弹原生 NSMenu，无任何拦截点（`3rd/tray/tray.h` APPKIT 分支 `tray_update` 只 `setMenu:`；EUI 构建实际走 tray_bridge 的 appkit 段，同样只 `setMenu:`，§2.1） |
| **R2** | 窗口磨砂三档（off/translucent/frosted）在 macOS 可用；失败保留 Transparent 降级 | `window_effect_stub.cpp` 恒返回 false → 一直降级 Transparent |
| **R3** | 桌宠三项 mac 等价能力：不进 Dock/⌘Tab（hideFromTaskbar 等价）、原生右键菜单、电源事件（sleep/wake） | 三个平台 stubs：`window_style_stub` / `context_menu_stub` / `power_events_stub` 全静默降级 |

| # | 决策 | 理由 |
|---|------|------|
| **D-1** | 三项全部走 **EUI-NEO 上游线**（修改 `../EUI-NEO/`，DevDesk 侧除文案平台化与版本 bump 外零改动） | DevDesk CLAUDE.md「EUI-NEO 上游贡献政策」：遇到 EUI-NEO 能力缺失优先修上游而非 DevDesk workaround；托盘/窗口效果/窗口行为均属通用平台能力 |
| **D-2** | 磨砂**不引任何 crate**（window-vibrancy / objc2 等），ObjC++ 直挂 `NSVisualEffectView` | window-vibrancy（0.5）在 macOS 26 Tahoe 上 4 次尝试全败（crate、Tauri 2 官方 EffectsBuilder、裸 objc2 FFI 均产不出可用 vibrancy）；本实现同机制、大概率同结果，故走最薄路径并留降级 |
| **D-3** | 磨砂失败 → **保留 Transparent 降级交付**，不阻塞 | stub 现状即降级路径，调用链天然支持（§5）；Tahoe 若真不支持，交付形态劣于 Windows 但行为不退化 |
| **D-4** | 平台实现按接口拆文件（`*_macos.cpp` / `*_macos.mm`），与既有 `*_win.cpp` / `*_posix.cpp` 同模式，业务代码不散布平台宏 | DevDesk CLAUDE.md 核心约束 5 / EUI-NEO 后端同模式；stubs 收窄为仅非 Apple 平台使用 |
| **D-5** | **M0 句柄供给 / M1 托盘 / M2 磨砂 / M3 桌宠为独立任务**，各自走 §7 跨仓库交付线（**M0 阻塞性串行先行，M2/M3 依赖**；隔离 worktree 产 patch → 非隔离线 apply + 编译 + commit） | 各任务改 EUI-NEO 独立文件、共享头接口签名零改动（M0 零 API 变化），冲突面最小；跨仓库施工受隔离守卫约束，必须"产补丁 + 非隔离线应用"（DevDesk CLAUDE.md 反事故红线）；M0 是 M2/M3 的硬前置（§3） |
| **D-6** | 收尾统一 bump `DEVDESK_EUI_NEO_MIN_REV`（DevDesk 顶层 CMakeLists.txt + DevDesk CLAUDE.md 两处同步），不掺进 M0-M3 | DevDesk CLAUDE.md：消费 EUI 新 API 后两处同步更新，当前依赖 rev `269a8c4` |
| **D-7** | 不做 mac 自动化构建/CI、不做 Linux 对等实现（§9） | 成稿时本机无 mac、无法在本仓库验证 mac 编译（2026-09-28 起本机已是 macOS，但本批仍不建 mac CI，macOS 编译走本机手动 + 实机验收）；Linux 无宿主需求 |

## 2. 现状事实（源码核实）

> 本节全部条目经 codegraph（projectPath=`D:/project/gui`）与直读核实，文件名无特殊标注即 **CONFIRMED**；⚠️ 为设计推断。

### 2.1 托盘：stub 无、Win 有拦截点、APPKIT 无

| 事实 | 证据 |
|------|------|
| APPKIT 分支入口 `#elif defined(TRAY_APPKIT)`，状态 `statusItem` / `statusBarButton` 为静态（**仅供 3rd/tray 独立构建**，EUI 构建不编译此宏，见下） | `EUI-NEO/3rd/tray/tray.h:91`、`:99-100` |
| APPKIT `tray_update` **只做两件事**：`setImage:` + `[statusItem setMenu:]` —— 左右键点击都由 AppKit 自动弹原生 NSMenu，**无 left/right 分流、无任何回调拦截点** | `EUI-NEO/3rd/tray/tray.h:196-202` |
| Win32 对照（要迁移的语义）：`WM_TRAY_CALLBACK_MESSAGE` 里 `lparam == WM_LBUTTONUP \|\| WM_RBUTTONUP` 分流（L248）→ `eui_tray_menu_requested(p.x, p.y, left)` 返回非 0 则跳过原生 `TrackPopupMenu`（L251-253）；0 走原生（L254-257） | `EUI-NEO/3rd/tray/tray.h:247-260` |
| `eui_tray_menu_requested` 的**前向声明位于 `#elif defined(TRAY_WINAPI)` 段内**（L206 起），APPKIT 分支（L91 起）在它之前编译不到 → 独立构建 3rd/tray 若想复用需上移声明（本批不改，§4.3） | `EUI-NEO/3rd/tray/tray.h:227`（声明行）、`:206`（段起点） |
| 钩子注册与实现：`eui_tray_set_menu_requested_fn` 存函数指针（L122-124），`eui_tray_menu_requested(x, y, left_button)` 转发（L126-131，未注册返回 0 = 原生路径）；注释明确"上层（app 层 styled 菜单等）已接管本次菜单展示，原生弹出被跳过" | `EUI-NEO/core/platform/tray_bridge.c:116-131` |
| DSL 层注册入口：`setTrayMenuRequestedHandler` → `trayMenuRequestedTrampoline` → `eui_tray_set_menu_requested_fn` | `EUI-NEO/core/platform/platform.cpp:747-763` |
| **V2 已定案（§13-6 关闭）**：EUI-NEO tray bridge 的 appkit 段（`g_status_item` + `[g_status_item setMenu:g_menu]`，init L276 / update L234）是 **macOS 唯一后端** —— EUI 构建只定义 `EUI_TRAY_*` 宏族（`CMakeLists.txt:401-418`：`elseif(APPLE)` → `EUI_TRAY_APPKIT=1` + 链接 Cocoa/objc），`3rd/tray` 的 `TRAY_APPKIT` 分支编译不到 → **M1 实作只改本文件（§4.1）** | `EUI-NEO/core/platform/tray_bridge.c:227-236`、`EUI-NEO/CMakeLists.txt:404-406` |
| DevDesk 消费点：`.tray(true)` / `.trayMenu(nativeTrayMenuItems(), false)`（keepDefault=false，原生菜单仅为降级数据源）/ `.onTrayMenu(...)` —— 左键 `ShowMainWindow` 返回 true，右键 `showTrayStyledMenu(x, y)` | `DevDesk/src/main.cpp:566-575` |
| DevDesk styled 菜单已就绪：`showTrayStyledMenu` 全函数（构建 items → `components::showStyledMenu("tray", items, style, x, y, 回调)`），Windows 已在用 | `DevDesk/src/main.cpp:390-408`（`showStyledMenu` 调用在 **:400**） |

### 2.2 磨砂：stub 恒 false 与降级链

| 事实 | 证据 |
|------|------|
| `applyWindowEffect(void*, WindowEffect)` 与 `applyTitleBarAppearance` **恒返回 false**（注释自述"macOS 的 NSVisualEffectView 磨砂 … 属磨砂设计文档 Phase E"） | `EUI-NEO/core/platform/window_effect_stub.cpp:8-14` |
| 降级链：GL 后端应用窗口效果失败 → `degradedWindowEffect`（回退档位，L66）→ 回写 `activeWindowEffect`（主循环应用/降级后写回，`dsl_app_impl.h:65-66` 注释、`:431-433`）→ 设置页经 `activeWindowEffect()` 回查（`:416-422`）展示真实档位 | `EUI-NEO/core/platform/window_effect.h`（`systemBackdropValue:53` / `degradedWindowEffect:66`）、`EUI-NEO/include/eui/detail/dsl_app_impl.h:403-433` |
| **M0 前置缺口**：GLFW 路线 `applyWindowEffectToWindow`（`glfw_app_main.cpp:269`）→ `applyWindowEffect(nativeWindowHandle(window), ...)`，而 `nativeWindowHandle` 非 Win 平台返 nullptr（§3）——**mac 上没 M0，磨砂/标题栏外观全部拿不到 NSWindow** | `EUI-NEO/core/app/glfw_app_main.cpp:252-269` |
| DevDesk 接线：启动 `module::settings::applyWindowVisuals(cfg.windowEffect)`（clearColor alpha 按档 + `app::setWindowEffect` 请求 backdrop）；启动快照 `.windowEffect(WindowEffect::Transparent)`（透明帧缓冲 hint 创建期不可逆，off 档靠 alpha=1 视觉等价） | `DevDesk/src/main.cpp:513-518`、`:550-554` |
| 降级提示文案 **"需 Windows 11 22H2+" 是 Win 专属**，mac 路径会显示错误信息 → 需平台化（§5.3） | `DevDesk/src/module_settings/settings_config.cpp:210-218`（关键文案在 **:215**） |
| `applyWindowVisuals` 全函数：alpha 按档 + `setWindowEffect(windowEffectRequest(mode))` | `DevDesk/src/module_settings/settings_config.cpp:223-231` |

### 2.3 桌宠三项 stubs 与 M0 前置缺口

| 事实 | 证据 |
|------|------|
| `applyWindowStyleFlags(void*, flags)` 恒 false（注释："macOS/Linux stub：桌宠设计 G5 的任务栏隐藏是 Windows 专属需求（macOS 子窗口本就不进 Dock）"）→ `hideFromTaskbar` 在 mac 永不生效 | `EUI-NEO/core/platform/window_style_stub.cpp:8-10` |
| `showContextMenu` 恒返回 `nullopt`（注释："macOS NSMenu popUp 可后续补…桌宠右键菜单回退自绘 contextMenu 组件"） | `EUI-NEO/core/platform/context_menu_stub.cpp:9-14` |
| `setSystemPowerHandler` 存储但**永不触发**；`install/uninstallSystemPowerNotifications` 空操作（注释：sleep/wake 通知 mac 对应 IOKit/NSWorkspace） | `EUI-NEO/core/platform/power_events_stub.cpp:12-19` |
| **M0 前置缺口**：`applyWindowStyleFlags` / `showContextMenu` 的 mac 实现都要经 `nativeWindowInfo(handle).platformWindow` 取 NSWindow（`window_backend.cpp` GLFW 变体 L831-843 仅 `_WIN32` 填，§3）——**mac 上没 M0，M3 三项全部拿不到 NSWindow** | `EUI-NEO/core/window/window_backend.cpp:831-843`（调用链 `pet_window.cpp` → `app::openWindow` → `sdl2/glfw_app_main` → `nativeWindowInfo`） |
| DevDesk 桌宠开窗消费点：`app::openWindow(DslWindowConfig{…})` —— `.decorated(false)`（:674）`.alwaysOnTop(true)`（:675）`.focusOnShow(false)`（:677）`.hideFromTaskbar(true)`（**:678**）`.transparentFramebuffer(true)`（:679）`.windowEffect(None)`（:685）`.onWindowCreated`（:688）`.onWindowDestroyed`（:697） | `DevDesk/src/module_pet/pet_window.cpp:669-702` |

### 2.4 平台纪律与交付约束

| 事实 | 证据 |
|------|------|
| 平台适配代码接口隔离：头文件平台无关接口，`*_win.cpp` / `*_macos.cpp` / `*_posix.cpp`，CMake 按平台选源；业务代码禁散布 `#ifdef` | `DevDesk/CLAUDE.md` 核心约束 5 |
| 跨仓库改动必须"产补丁 + 非隔离线应用"：隔离 worktree agent 无法对其它仓库 git 操作；非隔离线 prompt 写死只许改目标仓库 | `DevDesk/CLAUDE.md`「反事故红线」 |
| EUI-NEO 当前依赖 rev `269a8c4`（2026-09-27），本地 main 领先 origin 的提交**永不 push**（2026-09-28 用户决策：origin/main 停在 v0.6.0，本地领先是长期状态、不再按「待 push」跟踪）；`DEVDESK_EUI_NEO_MIN_REV` 在 DevDesk 顶层 CMakeLists.txt 与 CLAUDE.md 两处同步 | `DevDesk/CLAUDE.md`「常用命令」段 |

## 3. M0 —— macOS 原生句柄供给（阻塞性前置，串行先行 M2/M3）

> **V2 新增（独立审计）**。M2/M3 的 mac 平台实现全部需要原生 `NSWindow*` 句柄：磨砂要往 `contentView` 插 `NSVisualEffectView`，桌宠要设 `collectionBehavior` / 弹原生菜单 / 注册电源通知。而当前 GLFW 路线的**两处句柄供给对非 Win 平台恒返回 nullptr**——不补这两处，M2/M3 收 nullptr 一律直接走降级路径 = 白做。M0 零 API 变化（纯内部句柄供给修复），DevDesk 零改动。

| # | 文件 | 现状（源码核实） | 改动 |
|---|------|------------------|------|
| M0-1 | `EUI-NEO/core/app/glfw_app_main.cpp` | `nativeWindowHandle`（L252-259）：`#if defined(_WIN32) return glfwGetWin32Window(window); #else (void)window; return nullptr;` —— 非 Win 直接 nullptr，注释自述"非 Windows 平台 nativeWindowHandle 返回 nullptr，applyTitleBarAppearance 内部静默降级"（L250-251） | L255 `#else` 拆 `#elif defined(__APPLE__)`：`return glfwGetCocoaWindow(window);`（GLFW 官方 macOS API，返回 `NSWindow*`）+ 保留 `#else` nullptr。消费者：`applyTitleBarAppearanceToWindow`（L261-264）与 `applyWindowEffectToWindow`（L269-…，M2 主入口） |
| M0-2 | `EUI-NEO/core/window/window_backend.cpp` | GLFW 变体 `nativeWindowInfo`（L831-843）：仅 `#if defined(_WIN32)` 填 `result.platformWindow = glfwGetWin32Window(...)`（L834-841） | 加 `#elif defined(__APPLE__)`：`result.platformWindow = glfwGetCocoaWindow(static_cast<GLFWwindow*>(window));`。消费者：M3 的 `applyWindowStyleFlags` / `showContextMenu` 经 `nativeWindowInfo(handle).platformWindow` 取 NSWindow |

**注意点**：
- 同文件 SDL2 变体 `nativeWindowInfo`（L502-525）**已含 `__APPLE__`**（`SDL_SYSWM_COCOA` → `info.info.cocoa.window`，L518-521），不需改；DevDesk 走 GLFW 后端（`EUI_WINDOW_BACKEND=glfw`），SDL2 变体不影响本批。
- 两处改动均为纯 `__APPLE__` 分支追加，Windows / Posix 路径字节不动 → Win 回归面为零（§10.1）。
- M0 与 M1 无依赖（托盘走 `NSStatusItem`，不经窗口句柄）；**M2/M3 依赖 M0 先合入**（§11 拆任务）。

## 4. 托盘 styled 菜单（M1）

目标：macOS 托盘点击行为与 Windows 对齐 —— 左键 = 唤起主窗口，右键 = styled 菜单；原生 NSMenu 仅保留为降级。

### 4.1 唯一实作点：tray_bridge.c appkit 段分流（后端已定案）

**后端定案（V2，§13-6 关闭）**：macOS 构建走 `tray_bridge.c` 的 `EUI_TRAY_APPKIT` 段（`EUI-NEO/CMakeLists.txt:404-406` `elseif(APPLE)` 定义 `EUI_TRAY_APPKIT=1` 并链接 Cocoa/objc；`tray_bridge.c:135` `#if defined(EUI_TRAY_APPKIT)`）。`3rd/tray/tray.h` 的 `TRAY_APPKIT` 分支 EUI 构建**不编译**（§4.3）→ **M1 实作只改本文件**，不存在"两个后端按编译配置取舍"问题。

现有结构（代码核实）：`eui_tray_init`（L238-283）整套 NSStatusItem 创建（`statusItemWithLength` → `[g_status_item button]` L262 → `setImage:` L273 → `[g_status_item setMenu:g_menu]` L276）；更新路径 `eui_tray_menu_changed`（L227-236）→ `eui_tray_rebuild_menu` → `[g_status_item setMenu:g_menu]`（L234）；钩子 `eui_tray_menu_requested(x, y, left)`（L126-131，未注册返回 0 = 原生路径）在桥内**天然可达**（同一编译单元），DevDesk/DSL 侧已注册 handler（§4.4），零改动。

改造（对 `[g_status_item button]`，L262 已取到）：

1. `setTarget:` / `setAction:` 挂新增的 action 回调（新 static 函数 `tray_button_action`）；
2. `sendActionOn:(NSLeftMouseUpMask | NSRightMouseUpMask)` —— NSButton 默认只在左键 AC 时发 action，`sendActionOn:` 把右键一并纳入；
3. `tray_button_action` 体内：取 `[[NSApp currentEvent] type] == NSRightMouseUp` → `left_button = 0`，否则 1；取 `[[NSApp currentEvent] mouseLocation]`（⚠️ Cocoa 屏幕坐标，**原点在主屏左下**）→ 翻转 `y = 屏高 − mouseLocation.y` 到左上（§12-3）→ 调 `eui_tray_menu_requested(x, y, left_button)`：
   - **返回非 0**：上层（DSL handler → DevDesk `onTrayMenu`）已接管，Mac 端 DevDesk 已实现 styled 菜单路径（main.cpp:569-575），原生路径完全跳过 —— 与 Win32 `tray.h:251-253` 语义一致；
   - **返回 0**：恢复原生行为 —— `[g_status_item popUpStatusItemMenu:g_menu]` 显式弹出当前菜单（⚠️ 挂 action + `setMenu:` 共存争议与预备改法见 §4.2）；
4. `eui_tray_menu_changed` 保持 `setImage:` + `setMenu:` 更新路径（菜单内容随 `eui_tray_menu_changed` 更新），分流职责全部移到 button action；若按 §4.2 预备改法执行则两处 `setMenu:`（L234 / L276）移除。

### 4.2 挂 action 与 `setMenu:` 共存 —— AppKit 已知争议点（M1 实机验证第 0 项）

`NSStatusItem` 一旦 `setMenu:`，AppKit 在点击时**自动弹出该菜单**，`statusBarButton` 的 action 往往不触发（事件被菜单弹出消费）——「挂 action + `sendActionOn:` + 保留 `setMenu:`」能否共存是社区已知争议点，本设计 §4.1 主路径依赖共存。**预备改法（切换成本一行级）**：`setMenu:` 一律不调（L234 / L276 两处移除），按钮只挂 action，左/右键统一在 `tray_button_action` 内分流，返回 0 时显式 `[g_status_item popUpStatusItemMenu:g_menu]`（menu 对象仍由 `eui_tray_rebuild_menu` 维护，仅不再自动悬挂）。

**本项升为 M1 Mac 实机验证清单第 0 项（§10.2）**；实机验证前默认实现走 §4.1 主路径（保留 `setMenu:`），验证失败即切预备改法，两者均不需 DevDesk/DSL 侧改动。

### 4.3 `3rd/tray` APPKIT 分支：不动作（原 V1 §3.1/§3.2 方案作废）

`3rd/tray/tray.h` 的 `TRAY_APPKIT` 分支（L91 起）**只在 3rd/tray 独立构建时编译**（其 Makefile / example.c / README 自定 `TRAY_APPKIT 1`；EUI 构建只定义 `EUI_TRAY_*` 宏族，`CMakeLists.txt:401-418` 无 `TRAY_APPKIT`）→ 原 V1 §3.1（`eui_tray_menu_requested` 声明上移到共用段）与 §3.2（APPKIT 左右键分流）对 EUI 构建**编译不到、改动作废**：对 EUI 是死代码、对 macOS 运行零影响。若未来 3rd/tray 需独立复用，另行提交其仓库（不属本批范围）；届时声明上移方案对独立构建依然成立（其 Win32 分支 L247-260 已调 `eui_tray_menu_requested`，声明却在 `TRAY_WINAPI` 段内 L227）。

### 4.4 DevDesk 复用（预期零改动）

`platform.cpp:753-763` 已把 DSL handler 注册为 `eui_tray_menu_requested_fn`；DevDesk `main.cpp:566-575` 已实现左键 `ShowMainWindow` / 右键 `showTrayStyledMenu`（L400 的 `components::showStyledMenu` 直接消费 x/y）。mac 端接上分流后同一回调路径自动生效。

## 5. 磨砂（M2）

### 5.1 现状链路（§2.2）

stub 恒 false → GL 后端降级 → `activeWindowEffect` 回写 → 设置页展示降级文案（现为 Win 专属文案）。**前置：M0-1 合入**（`nativeWindowHandle` mac 返 NSWindow，否则 `applyWindowEffect` 收 nullptr 恒降级，§3）。

### 5.2 `window_effect_macos.mm`（新建，当前不存在）

- 位置 `EUI-NEO/core/platform/window_effect_macos.mm`，`.mm` 后缀走 ObjC++；
- 签名不变：`bool applyWindowEffect(void* /*GLFWwindow*/, WindowEffect)` / `bool applyTitleBarAppearance(void*, const TitleBarAppearance&)`（true = 生效；false = 走既有降级链）；
- 实现路径：`glfwGetCocoaWindow(glfwWindow*)` 取 NSWindow → `contentView` 层级插入 `NSVisualEffectView`：`setMaterial:` classic 组合（`HUDWindow` / `UnderWindowBackground`）+ `blendingMode: WithinWindow` + `state: Active`，**垫在 GL view 之下**（GL view 需透明/wantsLayer 才能透出材质，⚠️ 合成顺序实机验证）；
- 档位映射：`Frosted`（Acrylic 语义）→ NSVisualEffectView 可见；`Translucent` → view 透明度（数值 ⚠️ 待 Mac 实机定，§13-2）；`None` → view 隐藏/移除；
- **Tahoe 风险门**：不引 crate（D-2）；ObjC++ 直挂失败（不生效或异常）→ 返回 false → 既有 `degradedWindowEffect` + `activeWindowEffect` 回写链自动降级 Transparent，**不崩、不阻塞**（D-3）。

### 5.3 DevDesk 降级文案平台化

`settings_config.cpp:210-218` frosted 分支的 "需 Windows 11 22H2+"（:215）是 Win 专属。两案：

- **A（推荐，符合 D-4 平台拆文件纪律）**：新增平台接口如 `std::string windowEffectUnsupportedHint(mode)`（`*_win.cpp` 返回现文案；`*_macos.cpp` 返回"当前系统不支持磨砂，已回退为半透"；`*_posix.cpp` 通用文案），`windowEffectDegradedHint` 改调它；
- **B（最小）**：中性文案去掉 OS 专属信息（如"当前系统不支持磨砂，已回退为半透"），不新增接口。

**落地归属与串行依赖（V2 补，§7-6）**：上述改动在 `DevDesk/src/...`，属 DevDesk 仓库 → 必须走 **DevDesk 侧 worktree 线**执行（红线禁止非隔离线碰 DevDesk），不走 §7 的 EUI 补丁线。**案 A 依赖 EUI 侧 hint 接口先落地**（EUI 补丁线先行 → 合入 EUI-NEO main → DevDesk 接线后行，串行依赖写在 §7-6）；**案 B 中性文案改一行、零跨仓依赖**，DevDesk 线可独立先行。

§13-1 待用户复核。

## 6. 桌宠三项（M3）

三个 mac 平台文件（新建，均在 `EUI-NEO/core/platform/`），接口签名与 stubs 完全一致，stubs 收窄为仅非 Apple 平台（CMake 平台选源，D-4）。**前置：M0-2 合入**（`nativeWindowInfo` mac 填 NSWindow，否则三项全部收 nullptr，§3）。

### 6.1 `window_style_macos.mm`（hideFromTaskbar 等价）

`applyWindowStyleFlags(void* glfwWindow, const WindowStyleFlags& flags)`：`hideFromTaskbar` 时对 NSWindow 设 `collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorIgnoresCycle`（mach 常驻全部 Space、⌘Tab 循环忽略；mac 窗口本就不进 Dock，主窗口"任务栏"语义即此）。`Transparent`/其它 flag 映射按需，未知 flag 忽略。DevDesk 消费：`pet_window.cpp:678` `.hideFromTaskbar(true)`。句柄来源：`nativeWindowInfo(handle).platformWindow`（M0-2）。

### 6.2 `context_menu_macos.mm`（原生右键菜单）

`showContextMenu(void* glfwWindow, const std::vector<ContextMenuItem>& items)`：
- NSWindow 内 `NSMenu` 逐项构建（文本/禁用/分隔符），`popUpMenuPositioningItem:atLocation:` 在窗口坐标位置弹出（⚠️ 坐标从屏幕坐标换算窗口坐标）；
- 菜单项 action（block）记录选中 index，返回 `std::vector<int>{selectedPath}`；点空白/取消 → 返回 `nullopt`（与 stub 语义一致，调用方照旧回退）。
- DevDesk 消费：桌宠右键菜单（`context_menu_stub.cpp:3` 注释自述的"桌宠右键菜单"路径 —— 现在回退自绘 contextMenu 组件，小窗裁剪；mac 升级为原生弹出）。

> **⚠️ 坐标系注记（V2 补）**：`popUpMenuPositioningItem:atLocation:inView:` 的 `atLocation` 是 contentView **视图坐标、原点在左下**（Cocoa 约定，contentView 默认非 flipped），不是左上、也不是全局屏幕坐标。从屏幕坐标（同为左下原点，但含菜单栏/屏区边界）换算须两步：① `convertRectFromScreen:` / `convertPoint:fromView:nil` 转成窗口/视图坐标；② 纵向翻转 `y = contentView 高度 − y`（若上层给的是左上原点逻辑坐标）。与 §4.1 的 `mouseLocation` 翻转（屏高 − y，屏系）同理但对象不同——一处是视图系、一处是屏系，别混用。

### 6.3 `power_events_macos.mm`（sleep/wake 通知）

- `installSystemPowerNotifications(void*)`：NSWorkspace notificationCenter 注册 `NSWorkspaceWillSleepNotification` / `NSWorkspaceDidWakeNotification`（⚠️ AppKit/Foundation 通知，不依赖窗口句柄，参数忽略）；
- 回调（ObjC block → C++）：映射 `SystemPowerEvent`（Sleep / Resume，枚举对齐 Windows `WM_POWERBROADCAST` 语义），调 `setSystemPowerHandler` 存的 handler；
- `uninstallSystemPowerNotifications()`：移除通知注册。
- DevDesk 消费：桌宠 M2（睡眠暂停动画/计时，`desktop-pet-design.md`）。
- 不做 IOKit 电池/低电量（§9）。

## 7. 跨仓库交付线

按 DevDesk CLAUDE.md「反事故红线」：

1. **隔离 worktree agent**（`isolation: "worktree"`）为每个任务（M0/M1/M2/M3）改 EUI-NEO 源：隔离 agent 无法对 EUI-NEO 仓库 git 操作，但可在自己的 worktree 内**用文件副本模拟依赖**——把改后的 EUI-NEO 源拷成 `<worktree>/eui-mock/`（或把 DevDesk 消费点源码一并放副本），`EUI_NEO_DIR=<worktree>/eui-mock` 真跑 DevDesk 构建/定向单测验证改动可编译、消费 API 匹配；
2. 任务完成 → `git format-patch` 产出（base = 该任务分支起点；多 commit 成一链）→ 报告 patch 路径 + sha + 验证摘要；
3. **非隔离 agent**（不设 `isolation`）在 EUI-NEO 主树 `git apply` + 编译 + 原子 commit；prompt 写死「只许改 `D:/project/gui/EUI-NEO`，禁止碰 DevDesk 任何文件」；commit message 注明需求来源（"DevDesk 需要 X 功能"，EUI-NEO 仓库约定）；
4. 主会话读 diff 审计 + 验收（Patch 内容与任务声明一致、未夹带）；
5. **收尾**（M0-M3 全部合入后）：bump `DEVDESK_EUI_NEO_MIN_REV`（`DevDesk/CMakeLists.txt` + DevDesk `CLAUDE.md`「常用命令」段**两处同步**，当前 `269a8c4`）——bump 改的是 DevDesk 侧文件，走 DevDesk 侧 worktree 线执行（或 EUI 合入后由主会话派 DevDesk 侧收尾子任务，见下条第 6 条）；
6. **DevDesk 侧改动归属（V2 补）**：§8.2 的 `settings_config.cpp` 文案平台化（§5.3 案 A/B）与收尾 bump 均属 DevDesk 仓库改动——红线禁止「非隔离线 agent 碰 DevDesk」（其工作目录就是主树），一律由 **DevDesk 侧隔离 worktree 子任务**执行（普通交付流水线：worktree 子任务 → 原子 commit → 主会话审计合并），**不走本节的 EUI 补丁线**。串行依赖：**案 A** 需 EUI 侧 hint 接口先经本节补丁线合入 EUI-NEO main，DevDesk 接线后行（EUI 修订在前、DevDesk 接线在后）；**案 B** 中性文案改一行、零跨仓依赖，DevDesk 线可独立先行、不必等 EUI（§13-1 拍板选案）；
7. EUI-NEO 本地 main 领先 origin 的提交**永不 push**（2026-09-28 用户决策，origin/main 停在 v0.6.0）——本批改动继续落本地 main，与 push 无关（§13-3）。

## 8. 文件改动清单

### 8.1 EUI-NEO 侧

| 任务 | 文件 | 动作 |
|------|------|------|
| M0 | `EUI-NEO/core/app/glfw_app_main.cpp` | 改：`nativeWindowHandle`（L252-259）`#else` 拆 `#elif defined(__APPLE__)` 返 `glfwGetCocoaWindow(window)`（§3 M0-1） |
| M0 | `EUI-NEO/core/window/window_backend.cpp` | 改：GLFW 变体 `nativeWindowInfo`（L831-843）加 `__APPLE__` 分支填 `platformWindow`（§3 M0-2；SDL2 变体 L502-525 已有 `__APPLE__`，不动） |
| M1 | `EUI-NEO/core/platform/tray_bridge.c` | 改（**M1 唯一实作点**，后端已定案 §4.1/§13-6）：appkit 段（L133-340）`statusBarButton` 挂 action + `sendActionOn:(NSLeftMouseUpMask\|NSRightMouseUpMask)` + `tray_button_action` 分流（坐标翻转 + 调钩子 + 0 时 `popUpStatusItemMenu:`）；挂-menu 共存争议与预备改法见 §4.2 |
| M1 | `EUI-NEO/3rd/tray/tray.h` | **不改**（EUI 构建不编译 `TRAY_APPKIT` 分支；原 V1 §3.1/§3.2 方案作废，§4.3。仅 3rd/tray 独立复用场景另行提交其仓库） |
| M2 | `EUI-NEO/core/platform/window_effect_macos.mm` | **新建**：NSVisualEffectView 磨砂（§5.2） |
| M2 | `EUI-NEO/CMakeLists.txt` | 改：L265-294 四族选源 `if(WIN32)/else()` 拆 `elseif(APPLE)`（mac 编 `*_macos.mm`，stub 收窄给非 Apple）；顶层 `project()` 之后加 `enable_language(OBJCXX)` 让 `.mm` 进 eui_neo 编译链；APPLE 链接段 L316-323 可扩（Cocoa/objc 已由 L404-406 覆盖） |
| M3 | `EUI-NEO/core/platform/window_style_macos.mm` | **新建**：collectionBehavior 等价（§6.1） |
| M3 | `EUI-NEO/core/platform/context_menu_macos.mm` | **新建**：NSMenu popUp（§6.2） |
| M3 | `EUI-NEO/core/platform/power_events_macos.mm` | **新建**：NSWorkspace sleep/wake（§6.3） |
| M3 | `EUI-NEO/CMakeLists.txt` | 改：追加上述三文件 mac 选源（与 M2 同文件，写者约束见 §11） |

### 8.2 DevDesk 侧（全部走 DevDesk 侧 worktree 线，§7-6）

| 任务 | 文件 | 动作 |
|------|------|------|
| M2 | `DevDesk/src/module_settings/settings_config.cpp` | 改：`windowEffectDegradedHint` frosted 分支文案平台化（§5.3 案 A 或 B）——**DevDesk 侧线执行**：案 A 依赖 EUI hint 接口先合入（§7-6 串行）；案 B 一行中性文案零跨仓依赖可独立先行 |
| 收尾 | `DevDesk/CMakeLists.txt` + `DevDesk/CLAUDE.md` | 改：`DEVDESK_EUI_NEO_MIN_REV` bump 两处同步（当前 `269a8c4`，§7-5） |

M1 预期 DevDesk 侧零改动（`main.cpp:566-575` / `:390-408` 已就绪）。

## 9. 不做（YAGNI）

- **不引任何 vibrancy/objc2 crate**（D-2 Tahoe 风险门）；
- **不做 Linux 对等实现**：磨砂、桌宠三项的 stubs 保留给非 Apple 平台（收窄后仍服 Linux）；
- 托盘不做：图标动画、拖拽移动、多屏弹出定位自适应（单屏翻转先行）、Touch Bar；
- 磨砂不做：分区磨砂、圆角蒙版、材质强度/类型用户可配（固定 classic 组合）、`titlebarAppearsTransparent` 联动；
- 桌宠不做：IOKit 电池/低电量事件（只 sleep/wake）、Dock 真隐藏按钮（mac 无此概念）、按 Space 可配行为；
- 不做 mac CI/自动化构建（成稿时本机无 mac；2026-09-28 起本机已是 macOS，仍不建 CI），验收靠 §10 Mac 实机清单。

## 10. 测试

### 10.1 静态评审（本机可做，每任务）

- 实现后 codegraph/grep 复核改动点与 §2 / §8 行号一致、无遗漏消费点（含 M0 两处句柄供给的消费链：`glfw_app_main.cpp:252-269` 与 `window_backend.cpp:831-843`）；
- 双审（spec compliance + code quality）：改动非机械，不合并为一审；
- **Windows 回归（必须全绿）**：M0-M3 全部为 `__APPLE__` 分支追加或新建 mac 文件，Win/Posix 路径字节不动；eui-tray / window_effect / pet 冒烟 + 定向单测；
- 隔离 agent 构建验证（§7-1）：EUI-NEO 文件副本 + `EUI_NEO_DIR` 真编译真跑测试，证明消费 API 匹配（mac 编译器覆盖不到，见 §12-5）。

### 10.2 Mac 实机验收清单（**截至 2026-09-28 未执行**；本机已具备 macOS 环境，待排期）

| 任务 | 验收项 |
|------|--------|
| M0 | ① `nativeWindowHandle`（glfw_app_main.cpp）/ `nativeWindowInfo` GLFW 变体（window_backend.cpp）在 mac 返回非空 NSWindow*（M2/M3 各验收项的前置；实现后加一行诊断日志验证） |
| M1 | ⓪ **挂 action 与 `setMenu:` 共存**（AppKit 已知争议点：挂 menu 时点击自动弹菜单、action 可能不触发）——action 不触发则切预备改法：不挂 menu、左右键全走显式 `popUpStatusItemMenu:`（§4.2）；① 左键单击托盘 → 唤起主窗口；② 右键单击 → styled 菜单（与 Win 同款外观）；③ 菜单位于图标正下方、不超屏（坐标翻转正确）；④ 降级路径（styled 构建失败/0 返回）仍弹原生菜单；⑤ 菜单项点击回调正确 |
| M2 | ① 三档（off/translucent/frosted）切换视觉正确；② 磨砂材质深/浅主题均正常、主题切换保持；③ 设置页降级/生效文案平台化后正确；④ Tahoe 不生效时显示平台化降级文案、不崩 |
| M3 | ① 桌宠无边框置顶透明窗口正常开/关/拖；② 不进 Dock、⌘Tab 不出现（collectionBehavior）；③ 桌宠右键弹原生菜单、点项回调正确；④ 睡眠→唤醒后动画随 willSleep/didWake 暂停/恢复 |

## 11. 拆任务

| 任务 | 内容 | 依赖 |
|------|------|------|
| **M0** | 原生句柄供给（§3：`glfw_app_main.cpp` + `window_backend.cpp` 各加 `__APPLE__` 分支） | 无；**阻塞性串行先行**（M2/M3 依赖；M1 不依赖） |
| **M1** | 托盘 styled 菜单（§4：tray_bridge.c appkit 段挂 action 分流 + 共存争议预备改法；**不改 3rd/tray**） | 无（与 M0 无依赖，可并行） |
| **M2** | 磨砂（§5：`window_effect_macos.mm` + 顶层 CMakeLists 选源 + 文案平台化→DevDesk 线） | **M0** |
| **M3** | 桌宠三项（§6：三个 mac 平台文件 + 顶层 CMakeLists 追加） | **M0**（顶层 CMakeLists.txt 与 M2 同文件——并行时只追加各任务自己的段，或由主会话合并派单，遵守"同一文件同一时刻只有一个写者"） |
| **收尾** | bump `DEVDESK_EUI_NEO_MIN_REV`（两处同步，DevDesk 侧线）+ Win 全量回归 | M0-M3 全部合入后 |

M0-M3 各独立；**M0 阻塞性串行先行（M2/M3 依赖、M1 不依赖）**，M1 可与 M0 并行；每个任务完整走 §7 交付线（隔离 worktree 产 patch → 非隔离线 apply + 编译 + commit）后并入 EUI-NEO main；DevDesk 侧 `settings_config.cpp` 文案改动不属 EUI 补丁线（§7-6）。

## 12. 已接受边界

1. **Tahoe 磨砂失败** → Transparent 降级交付（不阻塞，D-3）；
2. **NSVisualEffectView 与 GL view 的合成顺序 / 唤醒（GL 上下文唤醒）** 未实机验证（⚠️）；
3. **坐标翻转方向为设计推断**（Cocoa 左下原点 → 左上），可能反——实机验证后微调一行；**§6.2 的 `atLocation` 为 contentView 视图坐标（左下原点），从屏幕坐标换算需窗口坐标转换 + 纵向翻转**，与托盘 `mouseLocation` 的屏系翻转是两套原点，勿混用（⚠️）；
4. **挂 action + `setMenu:` 共存**为 AppKit 已知争议点（挂 menu 后点击自动弹菜单、action 可能不触发），显式 `popUpStatusItemMenu:` 的时序/焦点细节同样未实机验证（⚠️）——已升为 **M1 Mac 实机验证第 0 项**（§10.2），预备改法：不挂 menu、全走显式 `popUpStatusItemMenu:`（§4.2）；
5. **ObjC++（`.mm`）首次进 EUI-NEO 编译链**（顶层 CMakeLists 需 `enable_language(OBJCXX)`，§8.1）：成稿时本机（Windows）无 mac SDK，编译正确性只能实机验证；隔离 agent 的副本构建覆盖不到 mac 编译器；
6. ~~`3rd/tray` APPKIT 与 tray_bridge appkit 两个后端谁被 macOS 链接未定~~ —— **V2 已定案关闭**：走 tray_bridge 的 `EUI_TRAY_APPKIT` 段（§13-6），`3rd/tray` APPKIT 分支 EUI 构建不编译，原「两处改造点都给、实现阶段按编译配置取舍」的开放式安排不再需要，本项移出开放边界。

## 13. 待用户复核

1. 磨砂降级文案平台化：**案 A**（平台接口拆分，推荐，符合平台拆文件纪律；需 EUI hint 接口先落地，§7-6 串行）vs **案 B**（中性文案去 OS 名，改一行，零跨仓依赖可独立先行）；
2. mac 磨砂材质组合（`HUDWindow` vs `UnderWindowBackground`）与 translucent 透明度数值 —— 实机观感定；
3. ~~本批 EUI-NEO 改动（含既有 ~70 commits）**仍不 push**？~~ —— **已关闭（2026-09-28 用户决策）**：EUI-NEO 永不 push 到 origin（origin/main 停在 v0.6.0），本批随同一口径落本地 main；
4. 桌宠 `hideFromTaskbar` mac 语义（`CanJoinAllSpaces | IgnoresCycle`）是否符合"不进 Dock / ⌘Tab 不可见"预期；
5. 托盘**原生 NSMenu 降级菜单是否保留**（本设计保留为降级数据源，与 Win keepDefault=false 语义一致；§4.2 预备改法下"降级"= 显式 popUp 同一菜单对象）；
6. ~~macOS 构建实际走 `3rd/tray` 的 `TRAY_APPKIT` 还是 tray_bridge 的 appkit 后端~~ —— **已关闭（V2 静态定案，不再待复核）**：macOS 走 tray_bridge 的 `EUI_TRAY_APPKIT` 段（`EUI-NEO/CMakeLists.txt:404-406` `elseif(APPLE)` 定义 `EUI_TRAY_APPKIT=1` 并链接 Cocoa/objc；`tray_bridge.c:135`）；`3rd/tray` 的 `TRAY_APPKIT` 宏在 EUI 构建中从不定义（仅 3rd/tray 独立构建自定，见其 Makefile/example.c），该分支不参与编译 → M1 实作只改 `tray_bridge.c`（§4.1），不再需要实现者自证。