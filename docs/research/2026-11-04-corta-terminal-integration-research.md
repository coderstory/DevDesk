# Corta 终端核心引入 DevDesk 集成可行性调研

- 日期：2026-11-04
- 调研人：devflow:researcher
- 上游：https://github.com/noah-qin/Corta（clone HEAD `4af5623`，2026-10-29，clone 于 `/tmp/corta-research/Corta`）
- 线索回流：pool clue domain=corta #1–#7（need 122）
- 标注约定：[一手源码 file:line] / [实测]（本机 Swift 6.4.2 / Xcode 27.2 实跑） / [检索]（社区/官方文档）/ [推断]

## 结论先行

1. **架构比预想更友好：PTY 在核心包内、渲染完全在应用层。** `CortaTerminal` 独立 SwiftPM 包零 UI 依赖（imports 仅 Foundation/Synchronization/Darwin/Dispatch/Compression/CortaPOSIX），自带 PTY spawn/读写/读线程；Metal 渲染器全部在应用仓 `Corta/Renderer/`，核心包一行 Metal 都没有。[一手源码 + 实测 import 扫描]
2. **Swift→C++ 互操作路线实跑可行**：`swift build -Xswiftc -cxx-interoperability-mode=default` 下核心包构建通过（3.78s），生成的头把 `TerminalSession`（RefCountedClass 语义）与 `Grid` 以完整 C++ class 暴露。ObjC 桥路线不可行——核心无任何公开 @objc 面，需自写 shim，价值低于直接 C++ interop。[实测 POC + 一手源码]
3. **渲染归属是本集成最大工作量决策点**：macOS 原生路径 = 往 EUI-NEO 的 GLFW 窗口嵌 NSView（无受支持 API，只有窗口材质的 AppKit 手术先例）；EUI-NEO 自绘路径 = 框架缺批量文本绘制原语，属框架级新能力。两路都不可回避 EUI-NEO 侧工作。[一手源码 + EUI docs]
4. **版本无冲突**：DevDesk 实际 minos=27.0（CMake 未设 deployment target，SDK 默认）≥ Corta 要求的 26.0；且核心源码零 `@available(macOS 26)` 标注，26.0 下限纯属包声明+Swift 6.2 工具链，不是 API 硬依赖。[实测 otool/CMakeCache]
5. **部署硬细节**：核心 spawn 依赖 `corta-exec` 助手可执行文件（posix_spawn 补 TIOCSCTTY 用），必须随 DevDesk.app 一并打包；SwiftPM automatic library 只产 `.o`+`.swiftmodule` 不产 `.a`，推荐 DevDesk 自建桥接包 path-depend 上游、由 `swift build` 旗标传导开启 interop，不改上游一个字节。[一手源码 Spawn.swift:20-21 + 实测]

---

## Q1 CortaTerminal 核心 API 形态

### 包管理形态
- 独立 SwiftPM 包 `CortaTerminal/Package.swift`（swift-tools-version 6.2，`platforms: [.macOS("26.0")]`）。仓库根是 Xcode 工程，根无 Package.swift。[一手源码 Package.swift:1,34-36]
- products：库 `CortaTerminal`（target 依赖 `CortaPOSIX`）、`CortaSFTP`（兄弟库，终端不需要）、`CortaPOSIX`；另有 7 个可执行工具（corta-dump / corta-bench / **corta-exec** / corta-image-decoder / corta-fuzz / corta-license / corta-release-check）。[一手源码 Package.swift:37-48]
- 包级 `.defaultIsolation(nil)`：parser/grid/PTY reader 跑非主线程，注释明言这是包存在的理由（Xcode 工程默认 MainActor，核心刻意不）。[一手源码 Package.swift:20-32]

### 对外入口类型与最小用法
入口 = `public final class TerminalSession: @unchecked Sendable`。[一手源码 TerminalSession.swift:54]

