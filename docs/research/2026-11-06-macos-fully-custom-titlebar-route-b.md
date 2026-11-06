# macOS 路线 B 实现路径调研：隐藏红绿灯 + 全自绘窗口钮（need:140）

> 课题：老板已拍板路线 B（隐藏原生红绿灯、全自绘窗口钮），本报告只补**实现路径**，不讨论 A/B 取舍（路线 A 成熟度见 `docs/research/2026-11-05-macos-custom-titlebar.md`）。
> 调研人：researcher ｜ 日期：2026-11-06 ｜ 工具调用：36 次（预算 60）｜ 构建：0 次
> 池线索：继承 clue 37/42/43/35（不重查）；新查证已入库 **clue 57/58/59**。

## 结论先行

1. **路线 B 可行，但两个子方案必选「保留 `Titled|Resizable` + `setHidden:YES` 隐藏三钮」（子方案①），不选真 Borderless（子方案②）**。子方案① = Electron/notes 同构、resize/圆角/阴影/双击/全屏全部系统托管零成本；子方案② 每一项都要自担且 GL 不透明层与系统圆角的配合成本显著。
2. **隐藏方式必须用 `setHidden:YES`，禁用 `removeFromSuperview`**：Apple 一手语义 `standardWindowButton:` 返回「in the window's view hierarchy」的按钮，remove 后返回 nil，且 `performClose:`/`performMiniaturize:` 在「窗口没有该按钮」时会响系统蜂鸣——隐藏保留语义，移除破坏语义。
3. **真 Borderless 也能原生 resize（不需自写 8 向）**：kitty 生产实装（kitty@4fdb55e）证明后补 `NSWindowStyleMaskResizable` 即得原生边缘 anchor；但这只解决 resize 一项，圆角/阴影/双击/拖拽仍要自担——子方案②的收益远小于代价。
4. **自绘钮首选 NSView 层叠（透明 NSButton/NSView 覆盖层）而非纯 EUI 画**：前者免费拿到 hit-test、hover、无障碍标签；EUI 画则这些全部自建。点击回传链已全部锚定（见 Q3，GLFW 桥接 file:line 齐全）。
5. **最大坑与路线 A 相同且更多一条**：全屏进出/`setTitle:`/任何 styleMask 变更都会重置钮状态，且 **Tietze 实证改 `.resizable` 会自动重新显示已隐藏的 traffic lights**——统一重挂钩子里必须含「重新 `setHidden:YES`」。

---

## Q1 隐藏红绿灯：`setHidden:` vs `removeFromSuperview`

### API 链

```objc
NSWindow *w = glfwGetCocoaWindow(glfwWindow);
[[w standardWindowButton:NSWindowCloseButton]      setHidden:YES];
[[w standardWindowButton:NSWindowMiniaturizeButton] setHidden:YES];
[[w standardWindowButton:NSWindowZoomButton]        setHidden:YES];
```

### 两法差异（一手依据）

| 维度 | `setHidden:YES` | `removeFromSuperview` |
|---|---|---|
| `standardWindowButton:` 后续返回 | 有效引用（按钮仍在视图层级，Apple 原文：「Window button **in the window's view hierarchy** … nil when such button is not in the window's view hierarchy」） | **nil**（不在层级了） |
| `performClose:` / `performMiniaturize:` | 正常（Apple 原文：只有「doesn't have a close/minimize button」才 `emits the alert sound`；hidden 按钮仍在层级=窗口「有」按钮） | **蜂鸣 + 功能断**（按钮不在层级=「没有」按钮；AppKit 内部判定路径待 POC 复核，文档语义推理） |
| 恢复显示 | `setHidden:NO` 一行 | 需自存引用 + `addSubview:`（丢官方重排，换容器丢 hover 菜单——上一报告 Innei 分析） |
| 风格联动（dark mode 等） | AppKit 仍管理按钮，安全 | 脱管 |

