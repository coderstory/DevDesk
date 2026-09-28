# macOS 三项能力补齐（托盘 styled 菜单 / 磨砂 / 桌宠三项）—— EUI-NEO 上游线（设计）

日期：2026-09-28 ｜ 状态：**设计（未实施）** ｜ 关联：`2026-09-20-devdesk-design.md`（权威 spec，实现前先读）、`2026-09-22-eui-window-frost-design.md`（磨砂全链既有设计，本 spec 是其 macOS 落地）、`2026-09-22-desktop-pet-design.md`（桌宠设计）

> **证据等级说明**：本机是 Windows 开发机（无 macOS 实机），凡 macOS 侧 API 行为 / 坐标方向 / ObjC++ 编译链均为**设计推断**，本文按 ⚠️ 标注；实施与验收以 §9 Mac 实机清单为准，不得把 ⚠️ 项当既定事实引用。

## 1. 需求与决策

DevDesk 目标平台含 macOS（macOS 主线 P0-P8 已完成，2026-09 迁 Windows 后平台适配代码按 `*_win.cpp` / `*_macos.cpp` / `*_posix.cpp` 拆文件保留可用）。三项 macOS 能力补齐（输入为已完成预研结论，本 spec 用 codegraph / Read 逐条核实 file:line，见 §2）：

| # | 需求 | 现状缺口 |
|---|------|----------|
| **R1** | 托盘菜单与 Windows 一致：左右键分流，右键/左键弹 **styled 自绘菜单**（round 角/主题/图标），原生 NSMenu 仅作降级 | macOS 托盘左右键都弹原生 NSMenu，无任何拦截点（`3rd/tray/tray.h` APPKIT 分支 `tray_update` 只 `setMenu:`） |
| **R2** | 窗口磨砂三档（off/translucent/frosted）在 macOS 可用；失败保留 Transparent 降级 | `window_effect_stub.cpp` 恒返回 false → 一直降级 Transparent |
| **R3** | 桌宠三项 mac 等价能力：不进 Dock/⌘Tab（hideFromTaskbar 等价）、原生右键菜单、电源事件（sleep/wake） | 三个平台 stubs：`window_style_stub` / `context_menu_stub` / `power_events_stub` 全静默降级 |

| # | 决策 | 理由 |
|---|------|------|
| **D-1** | 三项全部走 **EUI-NEO 上游线**（修改 `../EUI-NEO/`，DevDesk 侧除文案平台化与版本 bump 外零改动） | DevDesk CLAUDE.md「EUI-NEO 上游贡献政策」：遇到 EUI-NEO 能力缺失优先修上游而非 DevDesk workaround；托盘/窗口效果/窗口行为均属通用平台能力 |
| **D-2** | 磨砂**不引任何 crate**（window-vibrancy / objc2 等），ObjC++ 直挂 `NSVisualEffectView` | window-vibrancy（0.5）在 macOS 26 Tahoe 上 4 次尝试全败（crate、Tauri 2 官方 EffectsBuilder、裸 objc2 FFI 均产不出可用 vibrancy）；本实现同机制、大概率同结果，故走最薄路径并留降级 |
| **D-3** | 磨砂失败 → **保留 Transparent 降级交付**，不阻塞 | stub 现状即降级路径，调用链天然支持（§4）；Tahoe 若真不支持，交付形态劣于 Windows 但行为不退化 |
| **D-4** | 平台实现按接口拆文件（`*_macos.cpp` / `*_macos.mm`），与既有 `*_win.cpp` / `*_posix.cpp` 同模式，业务代码不散布平台宏 | DevDesk CLAUDE.md 核心约束 5 / EUI-NEO 后端同模式；stubs 收窄为仅非 Apple 平台使用 |
| **D-5** | M1 托盘 / M2 磨砂 / M3 桌宠三项**独立任务**，各自走 §6 跨仓库交付线（隔离 worktree 产 patch → 非隔离线 apply + 编译 + commit） | 三个任务改 EUI-NEO 独立文件、共享头接口签名零改动，冲突面最小；跨仓库施工受隔离守卫约束，必须"产补丁 + 非隔离线应用"（DevDesk CLAUDE.md 反事故红线） |
| **D-6** | 收尾统一 bump `DEVDESK_EUI_NEO_MIN_REV`（顶层 CMakeLists.txt + DevDesk CLAUDE.md 两处同步），不掺进 M1-M3 | DevDesk CLAUDE.md：消费 EUI 新 API 后两处同步更新，当前依赖 rev `269a8c4` |
| **D-7** | 不做 mac 自动化构建/CI、不做 Linux 对等实现（§8） | 本机无 mac，无法在本仓库验证 mac 编译；Linux 无宿主需求 |