最小用法（自 shell 启动）：
```swift
let session = try TerminalSession(executable: "/bin/zsh", size: TerminalSize(rows: 24, columns: 80))
session.onOutput = { /* 读线程回调：触发重绘 */ }
session.onChildExit = { exit in /* … */ }
session.start()                    // 启动专用读线程
let grid: Grid = session.snapshot() // 每帧可调；锁下取 Grid 值拷贝（O(1) COW）
session.write([UInt8]("ls\r\n".utf8)) // 键入字节 → PTY
session.resize(to: size)           // 先 reflow 后 TIOCSWINSZ，串行队列
session.stop()
```
[一手源码 TerminalSession.swift:265-308, 340-343, 435-443, 471-473, TerminalSession+Writing.swift:37]

**feed 模型**：PTY 字节 → 专用 Thread（非 Task，QoS userInitiated）读循环 → 批上限 ~1MB / 切片 16KB → Parser → Grid 状态。UI 侧是**回调 + 快照拉取**，不是委托/推送数据：`onOutput` 只是无参唤醒信号，数据靠 `snapshot()` 取 `Grid` 值拷贝。[一手源码 TerminalSession.swift:23-53, 594-604]

### 类型边界
| 层 | 类型（均在 `Sources/CortaTerminal/`） | 说明 |
|---|---|---|
| 解析 | `Parser.swift` / `ParserPerformer.swift` / `Performer*.swift`（10 个切片：Cursor/Editing/Erase/Modes/OSC/Query/Report/Kitty*/ITermImages/ShellIntegration） | ANSI/OSC/kitty 图形协议处理 |
| UTF-8 | `UTF8Decoder.swift` / `GraphemeTable.swift` / `CharacterWidth.swift` / `AmbiguousWidth.swift` | 含 ZWJ 簇与宽字符 |
| 网格 | `Grid.swift` + 7 个扩展（Cursor/Editing/Marks/Reflow/Screens/SideTables/Text/Writing）、`Cell.swift` / `Line.swift` / `Pen.swift` / `ScreenLines.swift` | Swift 值类型 struct，快照即值拷贝 |
| 回滚 | `Scrollback.swift` / `ScrollbackCoordinates.swift` + 后台压缩队列 | 有限回滚+压缩 |
| PTY | `PTY.swift` / `PTYError.swift` / `Spawn.swift` + `CortaPOSIX`（GuardedDescriptor/ProcessExit/Spawning） | **在核心内** |
| 杂项 | Selection / Search / KeyboardProtocol / KittyGraphics* / ImagePlacementTable / CommandRecord / HistorySuggestion / DirectoryCompletion 等 | 选区/搜索/键协议/图像/历史补全都在核心 |

### PTY 归属（与简报假设相悖，已实证）
**PTY I/O 在核心包内**：`TerminalSession.pty: PTY`（TerminalSession.swift:65），init 内 `PTY.spawn(...)`（:284）。spawn 做法是 `posix_spawn` 助手进程 **corta-exec**（独立 Mach-O，因 posix_spawn 无法表达 TIOCSCTTY），助手再 `execve` 到 shell。[一手源码 Spawn.swift:20-21、Package.swift:73-80]
**但核心可脱离真实 PTY 驱动**：`Seams.readerSource / writerSink` 可注入替代 PTY 读/写（测试用途，`TerminalSessionLockWaitTests` 等在用），即字节流驱动核心是官方支持的姿势——对桥接层/Windows 适配都是关键接口。[一手源码 TerminalSession.swift:93-110]

## Q2 Swift↔C++ 互操作可行路线

### a) Swift 6 C++ interop【实测 POC 通过】
- `swift build --target CortaTerminal -Xswiftc -cxx-interoperability-mode=default` **构建成功（3.78s，本机 Swift 6.4.2 / macOS 27.2 SDK）**。
- 生成头 `CortaTerminal-Swift.h` 暴露面（实测 grep）：
  - `Grid`：完整 C++ class（`:8008`，`SWIFT_SYMBOL("s:13CortaTerminal4GridV")`，含 inline thunk、`Grid::init(rows, columns, scrollbackLimit)`）；
  - `TerminalSession`：完整 C++ class（`:15247`，继承 `swift::_impl::RefCountedClass`，引用计数语义）；
  - 另有嵌套 struct（IOFailure 等）。
