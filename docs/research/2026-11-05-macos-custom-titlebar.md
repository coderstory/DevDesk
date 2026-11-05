# macOS 自绘标题栏调研（need:132）

> 课题：EUI-NEO 当前 Windows 走自绘标题栏（40px）、macOS 走系统标题栏（FSCV，bodyTop=30），两端 bodyTop 漂移 10px。目标：macOS 改自绘标题栏，几何与 Windows 完全一致。
> 调研人：researcher ｜ 日期：2026-11-05 ｜ 工具调用：32 次（预算 40，未超）
> 池线索：clue 37（自绘壳可行性存疑待验）→ 本报告即为该条待验项的查证结论；新事实已入库 clue 42、43。

## 结论先行

1. **路线 A（保留红绿灯、定位进自绘 40px 条）可行且是行业唯一成熟路线**：Electron / Tauri / Qt(QWindowKit) 三家全部走「保留 `NSWindowStyleMaskTitled` + `FullSizeContentView` + 透明标题栏 + 移动/隐藏红绿灯」，无任何主流框架走纯 Borderless 全自绘。
2. **EUI-NEO 现有代码已走完路线 A 的前半程**：`EUI-NEO/core/platform/window_effect_macos.mm:262-278` 已在追加 FSCV + 透明标题栏（与 Electron `native_window_mac.mm` 同款三连调用），缺的只是「移动红绿灯 + 拖拽区」后半程。
3. **简报开头对 window_backend.cpp 注释的判断成立**：「GLFW undecorated = Borderless 不可 resize/拖拽」是真的（GLFW 一手源码 + 官方文档），但推翻它不需要 undecorated——路线 A 根本不碰 `GLFW_DECORATED`。
4. **最大技术坑不是可行性，是「重挂时机」**：GLFW 全屏切换、AppKit 原生全屏（绿灯）、`setTitle:`（Tauri #13044 实锤）、改 styleMask 后 zoom 钮回退（Electron 源码注释实锤）都会重置红绿灯位置/自定义 mask，需要一套通知钩子统一重挂。
5. **拖拽标准答案**：拖拽区 `mouseDown:` 里 `[window performWindowDragWithEvent:[NSApp currentEvent]]`（macOS 10.11+，Apple 一手），GLFW 已消费鼠标事件不构成障碍——用覆盖层 NSView 抢 hit-test 即可。

---

## Q1 路线 A：保留红绿灯、定位进自绘 40px 标题栏

### 结论：可行，Electron/Tauri/QWindowKit 均为此路线，风险全部已知可控。

前提：窗口保持 GLFW decorated（`NSWindowStyleMaskTitled` 在），`window_effect_macos.mm:262-278` 的三连（FSCV + `titlebarAppearsTransparent` + `titleVisibility`）继续生效——内容区此时已从 y=0 开始（这正是 macOS 目前 bodyTop=30 硬编码的由来，改自绘后砍掉该 inset）。

### API 链

```objc
NSWindow *w = glfwGetCocoaWindow(glfwWindow);

NSButton *close = [w standardWindowButton:NSWindowCloseButton];
NSButton *mini  = [w standardWindowButton:NSWindowMiniaturizeButton];
NSButton *zoom  = [w standardWindowButton:NSWindowZoomButton];

// Electron 式（推荐）：不换 superview，只整体平移 origin
// 注意 origin 是相对其 superview（标题栏容器）的，AppKit 坐标 y 向上
NSRect f = close.frame;
f.origin = NSMakePoint(f.origin.x + dx, f.origin.y + dy);
[close setFrame:f];   // 或 setFrameOrigin:
// mini、zoom 同理，各自平移相同向量 → 间距自动保留
```