## 2. 现状事实（源码核实）

> 本节全部条目经 codegraph（projectPath=`D:/project/gui`）与直读核实，文件名无特殊标注即 **CONFIRMED**；⚠️ 为设计推断。

### 2.1 托盘：stub 无、Win 有拦截点、APPKIT 无

| 事实 | 证据 |
|------|------|
| APPKIT 分支入口 `#elif defined(TRAY_APPKIT)`，状态 `statusItem` / `statusBarButton` 为静态 | `EUI-NEO/3rd/tray/tray.h:91`、`:99-100` |
| APPKIT `tray_update` **只做两件事**：`setImage:` + `[statusItem setMenu:]` —— 左右键点击都由 AppKit 自动弹原生 NSMenu，**无 left/right 分流、无任何回调拦截点** | `EUI-NEO/3rd/tray/tray.h:196-202` |
| Win32 对照（要迁移的语义）：`WM_TRAY_CALLBACK_MESSAGE` 里 `lparam == WM_LBUTTONUP \|\| WM_RBUTTONUP` 分流（L248）→ `eui_tray_menu_requested(p.x, p.y, left)` 返回非 0 则跳过原生 `TrackPopupMenu`（L251-253）；0 走原生（L254-257） | `EUI-NEO/3rd/tray/tray.h:247-260` |
| `eui_tray_menu_requested` 的**前向声明位于 `#elif defined(TRAY_WINAPI)` 段内**（L206 起），APPKIT 分支（L91 起）在它之前编译不到 → 必须上移到共用段 | `EUI-NEO/3rd/tray/tray.h:227`（声明行）、`:206`（段起点） |
| 钩子注册与实现：`eui_tray_set_menu_requested_fn` 存函数指针（L122-124），`eui_tray_menu_requested(x, y, left_button)` 转发（L126-131，未注册返回 0 = 原生路径）；注释明确"上层（app 层 styled 菜单等）已接管本次菜单展示，原生弹出被跳过" | `EUI-NEO/core/platform/tray_bridge.c:116-131` |
| DSL 层注册入口：`setTrayMenuRequestedHandler` → `trayMenuRequestedTrampoline` → `eui_tray_set_menu_requested_fn` | `EUI-NEO/core/platform/platform.cpp:747-763` |
| ⚠️ EUI-NEO tray bridge 存在**第二个 APPKIT 实现**（`g_status_item` + `[g_status_item setMenu:g_menu]`，L232-235），与 `3rd/tray` 的 `TRAY_APPKIT` 后端并存；macOS 构建实际链接哪个由 EUI-NEO 后端选择宏决定，**实现阶段必须先行确认**（§12-6） | `EUI-NEO/core/platform/tray_bridge.c:227-236` |
| DevDesk 消费点：`.tray(true)` / `.trayMenu(nativeTrayMenuItems(), false)`（keepDefault=false，原生菜单仅为降级数据源）/ `.onTrayMenu(...)` —— 左键 `ShowMainWindow` 返回 true，右键 `showTrayStyledMenu(x, y)` | `DevDesk/src/main.cpp:566-575` |
| DevDesk styled 菜单已就绪：`showTrayStyledMenu` 全函数（构建 items → `components::showStyledMenu("tray", items, style, x, y, 回调)`），Windows 已在用 | `DevDesk/src/main.cpp:390-408`（`showStyledMenu` 调用在 **:400**） |

### 2.2 磨砂：stub 恒 false 与降级链