来源：Apple `standardWindowButton(_:)` / `performClose(_:)` / `performMiniaturize(_:)`（.md 一手，见来源清单 1-3）。

### 副作用逐项

- **Cmd+W / Cmd+M**：与钮可见性**无关**。`performClose:`/`performMiniaturize:` 是 NSWindow 方法，走 responder chain / 菜单路由，不经过按钮视图。但 GLFW 应用默认无菜单栏（kitty 自建 NSMenu 才有 Cmd+W，前轮已证）→ DevDesk 现状 Cmd+W/M 本来就不通，隐藏钮不使其变好也不变坏；要快捷键需自建 NSMenu（独立工作项）。
- **窗口仍可关闭/最小化/缩放**：是。三钮 hidden 后窗口 mask（`Titled|Closable|Miniaturizable|Resizable`，vendored `EUI-NEO/3rd/glfw/src/cocoa_window.m:813-822`）不变，程序语义全保留。
- **菜单「最小化」项状态**：由 `Miniaturizable` mask 与 `isMiniaturizable` 决定，不随按钮 hidden 变化 → 子方案①下自建菜单的该项正常 enable。DevDesk 当前无菜单栏，暂不适用。
- **hover 菜单/长按菜单**：按钮 hidden 后不可交互，其 hover 菜单自然不可达（视觉上本来也要自绘替代）。
- **重显陷阱（新实锤）**：Tietze（2020）实证「Making the window resizable via `.resizable` **will show the traffic lights automatically**, so each button is then hidden manually」→ 任何 styleMask 变更后必须重新 `setHidden:YES`（Electron「zoom 钮回退」注释的姊妹坑）。

---

## Q2 两子方案对比（老板指定必答）

**① 保留 `NSWindowStyleMaskTitled|Closable|Miniaturizable|Resizable`（GLFW decorated 默认）+ `setHidden:YES` 三钮 + 自绘钮**
**② 真 `GLFW_DECORATED=FALSE`（`NSWindowStyleMaskBorderless`，vendored 源码 :1285-1302 实证移除 Titled|Closable|Resizable、保留 Miniaturizable）**

| 维度 | ① Titled + 隐藏三钮 | ② 真 Borderless |
|---|---|---|
| **几何可控性** | FSCV 下内容 y=0 起（DevDesk 现网已在跑），`bodyTop` 直接取 40 与 Windows 对齐 | 完全自控、无 NSTitlebar 视图层叠，几何 100% 归自己 |
| **resize** | **系统原生，零成本**（DevDesk 现网实证） | **可修**：后补 Resizable mask 得原生 anchor（kitty@4fdb55e 生产实证，#1063）；但 mask 会被 GLFW 全屏切换重写（:1285-1302）→ 需 `NSWindowDidExitFullScreenNotification` 重补；Godot #79329 是反面教材（框架移除 mask → 用户长期不可 resize） |
| **全屏行为** | 原生动画/语义；自绘条在 `WillEnterFullScreen` 退场、`DidExitFullScreen` 重挂（Electron 同款时序） | `toggleFullScreen:` 仍可进原生全屏，但进出都重写 mask → 重挂钩子覆盖面更大；kitty 在用 |
| **窗口阴影** | 系统托管 | 形状按不透明内容矩形（直角阴影）；GLFW 全屏切换后会重设 `setHasShadow:YES`（源码在册），阴影存在但跟随矩形 |
| **圆角** | **系统托管**（Titled 窗口原生圆角） | **丢**；补法 = `backgroundColor clear + isOpaque NO + wantsLayer + layer.cornerRadius + invalidateShadow()`（Apple 一手 invalidateShadow 语义 + lukakerr 指南），**但 GL surface 不透明，圆角裁剪要 EUI 渲染层配合，成本显著** |
| **拖拽** | 覆盖层 `performWindowDragWithEvent:`（两方案工作量相同） | 同左，且无任何原生拖拽兜底 |
| **双击标题栏** | 原生自动（FSCV 下 AppKit 原生行为，前轮 clue 42 已证；覆盖层吞事件则手动兜底） | **无原生**，必须手动读 `AppleActionOnDoubleClick` 自实现 |
| **performClose:/performMiniaturize: 语义** | 保留（按钮在层级） | **蜂鸣**（standardWindowButton 返回 nil → 「没有按钮」）；最小化要走 `miniaturize:`（只查 mask）而非 `performMiniaturize:` |
| **小众 bug 面** | Electron/Tauri/notes 全在这条路上，坑都有现成答案 | kitty 单打独斗，坑要自己趟（如 GLFW 清 Titled 重置 title 属性的 HACK，源码 :1362） |