- `-cxx-interoperability-mode=downstream` 在本工具链报 invalid value（该拼写尚不可用），`default` 一档即可。[实测]
- 已知限制 [检索：swift.org cxx-interop 文档、swiftlang status 页、论坛]：
  - Swift 协议不暴露给 C++；
  - 依赖模块的公开类型不打进生成头（forum 已知坑）；
  - 不支持的 Swift API 映射为 unavailable C++ 声明；
  - SwiftPM 对「带 C++ interop 的库分发」支持不成熟；CMake 无一等 Swift 头生成支持（kitware issue #26188），集成需手动 include 头目录 + 手动链 Swift 运行时。
- 结论：**直接 include 生成头、C++ 侧拿 `CortaTerminal::TerminalSession` / `::Grid` 是真实可行的**，但边界类型是 `swift::String/Array` 形态，工程上建议再加一层薄 Swift 门面（我们自己包里的新 Swift 文件）把边界整成 C++ 干净类型——这一层很短，且不用改上游。

### b) ObjC 桥【不可行/不划算】
- 核心公开面**无 @objc/NSObject**：唯一 NSObject 是私有 ReaderBox（TerminalSession.swift:587，private）。[一手源码 + grep]
- ObjC 路线 = 在我们自己的包里写 @objc shim 类包一遍 TerminalSession/Grid——面不小（Grid 逐格访问、属性字符串、回调转发），且比 a) 多一层无收益。

### 社区实践证据
- SwiftTerm（同类 Swift 终端库）**没有公开的非 Swift 宿主嵌入先例**；社区共识路线 = C ABI 包装（`@_cdecl`）构建共享库，或 Swift 5.9+ 反向 C++ interop。[检索：GitHub SwiftTerm、swift.org 文档、WWDC23 session 10172 / WWDC25 session 311、conan 博客]
- 反推 Corta：上游活跃（双月 443+ commits），interoperability 编译矩阵上游未必维护，**interop 开关由下游 `swift build` 旗标传导（不改上游 Package.swift）是风险最低姿势**。[推断 + 检索]

### 推荐
**路线 a（C++ interop + DevDesk 自建薄门面包）**，风险低于 ObjC 桥（上游零 @objc 面）也低于 vendor 重写；POC 已实证编译通过。

## Q3 渲染归属

### Metal 渲染器在应用层【一手源码，非 core】
- 应用仓 `Corta/Renderer/`：`Metal4Backend.swift`(+Encoding/InstanceBufferRing)、`TerminalRenderer.swift`(+Cells/CursorCapture/FrameColors/Overlay/RowCache)、`Shaders.metal`、`KittyImageRenderer.swift`(+Decode/Drawing/Leases)。核心包无 Metal/AppKit import。[一手源码 + 实测 import 扫描]
- 宿主视图：`final class TerminalView: NSView, CALayerDelegate`，注释明言 hosting `CAMetalLayer`（Corta/TerminalView/TerminalView.swift:22-25）。不是 MTKView，是直挂 CAMetalLayer。[一手源码]

### 若复用 Corta 应用层渲染器：EUI-NEO 能否嵌原生 NSView？
**没有受支持 API，只有手术先例：**
- `EUI-NEO/core/platform/native_bridge.c` 仅有 `eui_set_native_child_window(parentWindow, childWindow, enabled)`（:40）——**原生子窗口**（独立 NSWindow），不是内容区子视图。[一手源码]
- AppKit 子视图操作有深度先例，但全是窗口材质用途：NSVisualEffectView 挂 NSThemeFrame、hitTest 恒 nil、还有两次失败尝试的取证记录（FSCV 会把 GL surface 整体下移 32pt 等）。[EUI-NEO docs/平台能力.md:220-221]
- 窗口本体走 GLFW（core/app/glfw_app_main.cpp）。往 GLFW NSWindow 的 content view 上叠 NSView 子视图技术上可做（材质先例证明平台层有这个操作能力），但 z 序/hitTest/resize 几何联动/与 GL surface 共存都要 EUI-NEO fork 侧新写原生桥接，且平台能力文档的失败记录说明这块水不浅。[一手源码 + 推断]