- Apple API：[`standardWindowButton(_:)`](https://developer.apple.com/documentation/appkit/nswindow/standardwindowbutton(_:)) 取三钮；[`performDrag(with:)`](https://developer.apple.com/documentation/appkit/nswindow/performdrag(with:)) 拖拽（见 Q3）。
- 社区范式（gist）：`removeFromSuperview → setFrameOrigin → addSubview` 换容器式（[steve228uk gist](https://gist.github.com/steve228uk/c960b4880480c6ed186d)）——**不推荐**，换 superview 会损失 zoom 钮 hover 菜单等容器级行为（[Innei 对 zoom 钮 hover 菜单的分析](https://innei.in/en/posts/tinkering/macos-zoom-button-hover-menu)）。

### Electron 一手实现（精读 `shell/browser/native_window_mac.mm`）

- 窗口 mask **保持 `NSWindowStyleMaskTitled`**，仅 `setTitlebarAppearsTransparent:YES` + `setTitleVisibility:NSWindowTitleHidden` + `setOpaque:NO`；
- 红绿灯定位封装在 `WindowButtonsProxy`：`initWithWindow:` → `setHeight:` → `setMargin:`，`hiddenInset` 默认 margin = **(12, 11)**，`trafficLightPosition` 选项可自定义；
- 重挂时机：`OnTitleChanged` → `redraw`；全屏 `NotifyWindowWillLeaveFullScreen` → `setVisible:NO`，`NotifyWindowLeaveFullScreen` → redraw；
- 源码注释明言：`SetStyleMask` 后「zoom button will revert to default, probably a bug of Cocoa or macOS」→ **任何 styleMask 变更后必须重挂**。

### 风险清单

| 风险 | 事实依据 | 对策 |
|---|---|---|
| 按钮间距被改坏 | 间距由系统管理；只平移 origin 不碰 size 即可保留 | 先读三钮现有 frame，整体平移同一向量，size 写死 |
| 垂直居中错位 | Big Sur 起钮径/间距随 macOS 版本变（12pt 钮径 8pt 间距） | 用 `[btn frame].size` 动态取，禁写死常量 |
| hover 行为 | 钮留在原 superview（标题栏容器层）时 hover 高亮/右键菜单全保；换 superview 才会丢 | 用 Electron 式原容器平移，不 reparent |
| 全屏切换位置丢 | Electron：will-leave-fullscreen 隐藏、leave-fullscreen redraw | 监听 `NSWindowWillEnterFullScreenNotification` / `NSWindowDidExitFullScreenNotification` 重挂 |
| zoom 钮回退默认位 | Electron 源码注释实锤（Cocoa bug） | 每次改 styleMask 后强制重挂一次 |
| `setTitle:` 重置钮位 | Tauri bug [#13044](https://github.com/tauri-apps/tauri/issues/13044) 实锤 | 包裹 `glfwSetWindowTitle`，设后重挂 |
| 窗口 resize 后错位 | 红绿灯锚定左侧不随宽动，但坐标换算依赖窗口几何时高度变化会漂（[Flutter 社区踩坑](https://www.reddit.com/r/flutterhelp/comments/1kgwq1m/macos_traffic_light_buttons_misalignment_how_can)） | `windowDidResize`（即 GLFW 帧回调路径）里重挂 |

---

## Q2 路线 B：完全自绘、隐藏红绿灯

### API 链

```objc
// 隐藏三钮（保留 Titled mask！不要去掉 Titled）
[[w standardWindowButton:NSWindowCloseButton]      setHidden:YES];
[[w standardWindowButton:NSWindowMiniaturizeButton] setHidden:YES];
[[w standardWindowButton:NSWindowZoomButton]        setHidden:YES];
// Electron kNormal 档等价做法：InternalSetWindowButtonVisibility(false)（私有，公开层就是逐钮 hidden）
// 参考文：Christian Tietze《Hide Traffic Light Buttons in NSWindow Without Removing》
// https://christiantietze.de/posts/2020/10/nswindow-hide-traffic-light-dark-mode

// 自绘钮动作 → 系统 API：
[w performClose:nil];        // 关闭，走 windowShouldClose: 代理，语义=点系统红钮
[w miniaturize:nil];         // 最小化（或 performMiniaturize:）
[w toggleFullScreen:nil];    // 全屏（绿灯语义）
```

### 风险

1. **必须保留 `Titled` + `Closable` + `Miniaturizable` mask 再藏钮**——若为「纯自绘」改走 `GLFW_DECORATED=false`（Borderless），立刻掉进 GLFW undecorated 全家桶（不可 resize，见 Q4/Q7）。
2. `performClose:` 是否能穿透 GLFW 的 close 回调链（GLFW 通过 `windowShouldClose:` 代理桥接 `glfwSetWindowCloseCallback`）——**待 POC**。
3. 键盘等价丢失：GLFW 默认无菜单栏，Cmd-W / Cmd-M 不通（kitty 是自己建了 NSMenu 才有的）。自绘钮需自管 accessibility 标签。
4. zoom 钮 hover 菜单（右键/长按出「进入全屏/移到侧边」）系统性丢失。
5. 拖拽、双击语义与路线 A 相同 workload（见 Q3/Q5），**路线 B 并不省事**——省掉的只有三颗钮的定位代码，换来的是语义修补。
6. Electron 用 hiddenInset（留钮）而非全藏，kNormal 全藏档在 Electron 里也是二级选项——**主流默认是留钮**。

---

## Q3 拖拽：标题栏可拖、内容区不可拖

### 标准做法：`performWindowDragWithEvent:`（= Swift `performDrag(with:)`）

[Apple 一手文档](https://developer.apple.com/documentation/appkit/nswindow/performdrag(with:).md)：

- **macOS 10.11+**；
- 参数 = 「The original mouse-down event received by the application or a view」——惯用 `[NSApp currentEvent]`；
- 「把拖动工作交给 Window Server，窗口可参与 space 切换等系统特性」；
- **「This method returns right away, and a mouse-up event may not get sent」**——调用立即返回，且 mouse-up 可能不送达。

### 三个候选 API 的分工（一手语义）

| API | 一手语义 | 适用 |
|---|---|---|
| `performDrag(with:)` | 视图收到 mouse-down 后主动交出拖动权 | **区域拖拽首选**（本场景） |
| `isMovableByWindowBackground` | 「movable by clicking and dragging **anywhere in its background**」，`NSTexturedBackgroundWindowMask` 窗口默认 YES | 整窗背景拖，**区域不可控**，需配下一行 |
| `mouseDownCanMoveWindow`（NSView） | 「determine the region by which a window can be moved」；**默认 opaque view=NO / 非透明=YES**；子类可按 event 重写 | 与上一行组合实现「只有覆盖层可拖」 |

来源：[isMovableByWindowBackground](https://developer.apple.com/documentation/appkit/nswindow/ismovablebywindowbackground) ｜ [mouseDownCanMoveWindow](https://developer.apple.com/documentation/appkit/nsview/mousedowncanmovewindow)

### GLFW 已消费鼠标事件怎么办：覆盖层 NSView 抢 hit-test

GLFW 的 ContentView 收 mouseDown 与拖拽不冲突，两条等价路径：

**路径 1（推荐，SO 30963700 / GLFW #902 社区模式）**——透明覆盖层自吞事件：

```objc
// 顶部 40px 条上放一个透明 NSView（frame 跟随窗口宽，高 40）
// GLFW 的 GL 内容视图在其下，这 40px 的 mouseDown 到不了 GLFW —— 正合需求
@implementation TitlebarDragView
- (void)mouseDown:(NSEvent *)event {
    [self.window performWindowDragWithEvent:[NSApp currentEvent]];
}
@end
```
来源：[Move an NSWindow by dragging an NSView (SO)](https://stackoverflow.com/questions/30963700/move-a-nswindow-by-dragging-a-nsview)；GLFW issue [#923](https://github.com/glfw/glfw/issues/923) 记载 Andos 在 #902 「hacked around OSX with performWindowDragWithEvent」。

**路径 2（AppKit 原生代劳）**：`window.isMovableByWindowBackground = YES` + 覆盖层 `mouseDownCanMoveWindow` 返回 YES、GLFW 内容视图返回 NO → AppKit 在 sendEvent 阶段直接进 Window Server 拖动，GLFW 全程看不到事件。需逐一确认各视图默认值（GLFW ContentView opaque 与否未查证 → **待 POC**）。

### 已知坑

1. **事件参数必须是真实 mouse-down**：传 nil 或过期事件会间歇失败——[Warp #16508](https://github.com/warpdotdev/warp/issues/16508)：`performWindowDragWithEvent:` 在 macOS 27.2 beta 上间歇失灵（有 A/B 数据）。用 `[NSApp currentEvent]` 是社区共识。
2. **mouse-up 不送达**（Apple 一手）→ GLFW 鼠标按下状态悬空：拖拽启动时手动 reset 输入状态（清 mouse button 状态），否则 EUI-NEO 的点击判定会卡「按住」。
3. **渲染冻结传闻**：[GLFW #2074](https://github.com/glfw/glfw/issues/2074) 与 [gamedev SE](https://gamedev.stackexchange.com/questions/137727/glfw-window-freezes-on-title-bar-hold-drag) 的冻结针对**原生标题条模态循环**阻塞 `glfwPollEvents`；`performDrag` 交给 Window Server 后立即返回，理论风险小，**实际表现待 POC**。
4. 调用必须在主线程 AppKit 事件上下文内（GLFW 事件回调天然在主线程，满足）。

---

## Q4 resize：边缘缩放

### 路线 A：什么都不用做

窗口仍是 `Titled|Resizable`，FSCV 只让内容视觉延伸，**窗口边缘命中仍由系统处理**。GLFW 的 `_glfwSetWindowResizableCocoa` 只增删 `NSWindowStyleMaskResizable` 位（一手源码），framebuffer_size/window size 回调走 `NSWindowDelegate windowDidResize`，与 styleMask 无关。且 DevDesk 现状（已证事实 2）就是在 FSCV 下正常 resize 运行的——**现网已实证**。

### 路线 B / undecorated：有已知缺口，可修但有代价

- [GLFW #1063（一手）](https://github.com/glfw/glfw/issues/1063)：macOS undecorated 时 GLFW **不给** `NSWindowStyleMaskResizable` → 用户不可 resize。[GLFW Window Guide](https://www.glfw.org/docs/latest/window_guide.html) 白纸黑字：「An undecorated window will not be resizable by the user」。
- Workaround（无需 fork，kitty 作者在 #1063 给出）：`glfwGetCocoaWindow()` 拿 NSWindow 后 `styleMask |= NSWindowStyleMaskResizable`，「simply adding the NSWindowStyleMaskResizable will give the borderless windows anchors (size-grips)」。
- 但 GLFW 全屏切换会重写 mask（见 Q7）→ 每次全屏退出后要重补。
- Borderless 不能成 key/main 的坑 GLFW 已处理（`GLFWWindow canBecomeKeyWindow` 返回 YES，一手源码）。

**结论：路线 A 在 resize 上零成本；路线 B 要补 mask + 全屏钩子。**

---

## Q5 语义保留：双击缩放/最小化、绿灯全屏

### 双击标题栏（缩放/最小化，跟随系统偏好）

- 系统偏好键：`AppleActionOnDoubleClick`（`"Maximize"` 默认 / `"Minimize"`）。
- **路线 A 下原生自动保留**：`Titled` mask + FSCV 时 NSTitlebarContainerView 仍在层叠中，标题条区域双击走 AppKit 原生路径。**一手反证**：Electron `native_window_mac.mm` 全文无任何双击处理代码，而 hiddenInset 窗口双击缩放正常 → AppKit 在 FSCV 下自动做。
- 例外：Q3 路径 1 的覆盖层会吞掉该区域 mouseDown，原生双击随之断掉 → 手动兜底：

```objc
- (void)mouseDown:(NSEvent *)event {
    if (event.clickCount >= 2) {
        NSString *action = [[NSUserDefaults standardUserDefaults]
            stringForKey:@"AppleActionOnDoubleClick"] ?: @"Maximize";
        if ([action isEqualToString:@"Minimize"]) { [self.window miniaturize:nil]; }
        else if (self.window.styleMask & NSWindowStyleMaskFullScreen) { /* 全屏中不 zoom */ }
        else { [self.window zoom:nil]; }
        return;
    }
    [self.window performWindowDragWithEvent:event];
}
```
- 代理可调：`windowShouldZoom:toFrame:` / `windowWillZoom:`（拦截/改 zoom 目标框）。
- 覆盖层方案是否真的吞掉原生双击、以及路径 2 是否保留原生双击——**待 POC**。

### 绿灯全屏

- 路线 A：zoom 钮原位保留即原生全屏；路线 B：自绘钮调 `toggleFullScreen:`。
- 全屏进出会动 styleMask 且重置红绿灯（Q1 风险表）→ 同一套通知钩子兜住。
- 进入全屏后自绘 40px 条应整体退场（系统标题条出现），`NSWindowWillEnterFullScreenNotification` 里停画。

---

## Q6 社区成熟做法（路线 A 佐证）

| 框架 | 方案 | 实现 | 取舍 | 证据级 |
|---|---|---|---|---|
| **Electron** | `titleBarStyle: 'hiddenInset'` + `trafficLightPosition: {x,y}` | 保留 `Titled` mask + FSCV 三连 + `WindowButtonsProxy` 移动钮（hiddenInset margin (12,11)）；全屏隐藏/恢复；styleMask 变更后 zoom 钮回退要重挂 | 依赖 macOS 版本 metrics；不处理双击（原生自动） | **一手源码精读** |
| **Tauri** | `titleBarStyle: "Overlay"` | tao/wry 同款 NSWindow 操作（`set_traffic_lights_inset` / raw `standardWindowButton` frame）；前端 `data-tauri-drag-region` 属性映射到底层 mouseDown 拖拽 | 已知坑 [#13044](https://github.com/tauri-apps/tauri/issues/13044)：`setTitle:` 重置钮位；社区讨论 [#7978](https://github.com/orgs/tauri-apps/discussions/7978)、[tauri-plugin-decorum](https://lib.rs/crates/tauri-plugin-decorum) 整合成熟做法 | 扫描（官方 v2 文档 + issue） |
| **Qt** | QWindowKit / FramelessHelper | naive `Qt::FramelessWindowHint` 在 macOS 破坏 resize/move（社区公认反模式）→ QWindowKit「**macOS native system button positioning**」+ `setHitTestVisible` 标记可交互控件 + hit-test 不可见区=可拖区；macOS 11+ | 纯自绘被两个头部库放弃，全部回归「原生窗口+FSCV+移钮」 | README 一手 + [Qt 论坛](https://forum.qt.io/topic/164518/custom-titlebar) 扫描 |

来源：[Electron native_window_mac.mm](https://github.com/electron/electron/blob/main/shell/browser/native_window_mac.mm)｜[Tauri v2 Window Customization](https://v2.tauri.app/learn/window-customization)｜[QWindowKit](https://github.com/stdware/qwindowkit)｜[FramelessHelper](https://github.com/wangwenx190/framelesshelper)

**判定：三家齐齐佐证路线 A，零反例。** 纯 Borderless 全自绘（路线 B 的极端形态）在 GLFW 世界只有 kitty 这类深度定制项目在做且要自己补 resize（#1063），无商业框架跟随。

---

## Q7 GLFW 集成风险

### undecorated 已知限制（一手源码 + 官方文档）

- `GLFW_DECORATED=false` → `NSWindowStyleMaskBorderless`，且**不带** `Resizable`（创建路径 `if (window->monitor || !window->decorated) styleMask |= NSWindowStyleMaskBorderless;`）→ 不可拖不可 resize 不可命中，`window_backend.cpp:1604-1612` 注释的理由逐条属实。
- 纯 Borderless 无法成 key/main —— GLFW 已在 `GLFWWindow canBecomeKeyWindow` 返回 YES 解决（一手）。

### styleMask 重写时机（一手，`src/cocoa_window.m` 精读）

GLFW **只在三处**写 styleMask，平时不会自发重置：

1. 窗口创建 `createNativeWindow`；
2. `glfwSetWindowAttrib` → `_glfwSetWindowDecoratedCocoa` / `_glfwSetWindowResizableCocoa`；
3. `glfwSetWindowMonitor` 全屏切换（进出都重写整套 Titled/Closable/Resizable/Borderless）。

→ EUI-NEO 后挂的 FSCV 等 mask **不会**被 GLFW 日常操作清掉；但走 `glfwSetWindowMonitor` 全屏就会。另外 AppKit 原生全屏（绿灯）进出由 AppKit 动 mask，退出后自挂 mask 是否自动恢复**待 POC**（Electron 选择无条件重挂，照抄即可）。

GLFW 全源码**从不**使用 `FullSizeContentView` / `titlebarAppearsTransparent` / `standardWindowButton`（一手）→ 无框架层干扰。

### glfwGetCocoaWindow

- Native access API，需 `#define GLFW_EXPOSE_NATIVE_COCOA` + `#include <GLFW/glfw3native.h>`；GLFW 3.3 引入（二手多源；官方 native guide 页已迁址未复核原文，不构成风险——**EUI-NEO `window_effect_macos.mm` 已在用**，即已证事实 2，当前集成实证可用）。

---

## 路线 A vs 路线 B 对比

| 维度 | 路线 A：保留红绿灯+定位进自绘条 | 路线 B：隐藏红绿灯+全自绘钮 |
|---|---|---|
| 布局几何统一（bodyTop=40、内容 y=0） | 达成 | 达成 |
| 钮视觉与 Windows 一致 | 不一致（Mac 原生钮，视觉仍 Mac） | 一致（自绘同款 46×40 热区） |
| resize | 系统，零成本 | 需保留 Titled 或补 Resizable+钩子 |
| 拖拽 | 覆盖层，两路线工作量相同 | 同左 |
| 双击缩放/最小化语义 | 原生自动（覆盖层吞事件则手动兜底） | 同左 |
| 全屏/绿灯语义 | 原生 | `toggleFullScreen:` + 全屏钩子 |
| 重挂钩子（全屏/title/styleMask） | 必需 | 必需 |
| hover 菜单/无障碍/键盘等价 | 原生保留 | 部分丢失需自管 |
| 社区佐证 | Electron/Tauri/Qt 三家 | 无主流框架 |
| 工作量 | 小（一个 ObjC++ 文件 + 3 通知钩子） | 中（A 的全部 + 钮交互 + 语义修补） |

## 推荐

**路线 A。** 理由：① 与 Electron/Tauri/QWindowKit 同构，每一步都有成熟实现可抄；② EUI-NEO 已完成一半（window_effect_macos.mm 三连），增量就是「平移三钮 + 拖拽覆盖层 + 重挂钩子」一个 ObjC++ 文件；③ resize/双击/全屏语义零成本白拿；④「几何一致」的目标（bodyTop=40）路线 A 即达成——Windows/Mac 钮视觉差异是「平台原生感」，恰是 macOS 用户预期，与布局漂移问题无关。路线 B 仅当「三钮像素级复刻 Windows」成为硬需求时再评估。

## 遗留（全部 POC 级待验证，不阻塞路线决策）

1. 红绿灯 superview 坐标换算与各 macOS 版本钮径 metrics——POC 实测，禁写死常量。
2. 覆盖层吞 mouseDown 后原生双击缩放是否失效（决定要不要手动兜底分支）。
3. AppKit 原生全屏退出后自挂 FSCV mask 是否自动恢复（决定重挂钩子挂几处）。
4. 路线 B 备查：`performClose:` 能否触发 GLFW close 回调链；Borderless+Resizable 边缘 anchor 手感。
5. kitty 具体实现文件两轮检索未命中（cocoa_window.m/glinit.c 均非目标文件）——不影响结论，Electron/QWindowKit 佐证已足。
6. `[NSApp currentEvent]` 在 macOS 27.x 的间歇失败（Warp #16508）——实装后留意。

## 来源清单

**精读（一手，正文提取）**
1. Electron `shell/browser/native_window_mac.mm`（main 分支 raw）——trafficLightPosition/FSCV/全屏/zoom 回退
2. GLFW `src/cocoa_window.m`（master raw）——styleMask 三处重写、canBecomeKeyWindow
3. [GLFW #1063](https://github.com/glfw/glfw/issues/1063)——undecorated 无 Resizable、kitty runtime 补 mask
4. [GLFW #923](https://github.com/glfw/glfw/issues/923)——拖拽区 feature request 状态（open，已移出 3.4）、#902 workaround
5. [Apple performDrag(with:)](https://developer.apple.com/documentation/appkit/nswindow/performdrag(with:))（.md 版）——10.11+、事件参数、立即返回
6. [Apple isMovableByWindowBackground](https://developer.apple.com/documentation/appkit/nswindow/ismovablebywindowbackground)（.md 版）
7. [Apple mouseDownCanMoveWindow](https://developer.apple.com/documentation/appkit/nsview/mousedowncanmovewindow)（.md 版）——默认值语义
8. [QWindowKit README](https://github.com/stdware/qwindowkit)——macOS 原生钮定位、hit-test 模型、macOS 11+

**扫描（搜索摘要/二手，未逐字精读）**
- [steve228uk gist](https://gist.github.com/steve228uk/c960b4880480c6ed186d)、[SO 30085631](https://stackoverflow.com/questions/30085631/nswindow-change-positions-of-window-buttons)、[Christian Tietze](https://christiantietze.de/posts/2020/10/nswindow-hide-traffic-light-dark-mode)、[Innei](https://innei.in/en/posts/tinkering/macos-zoom-button-hover-menu)、[Reddit flutterhelp](https://www.reddit.com/r/flutterhelp/comments/1kgwq1m/macos_traffic_light_buttons_misalignment_how_can)、[Name That UI](https://namethatui.com/macos/traffic-lights)
- [Tauri v2 Window Customization](https://v2.tauri.app/learn/window-customization)、[Tauri #13044](https://github.com/tauri-apps/tauri/issues/13044)、[Discussion #7978](https://github.com/orgs/tauri-apps/discussions/7978)、[tauri-plugin-decorum](https://lib.rs/crates/tauri-plugin-decorum)、[SO 79600424](https://stackoverflow.com/questions/79600424/how-to-change-window-controls-position-on-macos)
- [SO 30963700](https://stackoverflow.com/questions/30963700/move-a-nswindow-by-dragging-a-nsview)、[GLFW #2074](https://github.com/glfw/glfw/issues/2074)、[gamedev SE 137727](https://gamedev.stackexchange.com/questions/137727/glfw-window-freezes-on-title-bar-hold-drag)、[GLFW Discourse 2392](https://discourse.glfw.org/t/making-a-custom-titlebar/2392)、[2344](https://discourse.glfw.org/t/support-for-undecorated-resizeable-window/2344)、[Warp #16508](https://github.com/warpdotdev/warp/issues/16508)
- [GLFW Window Guide](https://www.glfw.org/docs/latest/window_guide.html)（undecorated not resizable 原句）、[Qt 论坛 164518](https://forum.qt.io/topic/164518/custom-titlebar)、[FramelessHelper](https://github.com/wangwenx190/framelesshelper)、[Cocoa with Love](https://www.cocoawithlove.com/2008/12/drawing-custom-window-on-mac-os-x.html)
- glfwGetCocoaWindow 3.3+ / GLFW_EXPOSE_NATIVE_COCOA：二手多源（官方 native guide 页已 404 未复核；EUI-NEO 已在用，实证无虞）