| 事实 | 证据 |
|------|------|
| `applyWindowEffect(void*, WindowEffect)` 与 `applyTitleBarAppearance` **恒返回 false**（注释自述"macOS 的 NSVisualEffectView 磨砂 … 属磨砂设计文档 Phase E"） | `EUI-NEO/core/platform/window_effect_stub.cpp:8-14` |
| 降级链：GL 后端应用窗口效果失败 → `degradedWindowEffect`（回退档位，L66）→ 回写 `activeWindowEffect`（主循环应用/降级后写回，`dsl_app_impl.h:65-66` 注释、`:431-433`）→ 设置页经 `activeWindowEffect()` 回查（`:416-422`）展示真实档位 | `EUI-NEO/core/platform/window_effect.h`（`systemBackdropValue:53` / `degradedWindowEffect:66`）、`EUI-NEO/include/eui/detail/dsl_app_impl.h:403-433` |
| DevDesk 接线：启动 `module::settings::applyWindowVisuals(cfg.windowEffect)`（clearColor alpha 按档 + `app::setWindowEffect` 请求 backdrop）；启动快照 `.windowEffect(WindowEffect::Transparent)`（透明帧缓冲 hint 创建期不可逆，off 档靠 alpha=1 视觉等价） | `DevDesk/src/main.cpp:513-518`、`:550-554` |
| 降级提示文案 **"需 Windows 11 22H2+" 是 Win 专属**，mac 路径会显示错误信息 → 需平台化（§4.3） | `DevDesk/src/module_settings/settings_config.cpp:210-218`（关键文案在 **:215**） |
| `applyWindowVisuals` 全函数：alpha 按档 + `setWindowEffect(windowEffectRequest(mode))` | `DevDesk/src/module_settings/settings_config.cpp:223-231` |

### 2.3 桌宠三项 stubs

| 事实 | 证据 |
|------|------|
| `applyWindowStyleFlags(void*, flags)` 恒 false（注释："macOS/Linux stub：桌宠设计 G5 的任务栏隐藏是 Windows 专属需求（macOS 子窗口本就不进 Dock）"）→ `hideFromTaskbar` 在 mac 永不生效 | `EUI-NEO/core/platform/window_style_stub.cpp:8-10` |
| `showContextMenu` 恒返回 `nullopt`（注释："macOS NSMenu popUp 可后续补…桌宠右键菜单回退自绘 contextMenu 组件"） | `EUI-NEO/core/platform/context_menu_stub.cpp:9-14` |
| `setSystemPowerHandler` 存储但**永不触发**；`install/uninstallSystemPowerNotifications` 空操作（注释：sleep/wake 通知 mac 对应 IOKit/NSWorkspace） | `EUI-NEO/core/platform/power_events_stub.cpp:12-19` |
| DevDesk 桌宠开窗消费点：`app::openWindow(DslWindowConfig{…})` —— `.decorated(false)`（:674）`.alwaysOnTop(true)`（:675）`.focusOnShow(false)`（:677）`.hideFromTaskbar(true)`（**:678**）`.transparentFramebuffer(true)`（:679）`.windowEffect(None)`（:685）`.onWindowCreated`（:688）`.onWindowDestroyed`（:697） | `DevDesk/src/module_pet/pet_window.cpp:669-702` |

### 2.4 平台纪律与交付约束

| 事实 | 证据 |
|------|------|
| 平台适配代码接口隔离：头文件平台无关接口，`*_win.cpp` / `*_macos.cpp` / `*_posix.cpp`，CMake 按平台选源；业务代码禁散布 `#ifdef` | `DevDesk/CLAUDE.md` 核心约束 5 |
| 跨仓库改动必须"产补丁 + 非隔离线应用"：隔离 worktree agent 无法对其它仓库 git 操作；非隔离线 prompt 写死只许改目标仓库 | `DevDesk/CLAUDE.md`「反事故红线」 |
| EUI-NEO 当前依赖 rev `269a8c4`（2026-09-27），本地 main 领先 origin 70 个提交未 push（push 是用户决策门控）；`DEVDESK_EUI_NEO_MIN_REV` 在顶层 CMakeLists.txt 与 CLAUDE.md 两处同步 | `DevDesk/CLAUDE.md`「常用命令」段 |

## 3. 托盘 styled 菜单（M1）

目标：macOS 托盘点击行为与 Windows 对齐 —— 左键 = 唤起主窗口，右键 = styled 菜单；原生 NSMenu 仅保留为降级。

### 3.1 `eui_tray_menu_requested` 声明上移