### 若 EUI-NEO 自绘终端文本网格
- EUI-NEO **没有公开的批量绘制原语**：include/ 无 drawList/addText 命中；图表组件是逐元素建 UI 节点（组件.md pieChart:130）；`科学绘图TODO.md` 自述「折线为线段/数据点创建独立 UI 节点」且数据驱动渲染体系是规划非现状。[一手 docs + grep]
- 已有可复用能力锚点：等宽字体 `.fontFamily("monospace")`（docs/EUI-NEO-CAPABILITIES.md:296）、`components::input` 的 `.fontFamily/.fontSize` + 内置选区/IME 预编辑（:79）、`onKeyEvent`（:456）。
- 工作量级 [推断]：**框架级新能力（M-L）**——需要批量字形渲染组件（glyph atlas + 逐帧脏区提交），现有逐节点 UI 模型撑不起 80×24+ 高频重绘；好处是完全平台中立，Windows 未来同一套渲染直接用。

### 结论
两条路线都要 EUI-NEO 侧投入：嵌 NSView（S-M 原生桥接 + 长期共存风险，macOS-only）vs 自绘网格（M-L 框架能力，双平台复用）。仅做 macOS 底座、求最快出活的路线是前者；求架构正的路线是后者。此决策留给技术方案阶段。

## Q4 构建集成

### `swift build` 产物形态【实测】
- automatic library product：`.build/debug/Modules/CortaTerminal.o` + `CortaTerminal.swiftmodule`，**无 .a**（静态归档需要 Package 声明 `type: .static`，上游只读改不了）。
- 可执行工具（corta-exec 等）正常产出二进制。

### CMake 侧集成做法
- CMake 无一等 Swift 支持（头生成也要自己编排，kitware #26188）。[检索]
- 可行做法（推荐）：
  1. DevDesk 增加自建 SwiftPM 桥接包（仓内 `third_party/` 或 `bridge/` 目录），`path:` 依赖 CortaTerminal submodule，产品=我们的门面库（含 interop 门面 Swift 文件）；
  2. CMake `add_custom_command` 跑 `swift build -c release -Xswiftc -cxx-interoperability-mode=default`（`-Xswiftc` 旗标传导到依赖 target 编译，上游零改动）；
  3. CMake 侧 include 生成头目录 + 把产物 `.o`（或桥接包的静态库）用 object library / 直接 link 进 DevDesk target + 链接 Swift 运行时（`-lswiftCore` 等，或 `-static-stdlib` 免部署机依赖）。[一手 + 检索 + 推断]
- 与 DevDesk 构建体系兼容性：该链路只存在于 macOS，符合「平台差异拆文件 + CMake 按平台选源」纪律（Windows 不编译 swift target，业务代码无 #ifdef）。构建入口仍走 `scripts/build-test-run.sh`（脚本是否需要感知 swift 依赖，留给技术方案）。

### macOS 26 硬依赖真实落点
- 核心源码 `@available` 标注 = **0 条**（全 Sources/CortaTerminal grep）；imports 全集 = Foundation/Synchronization(macOS 15+)/Darwin/Dispatch/Compression。26.0 下限落在 Package.swift 的 `platforms: [.macOS("26.0")]` 声明与 swift-tools 6.2 工具链要求上，**不是 API 硬依赖**。[实测 grep + 一手源码]
- DevDesk 当前 deployment target：CMakeLists **未设置**（`build/CMakeCache.txt` 里 `CMAKE_OSX_DEPLOYMENT_TARGET:STRING=` 为空），实机 `build/DevDesk` 的 LC_BUILD_VERSION `minos 27.0`。27.0 ≥ 26.0，**无冲突，无需下调**；若未来 DevDesk 想支持更老系统，才需要动包声明（fork patch 或上游 PR）。[实测 otool/CMakeCache]
- 附：上游 pin Xcode 26.6.0（`.xcode-version`），本机 27.2 实测也能编过核心 target。[实测]

### 部署细节（易漏）
- **corta-exec 必须随包分发**：核心 spawn 经由 posix_spawn 启动 corta-exec（补 TIOCSCTTY）再 execve shell，打包脚本要把这个助手 Mach-O 放进 .app 并让核心找得到（寻址逻辑在 Spawn.swift，集成时需核对 path 解析）。[一手源码 Spawn.swift:20-21,96]
- corta-image-decoder（沙盒 PNG 解码助手）终端用不到 Kitty 图像则可不带，但 Kitty 图形支持在核心里，若要支持 `imgcat` 需要评估。[推断]