**判定：选①。** ②的每一行「可修」都对应一份持续维护成本，唯一实质收益（三钮视觉彻底消失）①同样达成。kitty 证明②可行但它是多年深度定制项目；notes/Electron 证明①是「产品级」路线。

---

## Q3 自绘钮实现与点击回传

### 实现层选择：NSView 层叠 > 纯 EUI 画

| 方案 | 优点 | 缺点 |
|---|---|---|
| **NSView 层叠（推荐）**：40px 条区域叠透明 NSButton/NSView（自定义绘制外观），盖在 GLFW 内容视图上 | hit-test 免费、hover/高亮免费、`accessibilityLabel`/`NSAccessibilityButtonRole` 免费（VoiceOver 可达）、事件天然不进 GLFW | 需管理 z 序与全屏退场 |
| EUI 层画 + EUI 命中 | 与 Windows 端自绘钮代码同构 | 鼠标事件在 GLFW 消费路径上做「点击在钮区→不走 EUI 控件→回调原生动作」的分叉；无障碍零基础；hover 自建 |

生产先例走 NSView 层叠：notes `framelesswindow.mm` 用 `ButtonPasser` target/action 覆盖三钮 action（`[zoomButton setTarget:passer]`）——它甚至保留原生钮只换 action；我们 hidden 后用自建 NSButton 同理。

### 点击回传链（全部 file:line 锚定，vendored `EUI-NEO/3rd/glfw/src/cocoa_window.m`）

| 自绘钮 | 推荐调用 | GLFW 桥接证据 |
|---|---|---|
| 关闭 | `[w performClose:nil]`（走 `windowShouldClose:` 代理 = GLFW close 回调链，**可 veto**；等价点红钮） | `performClose:` 一手文档：触发 windowShouldClose → close；GLFW close 回调桥接 windowShouldClose（前轮 clue 43 链路） |
| 关闭（备选） | `glfwSetWindowShouldClose(win, 1)` | 设标志强制关闭，**不经 windowShouldClose 回调，不可 veto**——除非明确要「强制关」语义才用 |
| 最小化 | `glfwIconifyWindow(win)` | `_glfwIconifyWindowCocoa` → `[window miniaturize:]`（:1184-1189） |
| 恢复 | `glfwRestoreWindow(win)` | → `deminiaturize:` / `zoom:`（:1191-1199） |
| 缩放/最大化 | `glfwMaximizeWindow(win)` | `_glfwMaximizeWindowCocoa` → `[window zoom:]` 带 `isZoomed` 守卫（:1201-1207） |
| 全屏（绿灯语义） | `[w toggleFullScreen:nil]` | Apple 原生绿灯等价物；notes 生产实证（ButtonPasser → toggleFullScreen） |

注意：`zoom:`（glfwMaximizeWindow）与 `toggleFullScreen:` 是两种语义——Windows 绿钮=最大化 zoom，macOS 绿灯=全屏。路线 B 钮视觉抄 Windows 但语义应抄 macOS（绿灯=全屏），否则 mac 用户预期错位。**需产品确认**，默认建议 mac 全屏语义。

---

## Q4 拖拽移动