现声明在 `tray.h:227`（`#elif defined(TRAY_WINAPI)` 段内）。上移到共用段（`tray.h:22` 的 `static void tray_update(...)` 前置声明附近、`#if defined(TRAY_APPINDICATOR)` 之前），APPKIT / Win32 两分支共用。Win32 分支调用点（L251）零改动。实现仍在 `tray_bridge.c:126`（APPKIT 分支下也可达——桥文件是 C，编译进所有后端）。⚠️ 若 §12-6 确认桥 appkit 后端被 macOS 选用，则 `3rd/tray` 的 APPKIT 分支可能根本不参与 macOS 编译——**两处改造点本 spec 都给出**，实现阶段按实际链接配置取舍，避免二选一后被编译宏打脸。

### 3.2 APPKIT 左右键分流（3rd/tray/tray.h）

在 `tray_init`（L145-194）初始化阶段对 `statusBarButton`：

1. `setTarget:` / `setAction:` 挂新增的 action 回调（新 static 函数 `tray_button_action`）；
2. `sendActionOn:(NSLeftMouseUpMask | NSRightMouseUpMask)` —— NSButton 默认只在左键 AC 时发 action，`sendActionOn:` 把右键一并纳入；
3. `tray_button_action` 体内：取 `[[NSApp currentEvent] type] == NSRightMouseUp` → `left_button = 0`，否则 1；取 `[[NSApp currentEvent] mouseLocation]`（⚠️ Cocoa 屏幕坐标，**原点在主屏左下**）→ 翻转 `y = 屏高 - mouseLocation.y` 到左上原点（§11-3 方向边界）→ 调 `eui_tray_menu_requested(x, y, left_button)`：
   - **返回非 0**：上层（DSL handler → DevDesk `onTrayMenu`）已接管，Mac 端 DevDesk 已实现 styled 菜单路径（main.cpp:569-575），原生路径完全跳过 —— 与 Win32 `tray.h:251-253` 语义一致；
   - **返回 0**：恢复原生行为 —— `popUpStatusItemMenu:` 弹出当前菜单（⚠️ 挂 action 后 `setMenu:` 的自动弹出被取代，原生路径必须显式 popUp，时序细节 §11-4）；
4. `tray_update`（L196-202）保持 `setImage:` + `setMenu:`（菜单内容随 `eui_tray_menu_changed` 更新），分流职责全部移到 button action。

### 3.3 桥 appkit 后端对齐（tray_bridge.c，视 §12-6）

`[g_status_item setMenu:g_menu]`（L234）的更新路径保持；同样在 `g_status_item` 的 `statusBarButton` 上挂 action + `sendActionOn:` 分流，action 内直接调 `eui_tray_menu_requested`（桥内已有该函数，L126 天然可达），返回 0 时 `popUpStatusItemMenu:`。两处实现共用同一钩子函数，DevDesk/DSL 侧零改动。

### 3.4 DevDesk 复用（预期零改动）

`platform.cpp:753-763` 已把 DSL handler 注册为 `eui_tray_menu_requested_fn`；DevDesk `main.cpp:569-575` 已实现左键 `ShowMainWindow` / 右键 `showTrayStyledMenu`（L400 的 `components::showStyledMenu` 直接消费 x/y）。mac 端接上分流后同一回调路径自动生效。

## 4. 磨砂（M2）

### 4.1 现状链路（§2.2）

stub 恒 false → GL 后端降级 → `activeWindowEffect` 回写 → 设置页展示降级文案（现为 Win 专属文案）。

### 4.2 `window_effect_macos.mm`（新建，当前不存在）

- 位置 `EUI-NEO/core/platform/window_effect_macos.mm`，`.mm` 后缀走 ObjC++；
- 签名不变：`bool applyWindowEffect(void* /*GLFWwindow*/, WindowEffect)` / `bool applyTitleBarAppearance(void*, const TitleBarAppearance&)`（true = 生效；false = 走既有降级链）；
- 实现路径：`glfwGetCocoaWindow(glfwWindow*)` 取 NSWindow → `contentView` 层级插入 `NSVisualEffectView`：`setMaterial:` classic 组合（`HUDWindow` / `UnderWindowBackground`）+ `blendingMode: WithinWindow` + `state: Active`，**垫在 GL view 之下**（GL view 需透明/wantsLayer 才能透出材质，⚠️ 合成顺序实机验证）；
- 档位映射：`Frosted`（Acrylic 语义）→ NSVisualEffectView 可见；`Translucent` → view 透明度（数值 ⚠️ 待 Mac 实机定，§12-2）；`None` → view 隐藏/移除；
- **Tahoe 风险门**：不引 crate（D-2）；ObjC++ 直挂失败（不生效或异常）→ 返回 false → 既有 `degradedWindowEffect` + `activeWindowEffect` 回写链自动降级 Transparent，**不崩、不阻塞**（D-3）。