## Q5 输入路径

- **核心 input API**（全部字节级）：`write(_ bytes: [UInt8]) -> WriteOutcome`（TerminalSession+Writing.swift:37）、`write(chunks:)`(:55)、`write(paste:closing:)`(:66，bracketed paste 带取消语义)、`writeFocusReport(isFocused:)`(:46)、`interruptPendingPaste()`(:99)。写侧 enqueue 到串行队列，永不阻塞在子进程不读上。[一手源码]
- **键盘→字节编码在应用层**（Corta 自己的 NSView，我们不继承它，但它是现成的编码参考实现）：
  - `TerminalView+Keyboard.swift:29` `override func keyDown(with event: NSEvent)`——IME 收编的键走 `insertText`，终端功能键走 `doCommand(by:)` 从 key event 编码；
  - `TerminalView+IME.swift:43-49` `insertText` → `onKeyBytes?(Array(text.utf8))`；组合期 marked text 不进 grid 不进 PTY（:21 注释）；
  - 核心另有 `KeyboardProtocol.swift`（kitty 键盘协议侧）。
- **DevDesk/EUI-NEO 侧能力**：`onKeyEvent` 元素级 + 窗口级 fallback（EUI-NEO-CAPABILITIES.md:456-463,633）；IME macOS/Windows 完整（:25）；`components::input` 内置选区/撤销/IME 预编辑（:79）。**有按键事件**，符号锚点 `eui::KeyEvent`（CAPABILITIES.md:463）。
- 缺口 [推断]：DevDesk 侧需要自己实现「KeyEvent→终端转义序列」编码器（功能键/修饰键/ kitty 协议），Corta 应用层的 Keyboard/IME 两个 Swift 文件是可对照移植的一手参考。

## Q6 Windows 占位（ConPTY 最小概念面）

ConPTY = Windows 伪控制台 API：`CreatePipe×2 → CreatePseudoConsole(size, hIn, hOut, 0, &hPC) → STARTUPINFOEX 挂 PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE → CreateProcess → 双向中继字节 → ResizePseudoConsole / ClosePseudoConsole`。[检索：Microsoft Learn「Creating a Pseudoconsole session」]
平台中立接口设计要点：与核心 `Seams.readerSource/writerSink` 抽象同构，PTY 抽象面 = `{spawn, read, write, resize(rows,cols), terminate}` 五个操作即可覆盖 Darwin PTY 与 ConPTY 两端。[推断]

## Q7 引入方式权衡

上游状态（gh api，2026-11-04）：created 2026-09-02、最近 push 2026-11-01（3 天前）、22 stars、open issues 3、Apache-2.0、双月新仓高频开发。

| 方式 | 优点 | 缺点 | 适配面 |
|---|---|---|---|
| A. submodule 指上游（只读）+ DevDesk 桥接层 | 零 fork 维护；升级=fast-forward；与 DevDesk 既有 submodule 纪律一致（EUI-NEO 先例） | 无法打补丁——需 patch 概率不高但非零（如未来要求静态库 product、deployment target 变更、corta-exec 寻址修正）；上游高频演进可能带破坏性 API 变更，固定 commit 可缓解 | 本调研的 interop 路线**不需要改上游**（旗标传导），A 的最大短板被绕开 |
| B. vendored 快照 | 完全自主，断网可构建 | 同步全手动；仓库大（含 golden/fuzz 语料）；失去上游修复；Apache-2.0+NOTICE 合规要自己做 | 不推荐，除非确认长期深度改造 |
| C. fork 后 submodule | 可打补丁（EUI-NEO 同款模式：origin 只读 + fork 同步）；patch 概率兑现时无阻塞 | 双机/CI 多一套 remote 纪律；fork 落后上游的漂移风险 | patch 需求概率中等时最稳 |

patch 需求概率评估 [推断]：偏低但非零——已知潜在诉求=①静态库 product 声明（可用下游包绕开）②deployment target 调整（当前 27.0 无需）③corta-exec 打包寻址（大概率 DevDesk 侧可解决）。**A 与 C 都可行，差别只在「要不要预留补丁能力」；本报告不替老板拍板，倾向信号：若接受「不改上游」纪律则 A 够用，若要保险则 C。**