- **`performWindowDragWithEvent:` 为标准答案**（一手：10.11+、入参=原始 mouse-down、立即返回、mouse-up 可能不送达——前轮 clue 43/上一报告 Q3 已锚定）。
- gh code search `performWindowDragWithEvent` 25 命中：Zed、ollama、zen-browser、Avalonia、Ladybird、Wails v2/v3、iTerm2、libui、openclaw、magpie——**9+ 生产项目统一此派**。
- 手动 `setFrameOrigin:` 派也存在（libui 兜底路径、notes Qt 层 `move()`）：libui winmoveresize.m 展示了完整兜底写法（事件跟踪循环 + 每次转屏幕坐标 + 初始 frame 加偏移）。仅当 `performDrag` 在目标 macOS 版本失效（Warp #16508 报过 27.2 beta 间歇失灵）时作为 plan B。
- EUI 命中区接法：顶部 40px 透明覆盖层 NSView（frame=窗口宽×40，跟随窗口宽），`mouseDown:` 里调 `performWindowDragWithEvent:[NSApp currentEvent]`；`clickCount>=2` 时按 `AppleActionOnDoubleClick` 分支 zoom/miniaturize（上一报告 Q5 已给代码骨架）。覆盖层在钮位子区放三个自绘 NSButton，命中顺序=按钮>拖拽区。

---

## Q5 边缘 resize

- **子方案①**：原生（`Resizable` mask 在），零成本，现网已实证运行。
- **子方案② 的三条路**（按成本升序）：
  1. **后补 mask（kitty 路线，推荐若走②）**：创建后 `styleMask |= NSWindowStyleMaskResizable` → AppKit 给原生 8 向 anchor/size-grips（#1063 asmvik 原话「simply adding the NSWindowStyleMaskResizable will give the borderless windows anchors (size-grips)」；kitty@4fdb55e 生产在跑，2017 年至今无过期反证）。代价：GLFW 全屏切换重写 mask（:1285-1302）→ 全屏退出重补。
  2. **自实现 8 向（libui 路线）**：hit-test 边缘区（外部 resetCursorRects 设光标形态）→ `winmoveresize.m` 事件跟踪循环 + `setFrame:` + min/max clamp。libui 注释自曝已知 bug（对边同时缩放）；min/max 用 Auto Layout `fittingSize` 测量。成本高，仅 mask 方案失灵时启用。
  3. 混合：mask 优先、失灵降级手动（libui 内部就是这个顺序：`performWindowDragWithEvent` 优先、手动兜底）。
- 光标形态：自实现时才需要（`resetCursorRects` 返回 `NSCursor.resizeLeftRightCursor` 等四角/四边光标）；mask 方案系统自动。

---

## Q6 全屏

- `toggleFullScreen:` 进出都会动 styleMask：GLFW 侧 `glfwSetWindowMonitor` 重写整套 mask（前轮 clue 43），AppKit 原生全屏进出也由 AppKit 动 mask → **统一重挂钩子挂在 `NSWindowWillEnterFullScreenNotification`（退场自绘条）+ `NSWindowDidExitFullScreenNotification`（重挂 FSCV/隐藏钮/mask 补丁）**（clue 43 已证「后挂 mask 会被改写，需 DidExitFullScreen 重挂」，Electron 无条件重挂模式照抄）。
- 子方案①全屏中：自绘 40px 条与三钮整体退场（系统全屏无标题栏），退出后重挂。
- 子方案②全屏中：同上，另加「退出全屏后 Borderless|Resizable 重写」（kitty 同款）。

---

## Q7 无障碍 / 键盘

- **VoiceOver**：`setHidden:` 的钮从辅助功能树消失（hidden 视图不可达）→ 窗口钮语义对读屏用户丢失。补法：NSView 层叠自绘钮设置 `accessibilityLabel="关闭"` + `accessibilityRole=NSAccessibilityButtonRole`（免费拿到）；纯 EUI 画则**零无障碍**（GL 内容不在 a11y 树）。⚠ 实测验证待 POC（Apple 对 hidden 视图的 a11y 排除是通用语义，本报告未单独取一手 a11y 文档，证据等级=推测）。
- **键盘**：隐藏钮不影响任何快捷键（responder chain 不经过按钮视图）；但 GLFW 无菜单栏 → Cmd+W/Cmd+M/Ctrl+Cmd+F 本来不通。若产品要求：自建 NSMenu（File>Close=performClose:、Window>Minimize=performMiniaturize:、Enter Full Screen=toggleFullScreen:）→ kitty 先例。独立工作项，不阻塞路线 B。