### 4.3 DevDesk 降级文案平台化

`settings_config.cpp:210-218` frosted 分支的 "需 Windows 11 22H2+"（:215）是 Win 专属。两案：

- **A（推荐，符合 D-4 平台拆文件纪律）**：新增平台接口如 `std::string windowEffectUnsupportedHint(mode)`（`*_win.cpp` 返回现文案；`*_macos.cpp` 返回"当前系统不支持磨砂，已回退为半透"；`*_posix.cpp` 通用文案），`windowEffectDegradedHint` 改调它；
- **B（最小）**：中性文案去掉 OS 专属信息（如"当前系统不支持磨砂，已回退为半透"），不新增接口。

§12-1 待用户复核。

## 5. 桌宠三项（M3）

三个 mac 平台文件（新建，均在 `EUI-NEO/core/platform/`），接口签名与 stubs 完全一致，stubs 收窄为仅非 Apple 平台（CMake 平台选源，D-4）：

### 5.1 `window_style_macos.mm`（hideFromTaskbar 等价）

`applyWindowStyleFlags(void* glfwWindow, const WindowStyleFlags& flags)`：`hideFromTaskbar` 时对 NSWindow 设 `collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorIgnoresCycle`（mach 常驻全部 Space、⌘Tab 循环忽略；mac 窗口本就不进 Dock，主窗口"任务栏"语义即此）。`Transparent`/其它 flag 映射按需，未知 flag 忽略。DevDesk 消费：`pet_window.cpp:678` `.hideFromTaskbar(true)`。

### 5.2 `context_menu_macos.mm`（原生右键菜单）

`showContextMenu(void* glfwWindow, const std::vector<ContextMenuItem>& items)`：
- NSWindow 内 `NSMenu` 逐项构建（文本/禁用/分隔符），`popUpMenuPositioningItem:atLocation:` 在窗口坐标位置弹出（⚠️ 坐标从屏幕坐标换算窗口坐标）；
- 菜单项 action（block）记录选中 index，返回 `std::vector<int>{selectedPath}`；点空白/取消 → 返回 `nullopt`（与 stub 语义一致，调用方照旧回退）。
- DevDesk 消费：桌宠右键菜单（`context_menu_stub.cpp:3` 注释自述的"桌宠右键菜单"路径 —— 现在回退自绘 contextMenu 组件，小窗裁剪；mac 升级为原生弹出）。

### 5.3 `power_events_macos.mm`（sleep/wake 通知）

- `installSystemPowerNotifications(void*)`：NSWorkspace notificationCenter 注册 `NSWorkspaceWillSleepNotification` / `NSWorkspaceDidWakeNotification`（⚠️ AppKit/Foundation 通知，不依赖窗口句柄，参数忽略）；
- 回调（ObjC block → C++）：映射 `SystemPowerEvent`（Sleep / Resume，枚举对齐 Windows `WM_POWERBROADCAST` 语义），调 `setSystemPowerHandler` 存的 handler；
- `uninstallSystemPowerNotifications()`：移除通知注册。
- DevDesk 消费：桌宠 M2（睡眠暂停动画/计时，`desktop-pet-design.md`）。
- 不做 IOKit 电池/低电量（§8）。

## 6. 跨仓库交付线

按 DevDesk CLAUDE.md「反事故红线」：