许可：Apache-2.0 对子模块引入友好；吉祥物/图标 NOTICE 条款只要不带上游美术资产即不触发。

## 推荐（综合）

1. 引入方式 A（submodule 指上游固定 commit）+ DevDesk 自建 SwiftPM 桥接包（path-depend + interop 门面），CMake add_custom_command 编排，Windows 不编译该链路。
2. 渲染路线二选一留给技术方案卡：嵌 NSView（快，macOS-only，EUI-NEO 原生桥接 S-M）vs EUI-NEO 自绘网格（慢，双平台复用，框架级 M-L）。
3. 首个里程碑建议不带 UI 验证核心链路：corta-dump 等价物（字节进→Grid 出）在 DevDesk C++ 侧跑通，再接输入编码与渲染。

## 遗留

- Swift 运行时库部署形态（动态 dylib 随包 vs `-static-stdlib`）未实测体积/许可影响，技术方案阶段定。
- corta-exec 运行时寻址逻辑（Bundle/PATH）未逐行核对 Spawn.swift 全文，集成时要确认。
- Grid→C++ 侧逐格读取的真实性能（swift:: 值类型边界开销）未做 POC 基准，属首里程碑验证项。
- EUI-NEO 嵌 NSView 与 GL surface 共存的 z 序/resize 细节只有材质场景的先例，终端场景需真机验证。

## 来源清单

精读（一手源码，clone HEAD 4af5623）：
- CortaTerminal/Package.swift（全文）
- CortaTerminal/Sources/CortaTerminal/TerminalSession.swift（全文）
- CortaTerminal/Sources/CortaTerminal/TerminalSession+Writing.swift（public API 面）
- CortaTerminal/Sources/CortaTerminal/Spawn.swift（corta-exec 关键注释）
- Corta/TerminalView/TerminalView.swift（视图宿主声明）
- Corta/TerminalView/TerminalView+Keyboard.swift / +IME.swift（输入路径）
- EUI-NEO：core/platform/native_bridge.c、docs/平台能力.md:220-221、docs/组件.md、docs/科学绘图TODO.md、docs/EUI-NEO-CAPABILITIES.md（交互/键盘/字体行）
- DevDesk：build/CMakeCache.txt、build/DevDesk（otool LC_BUILD_VERSION）

实测（POC）：
- `swift build --target CortaTerminal`（5.29s 通过）与加 `-Xswiftc -cxx-interoperability-mode=default`（3.78s 通过）
- 生成头 CortaTerminal-Swift.h 暴露面 grep（Grid :8008 / TerminalSession :15247）
- 核心源码 import 全集与 @available 计数 grep

扫描（检索/二手）：
- [Mixing Swift and C++ — swift.org](https://www.swift.org/documentation/cxx-interop)
- [Swift C++ interop 状态页](https://swift.swiftgg.team/documentation/cxx-interop/status)
- [swift-evolution: Using Swift from C++](https://github.com/apple/swift-evolution/blob/main/visions/using-swift-from-c%2B%2B.md)
- [论坛：生成头不打外部模块类型](https://forums.swift.org/t/feature-print-external-types-in-swift-h-if-they-are-part-of-the-public-api/84208)
- [CMake issue #26188：Swift interop 头生成无一等支持](https://gitlab.kitware.com/cmake/cmake/-/issues/26188)
- [SwiftPM C++ interop 库分发讨论](https://forums.swift.org/t/c-c-interop-improvements-with-swiftpm-for-libraries/76821)
- [SwiftTerm（无 C++ 宿主先例的佐证）](https://github.com/migueldeicaza/SwiftTerm)
- [WWDC25: Safely mix C, C++, and Swift](https://developer.apple.com/videos/play/wwdc2025/311) / [WWDC23: Mix Swift and C++](https://developer.apple.com/videos/play/wwdc2023/10172)
- [MS Learn：Creating a Pseudoconsole session（ConPTY）](https://learn.microsoft.com/en-us/windows/console/creating-a-pseudoconsole-session)
- [ConPTY 发布博文](https://devblogs.microsoft.com/commandline/windows-command-line-introducing-the-windows-pseudo-console-conpty)
- GitHub API：repos/noah-qin/Corta（活跃度，gh api 实测）