---

## Q8 阴影 / 圆角（子方案②专项）

- 阴影：Borderless **不自动丢**（GLFW 创建后 `setHasShadow:YES` 且全屏切换后重设，源码在册），但形状跟随窗口矩形。
- 圆角：**丢系统圆角**（系统圆角是 Titled 窗口 frame 的一部分）。补法（lukakerr 全公开 API 指南 + Apple `invalidateShadow()` 一手「recomputed based on the current window shape」）：
  ```objc
  w.backgroundColor = [NSColor clearColor]; w.opaque = NO;
  cv.wantsLayer = YES; cv.layer.cornerRadius = 10; cv.layer.masksToBounds = YES;
  [w invalidateShadow];   // 形状变化后重算阴影
  ```
- **关键成本**：GLFW 的 OpenGL surface 不透明 → `masksToBounds` 圆角裁剪要求 EUI 渲染侧同步（清屏透明/视口让角），渲染管线的透明合成改动不小。子方案①无此项。

---

## 真实先例清单

| 项目 | 走法 | 证据 |
|---|---|---|
| **kitty**（GLFW 终端，与本项目最同构） | ② Borderless+后补 Resizable | GLFW #1063 内 kitty 作者自述 + kitty@4fdb55e；生产多年 |
| **nuttyartist/notes** | ① Titled+FSCV+隐藏/移钮 + ButtonPasser | `src/framelesswindow.mm`（raw 精读） |
| **Electron** | ①（hiddenInset 留钮为主，kNormal 全藏档=逐钮 hidden） | 前轮 clue 42 源码精读 |
| **libui** | 手动 move/resize 兜底库 | `darwin/winmoveresize.m`（raw 精读） |
| **Godot** | ② 反面教材 | #79329 open：框架移除 Resizable → 不可 resize |
| **Zed / ollama / zen / Avalonia / Ladybird / Wails / iTerm2 / openclaw / magpie** | 拖拽统一 `performWindowDragWithEvent` | gh code search 25 命中（路径见来源 8） |

**结论：路线 B（隐藏钮形态）有真实生产先例（Electron kNormal、notes、Tietze 应用）；真 Borderless 全自绘有 kitty 单例支撑。无「GLFW+Borderless+全自绘钮」的框架级先例——符合上一报告「无主流框架走纯 Borderless」判定，此处不重复论证。**

---

## 推荐

**子方案①：保留 GLFW decorated（`Titled|Closable|Miniaturizable|Resizable`）+ 三钮 `setHidden:YES` + NSView 层叠自绘钮 + 覆盖层拖拽 + `toggleFullScreen:` + 统一重挂钩子（含重新 setHidden）。**

理由：① resize/圆角/阴影/双击/全屏动画全部系统白拿，DevDesk 现网已在该形态下运行（增量最小）；② 每一步都有生产级参考（Electron/notes）；③ `performClose:`/`performMiniaturize:` 语义完整保留；④ 子方案②省下的只有「三钮实体」却背上 GL 透明圆角、手动双击、mask 重补三座山。

## 三风险