1. **隔离 worktree agent**（`isolation: "worktree"`）为每个任务（M1/M2/M3）改 EUI-NEO 源：隔离 agent 无法对 EUI-NEO 仓库 git 操作，但可在自己的 worktree 内**用文件副本模拟依赖**——把改后的 EUI-NEO 源拷成 `<worktree>/eui-mock/`（或把 DevDesk 消费点源码一并放副本），`EUI_NEO_DIR=<worktree>/eui-mock` 真跑 DevDesk 构建/定向单测验证改动可编译、消费 API 匹配；
2. 任务完成 → `git format-patch` 产出（base = 该任务分支起点；多 commit 成一链）→ 报告 patch 路径 + sha + 验证摘要；
3. **非隔离 agent**（不设 `isolation`）在 EUI-NEO 主树 `git apply` + 编译 + 原子 commit；prompt 写死「只许改 `D:/project/gui/EUI-NEO`，禁止碰 DevDesk 任何文件」；commit message 注明需求来源（"DevDesk 需要 X 功能"，EUI-NEO 仓库约定）；
4. 主会话读 diff 审计 + 验收（Patch 内容与任务声明一致、未夹带）；
5. **收尾**（M1-M3 全部合入后）：bump `DEVDESK_EUI_NEO_MIN_REV`（顶层 `CMakeLists.txt` + DevDesk `CLAUDE.md`「常用命令」段**两处同步**，当前 `269a8c4`）；
6. EUI-NEO 本地 main 领先 origin ~70 commits 未 push（push 与否是用户决策门控）——本批改动继续落本地 main，与 push 决策解耦（§12-3）。

## 7. 文件改动清单

### 7.1 EUI-NEO 侧

| 任务 | 文件 | 动作 |
|------|------|------|
| M1 | `EUI-NEO/3rd/tray/tray.h` | 改：`eui_tray_menu_requested` 声明上移（L227 → 共用段）；APPKIT `tray_init` 挂 `statusBarButton` action + `sendActionOn:(NSLeftMouseUpMask\|NSRightMouseUpMask)` `tray_button_action` 分流（坐标翻转 + 调钩子 + 0 时 `popUpStatusItemMenu:`） |
| M1 | `EUI-NEO/core/platform/tray_bridge.c` | 改：appkit 段（L227-236）对齐同款分流（若 §12-6 确认该后端被 macOS 选用）；否则只读确认 |
| M2 | `EUI-NEO/core/platform/window_effect_macos.mm` | **新建**：NSVisualEffectView 磨砂（§4.2） |
| M2 | `EUI-NEO/core/platform/CMakeLists.txt` | 改：mac 分支编译 `*_macos.mm`（OBJCXX）+ 链接 Cocoa/AppKit；`window_effect_stub.cpp` 从 mac 排除 |
| M3 | `EUI-NEO/core/platform/window_style_macos.mm` | **新建**：collectionBehavior 等价（§5.1） |
| M3 | `EUI-NEO/core/platform/context_menu_macos.mm` | **新建**：NSMenu popUp（§5.2） |
| M3 | `EUI-NEO/core/platform/power_events_macos.mm` | **新建**：NSWorkspace sleep/wake（§5.3） |
| M3 | `EUI-NEO/core/platform/CMakeLists.txt` | 改：追加上述三文件 mac 选源（与 M2 同文件，写者约束见 §10） |

### 7.2 DevDesk 侧

| 任务 | 文件 | 动作 |
|------|------|------|
| M2 | `DevDesk/src/module_settings/settings_config.cpp` | 改：`windowEffectDegradedHint` frosted 分支文案平台化（§4.3，案 A 或 B） |
| 收尾 | `DevDesk/CMakeLists.txt` + `DevDesk/CLAUDE.md` | 改：`DEVDESK_EUI_NEO_MIN_REV` bump 两处同步（当前 `269a8c4`） |

M1 预期 DevDesk 侧零改动（`main.cpp:566-575` / `:390-408` 已就绪）。

## 8. 不做（YAGNI）

- **不引任何 vibrancy/objc2 crate**（D-2 Tahoe 风险门）；
- **不做 Linux 对等实现**：磨砂、桌宠三项的 stubs 保留给非 Apple 平台（收窄后仍服 Linux）；
- 托盘不做：图标动画、拖拽移动、多屏弹出定位自适应（单屏翻转先行）、Touch Bar；
- 磨砂不做：分区磨砂、圆角蒙版、材质强度/类型用户可配（固定 classic 组合）、`titlebarAppearsTransparent` 联动；
- 桌宠不做：IOKit 电池/低电量事件（只 sleep/wake）、Dock 真隐藏按钮（mac 无此概念）、按 Space 可配行为；
- 不做 mac CI/自动化构建（本机无 mac），验收靠 §9 Mac 实机清单。

## 9. 测试

### 9.1 静态评审（本机可做，每任务）