1. **重挂/重隐时机**：全屏进出、`setTitle:`（Tauri #13044）、任何 styleMask 变更都会重置钮位/可见性，且 Tietze 实证改 `.resizable` 会自动重显钮 → 所有钩子必须含「重新 `setHidden:YES`」，漏一处即白钮浮现在自绘条上。
2. **`performClose:` 穿透 GLFW 回调链待 POC**（上一报告遗留项）：按钮 hidden 不影响语义（一手文档），但 windowShouldClose→glfwSetWindowCloseCallback 的 veto 路径需实测；失败则降级 `glfwSetWindowShouldClose`（牺牲 veto）。
3. **无障碍/快捷键缺口**：hidden 钮从 a11y 树消失、GLFW 无菜单栏 Cmd+W/M 不通——NSView 自绘钮补标签可解一半；菜单是独立工作项，不做则 mac 键盘用户少语义（VoiceOver 影响等级=推测，待 POC）。

## 遗留（全部 POC 级）

1. `performClose:` → GLFW close 回调链实测（含 veto）。
2. hidden 钮在 VoiceOver 树的实际表现 + 自绘 NSButton a11y 标签实测。
3. 菜单「最小化」项 validation 在钮 hidden 下的表现（DevDesk 无菜单栏，低优先）。
4. `performClose:`/`performMiniaturize:` 在 removeFromSuperview 后是否真蜂鸣（AppKit 内部判定路径，文档语义推理未实测——本方案不采用 remove，仅备案）。
5. 全屏退出后 `setHidden` 重挂的具体时序（WillEnter 还是 DidExit，Electron 用 will-leave 隐藏 + leave 重挂，照抄后实测）。

## 来源清单

**精读（一手，正文提取）**
1. [Apple `standardWindowButton(_:)`](https://developer.apple.com/documentation/appkit/nswindow/standardwindowbutton(_:).md) — 「in the window's view hierarchy … nil when not」
2. [Apple `performClose(_:)`](https://developer.apple.com/documentation/appkit/nswindow/performclose(_:).md) — 模拟点钮、windowShouldClose 链、无按钮时 alert sound
3. [Apple `performMiniaturize(_:)`](https://developer.apple.com/documentation/appkit/nswindow/performminiaturize(_:).md) — 无 minimize 按钮时 alert sound
4. [Apple `invalidateShadow()`](https://developer.apple.com/documentation/appkit/nswindow/invalidateshadow().md) — 阴影按当前形状重算
5. [GLFW #1063](https://github.com/glfw/glfw/issues/1063) — asmvik size-grips 原话、kitty@4fdb55e、2017 closed 无过期反证
6. vendored `EUI-NEO/3rd/glfw/src/cocoa_window.m:813-822`（创建 mask 含 Miniaturizable）、`:1184-1207`（iconify/maximize/restore 桥接）、`:1285-1302`（decorated 切换 mask 重写）、`:1362`（清 Titled 的 title 副作用 HACK）
7. [libui `darwin/winmoveresize.m`](https://raw.githubusercontent.com/andlabs/libui/master/darwin/winmoveresize.m) — 手动 move/resize 完整兜底实现
8. [notes `src/framelesswindow.mm`](https://raw.githubusercontent.com/nuttyartist/notes/master/src/framelesswindow.mm) — Titled+FSCV+ButtonPasser+toggleFullScreen 生产实现

**扫描（搜索摘要/未逐字精读）**
- [Tietze《Hide Traffic Light Buttons in NSWindow Without Removing》](https://christiantietze.de/posts/2020/10/nswindow-hide-traffic-light-dark-mode/)（WebFetch 精读摘要，resizable 重显实锤）
- [lukakerr《NSWindow Styles》](https://lukakerr.github.io/swift/nswindow-styles)（圆角实践）、[SO 4593661 Borderless rounded corners](https://stackoverflow.com/questions/4593661/borderless-nswindow-with-rounded-corners)（403 未读，方案经搜索摘要交叉）
- [Godot #79329](https://github.com/godotengine/godot/issues/79329)、[GLFW #2266](https://github.com/glfw/glfw/issues/2266)
- gh search code `performWindowDragWithEvent`（Zed `crates/gpui_macos/src/window.rs`、Wails `v3/pkg/application/webview_notch_window_darwin.m`、iTerm2 `iTermStoplightHotbox.m` 等）