- 实现后 codegraph/grep 复核改动点与 §2 / §7 行号一致、无遗漏消费点；
- 双审（spec compliance + code quality）：改动非机械，不合并为一审；
- **Windows 回归（必须全绿）**：全链不改 Win 路径（仅声明上移是共用段，不影响 Win 分支语义）；eui-tray / window_effect / pet 冒烟 + 定向单测；
- 隔离 agent 构建验证（§6-1）：EUI-NEO 文件副本 + `EUI_NEO_DIR` 真编译真跑测试，证明消费 API 匹配。

### 9.2 Mac 实机验收清单（无 mac 机 → 排期到用户侧）

| 任务 | 验收项 |
|------|--------|
| M1 | ① 左键单击托盘 → 唤起主窗口；② 右键单击 → styled 菜单（与 Win 同款外观）；③ 菜单位于图标正下方、不超屏（坐标翻转正确）；④ 降级路径（styled 构建失败/0 返回）仍弹原生菜单；⑤ 菜单项点击回调正确 |
| M2 | ① 三档（off/translucent/frosted）切换视觉正确；② 磨砂材质深/浅主题均正常、主题切换保持；③ 设置页降级/生效文案平台化后正确；④ Tahoe 不生效时显示平台化降级文案、不崩 |
| M3 | ① 桌宠无边框置顶透明窗口正常开/关/拖；② 不进 Dock、⌘Tab 不出现（collectionBehavior）；③ 桌宠右键弹原生菜单、点项回调正确；④ 睡眠→唤醒后动画随 willSleep/didWake 暂停/恢复 |

## 10. 拆任务

| 任务 | 内容 | 依赖 |
|------|------|------|
| **M1** | 托盘 styled 菜单（§3：声明上移 + APPKIT 分流 + 桥对齐） | 无 |
| **M2** | 磨砂（§4：`window_effect_macos.mm` + CMake + 文案平台化） | 无 |
| **M3** | 桌宠三项（§5：三个 mac 平台文件 + CMake 追加） | 无（CMakeLists.txt 与 M2 同文件——并行时只追加各任务自己的段，或由主会话合并派单，遵守"同一文件同一时刻只有一个写者"） |
| **收尾** | bump `DEVDESK_EUI_NEO_MIN_REV`（两处同步）+ Win 全量回归 | M1-M3 全部合入后 |

M1-M3 各独立、可并行；每个任务完整走 §6 交付线（隔离 worktree 产 patch → 非隔离线 apply + 编译 + commit）后并入 EUI-NEO main。

## 11. 已接受边界

1. **Tahoe 磨砂失败** → Transparent 降级交付（不阻塞，D-3）；
2. **NSVisualEffectView 与 GL view 的合成顺序 / 唤醒（GL 上下文唤醒）** 未实机验证（⚠️）；
3. **坐标翻转方向为设计推断**（Cocoa 左下原点 → 左上），可能反——实机验证后微调一行；
4. **挂 action + `sendActionOn:` 后原生菜单降级路径**（显式 `popUpStatusItemMenu:` 的时序/焦点细节）未验证（⚠️）；
5. **ObjC++（`.mm`）首次进 EUI-NEO 编译链**：本机（Windows）无 mac SDK，编译正确性只能实机验证；隔离 agent 的副本构建覆盖不到 mac 编译器；
6. **`3rd/tray` APPKIT 与 tray_bridge appkit 两个后端谁被 macOS 链接**未定（§12-6）——两处改造点都已给出，实现阶段按编译配置取舍。

## 12. 待用户复核

1. 磨砂降级文案平台化：**案 A**（平台接口拆分，推荐，符合平台拆文件纪律）vs **案 B**（中性文案去 OS 名，改一行）；
2. mac 磨砂材质组合（`HUDWindow` vs `UnderWindowBackground`）与 translucent 透明度数值 —— 实机观感定；
3. 本批 EUI-NEO 改动（含既有 ~70 commits）**仍不 push**？本批随同一门控落本地 main；
4. 桌宠 `hideFromTaskbar` mac 语义（`CanJoinAllSpaces | IgnoresCycle`）是否符合"不进 Dock / ⌘Tab 不可见"预期；
5. 托盘**原生 NSMenu 降级菜单是否保留**（本设计保留为降级数据源，与 Win keepDefault=false 语义一致）；
6. macOS 构建实际走 `3rd/tray` 的 `TRAY_APPKIT` 还是 tray_bridge 的 appkit 后端 —— 实现阶段首个确认项（可省，若实现者能自证）。