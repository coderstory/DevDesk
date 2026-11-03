# Windows 真终端全链路技术方案调研（[pool:need:123]）

- 日期：2026-11-04 ｜ 角色：researcher ｜ 状态：终版
- 场景：DevDesk Windows 侧终端模块页内嵌真终端面板（macOS 侧并行调研走 Corta 核心 [pool:need:122]，池内 corta 线索 1-7 已复用不重查）
- 硬约束：禁浏览器套壳（xterm.js/webview/electron 全排除）；全链路 = PTY→解析→渲染→输入回写；MinGW64 工具链可集成、单 exe 零外部 DLL、日常使用质量
- 证据分级标注：〔实跑〕〔一手〕〔二手〕〔推测〕，其中〔一手〕= 本轮精读官方文档/源码头文件；池内 clue 8-16 为本次新查证事实（下游免重查）

---

## 结论先行

1. **全链路无外部阻塞，且能守住单 exe 零外部 DLL 契约**：PTY 用 kernel32 ConPTY（Win10 1809+，MinGW-w64 ≥7.0 头文件自带声明、链 kernel32 即可，无需第三方库）〔一手〕；解析器 libvterm（MIT、C99、零依赖、静态编入 exe）〔一手〕；渲染走 EUI-NEO 自绘（底座已齐，见 Q3）〔一手〕。许可证链全绿（MIT ×2 + OS 内置系统组件）。
2. **解析器推荐 libvterm 0.3.x**：neovim :terminal 同款（2015 年至今生产验证）、damage 矩形回调 + cell 级回读与网格渲染天然对齐、滚动回（scrollback）回调内置。微软 Terminal parser 许可允许（MIT）但 MinGW 集成高风险且无收益；自研工作量与回归风险显著高于引入成本。〔一手+二手〕
3. **EUI-NEO 渲染底座比预期完整，自绘路线贴合度最高**：字体测量（text_measure.cpp）、文本整形（text_shaping.cpp）、字形 atlas + 顶点批渲染（vulkan_text.cpp 的 ensureTextAtlas/flushTextBatch）在后端层全部存在；缺的只是 DSL/组件层的「终端网格」组件——是新增一个组件，不是给框架加渲染能力。〔一手 codegraph〕
4. **IME 输入链路现成**：EUI-NEO Windows 平台层已有 IME 组合状态机（queryImeComposition，window_input_backend.cpp:81 → input_state.h consumeTextInput），CAPABILITIES.md:25 明言双平台 IME 完整；终端组件接现有链路，组合期文本走 overlay、不进 grid 不进 PTY（Corta 同款先例）。键盘→VT 编码需自写映射表（业界通行做法，~150 行级）。〔一手〕
5. **主要风险集中在 ConPTY 行为面**（resize 竞态、关闭排空、吞吐节流），全部有官方文档级说明与社区成熟解法；NuGet redist（Microsoft.Windows.Console.ConPTY，MIT）可作为绕开老 OS conhost 的可选热升级，但会引入外部 DLL，与零 DLL 契约冲突 → 列为老板拍板项（见遗留）。〔一手〕

---

## Q1 ConPTY 现状

### API 形态〔一手：MS Learn《Creating a Pseudoconsole session》，2025-08 更新〕

三个函数全在 kernel32，HRESULT 返回：

```c
HRESULT CreatePseudoConsole(COORD size, HANDLE hInput, HANDLE hOutput, DWORD dwFlags, HPCON* hPC);
HRESULT ResizePseudoConsole(HPCON hPC, COORD size);
void    ClosePseudoConsole(HPCON hPC);
```

标准会话建立流程（官方文档同款）：
1. `CreatePipe` ×2 建立**同步**管道（禁 OVERLAPPED）：inputRead/inputWrite、outputRead/outputWrite；
2. `CreatePseudoConsole(size, inputRead, outputWrite, dwFlags, &hPC)`；
3. `STARTUPINFOEX` + `InitializeProcThreadAttributeList`（双调算尺寸再初始化）+ `UpdateProcThreadAttribute(PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hPC, sizeof(hPC))`；
4. `CreateProcess` 带 `EXTENDED_STARTUPINFO_PRESENT`；
5. CreateProcess 成功后**立即关闭** inputRead/outputWrite（引用计数，保断管检测）；
6. 独立线程读 outputRead / 写 inputWrite 中继字节。

dwFlags 当前有 `PSEUDOCONSOLE_INHERIT_CURSOR`（继承光标，需应答查询）〔一手〕；新版 conpty 另有 passthrough 模式旗标〔推测：见于新版 conhost，本轮未核验具体常量〕。

### 版本门槛〔一手〕

- OS 内置：Windows 10 **1809（10.0.17763）**起。
- NuGet redist：官方包 `Microsoft.Windows.Console.ConPTY`（owner: Microsoft.Terminal）——**"should work on all versions of Windows 10.0.17763.0 and above"**，包内含 conpty.dll + OpenConsole.exe；最新稳定 **1.25.260930003（2026-10-02）**，另 `CI.` 前缀 preview 通道（1.26 preview 同日）；许可 **MIT**；native 目标、零依赖、1.68MB、总下载 167.2K。微软策略 = 用 NuGet 分发 ConPTY 修复替代 OS servicing（microsoft/terminal issue #17452）〔一手 nuget 页 + 二手 issue 语境〕。
- 架构匹配：OpenConsole.exe 须匹配**内核**位数、conpty.dll 须匹配**应用**位数（discussion #20391）〔二手〕。

### MinGW 链接方式〔一手+二手〕

- 无独立 conpty.h：`HPCON`/`CreatePseudoConsole`/`ResizePseudoConsole`/`ClosePseudoConsole` 声明在 mingw-w64 **wincon.h**（v7.0 起随 RS5 patch 加入），`PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE` 在 processthreadsapi.h；链接 kernel32（MinGW 默认已链）→ **DevDesk 的 MSYS2 MinGW64（当前 headers 版本远新于 7.0）直接可用，零额外依赖**。
- NuGet 包是 .nupkg 原生二进制，CMake 侧不需要它也能编（用 OS 内置 API）；若选择 redist 路线，则解包后将 conpty.dll/OpenConsole.exe 随 exe 分发并动态加载（详见遗留 1）。

### 已知坑〔一手官方 Warning/Note + 二手 issue〕

| 坑 | 说明 | 对策 |
|---|---|---|
| 单线程死锁 | 官方 Warning：两管道须各由独立线程服务，单线程同步模式下缓冲填满即死锁 | 读/写/关各归其线程；写侧串行队列（Corta 线索 2 同款模型） |
| 关闭排空 | `ClosePseudoConsole` 可能向 hOutput 发收尾帧，不排空即死锁；`PSEUDOCONSOLE_INHERIT_CURSOR` 下不回应光标查询（hOutput 收问、hInput 答复）也死锁 | 管道线程持续排空至断裂；关闭流程 = 先等读线程见 EOF 再 join |
| 启动期关闭 | 子进程尚在连接时关闭会话 → 子进程弹 **0xc0000142** 错误对话框 | 关闭前确认子进程已起或先 terminate 子进程 |
| resize 竞态 | 输出飞行中调 `ResizePseudoConsole` 会截断行（discussion #16879）；reflow 由 conhost 做，跨尺寸回放花屏（issue #16985） | resize 在读侧暂停窗口内同步调用（issue #1156 社区口径）；UI 侧防抖合并 |
| 吞吐 | ConPTY 把屏幕缓冲重编码为 VT 帧输出，存在历史实测人为节流（~0.01 MiB/s，kichwacoders 2021〔二手/历史数据〕，新版 conhost 已改善） | Windows 11 目标基本无忧；如需保 Win10 老版本，用 NuGet redist 带新 conpty |
| 崩溃隔离 | 会话由 conhost/OpenConsole **进程外**承载；conhost 崩溃 → 管道断裂，宿主 app 通过 broken channel 语义感知（官方句柄生命周期说明），不带走宿主 | 读线程 EOF/断管 → 终端组件显示「会话已结束」 |

---

## Q2 VT/终端解析器选型

### 对比表

| 维度 | libvterm 0.3.x | 微软 Terminal parser/TerminalCore | libghostty-vt（新发现） | 自研 parser |
|---|---|---|---|---|
| 许可 | **MIT**〔一手：brew formula + neovim fork LICENSE 双源〕 | **MIT**〔二手：仓 README；本轮未逐文件核验〕 | 待查（Ghostty 主体 MIT） | — |
| 语言/形态 | C99 单库，零依赖，运行态零 malloc〔一手〕 | C++，wil/MSVC 取向，仓内子项目 | Zig（不可直接链） | C++20 |
| 接口形态 | `vterm_input_write` 喂字节；回调：damage/moverect/movecursor/settermprop/bell/resize/sb_pushline/sb_popline/sb_clear〔一手 include/vterm.h〕 | parser 状态机 + TerminalCore 缓冲层；无公开稳定 ABI | 待查 | 自定义 |
| 状态回读 | damage 矩形回调 + `vterm_screen_get_cell` 逐 cell（chars[6]/width/attrs/fg/bg）+ `vterm_state_get_cursorpos` + 颜色 `vterm_state_convert_color_to_rgb`〔一手〕 | 有（完整缓冲模型） | 待查 | 自定义 |
| 依赖 | 无 | til/terminal-inc/wil（TerminalCore 再拖 adapter/renderer）〔二手〕 | Zig 工具链 | 无 |
| MinGW 集成 | 直接编（C99） | **高风险**：无官方拆库，wil/Windows 类型未验证 MinGW 构建〔二手+推测〕 | 不可行（C++ 宿主） | 可行 |
| 活跃度 | 上游慢（v0.3.3，leonerd 页 2023-01 后无新 release），实际维护在 neovim vendored 副本〔一手+二手〕 | 高频（微软官方仓） | 活跃（Ghostty 拆出中） | — |
| 真实先例 | **neovim :terminal（2015+）、pangoterm**〔一手+二手〕 | Windows Terminal、conhost 自身 | Neovim 社区评估替换 libvterm（reddit r/neovim 1u10uup）〔二手〕 | — |

另：libtsm 等历史候选存在但本轮未验证（多年未维护迹象），不纳入对比；候选面不封闭，后续发现即纳入。

### 各候选落点判断

- **libvterm（推荐）**：cell 模型（VTermScreenCell：chars[6] UTF-32 + width 宽字标志 + attrs（bold/underline/italic/reverse/strike/…）+ fg/bg tagged-union RGB/索引色）与 EUI-NEO 网格渲染 1:1 对应；damage 回调直接驱动脏区重绘；滚动回用 sb_pushline 存宿主侧 ring buffer、回滚时 sb_popline 拉回。API 全签名见 clue 11〔一手 include/vterm.h〕。鼠标上报编码库自带（`vterm_mouse_move`/`vterm_mouse_button`，按 VTERM_PROP_MOUSE 模式启用）；键盘编码库不管（需自写表，见 Q4）。
- **微软 parser**：许可允许、代码质量高，但 src/terminal/parser 相对自包含（依赖 terminal/inc + wil + Windows 类型）、TerminalCore 深耦合 til/adapter/renderer〔二手〕；无官方拆库 NuGet；MinGW 构建未验证、wil/MSVC/SAL 取向阻力大 → 相比 libvterm 无净收益，除非将来需要其独有的高阶特性（conpty 深度协同）。
- **自研**：需覆盖 ECMA-48 CSI 全谱 + DEC 私有模式 + OSC（标题/剪贴板/超链接）+ DCS + SGR 24bit + alt-screen + 滚动区 + DSR/DA 应答 + UTF-8 宽字，neovim/xterm.js 的 parser 均数千行量级且经年修 bug〔推测，未本轮实测〕；vttest 回归面大。「日常使用质量」门槛下自研性价比最低。

---

## Q3 渲染路径：EUI-NEO 自绘网格 vs 原生 HWND 子窗口

### 路径 1：EUI-NEO 自绘等宽文本网格（贴合度高，推荐）

〔一手 codegraph，本仓 HEAD 2026-11-04〕渲染底座已存在：

- 后端文本接口 `drawText`：EUI-NEO/core/render/render_backend.h:281；
- 文本栈全套：core/render/ 下 text.h / text_layout.cpp / **text_measure.cpp**（等宽 cell 宽高测量）/ **text_shaping.cpp**（CJK 整形）/ text_types.h；
- 批量字形管线：vulkan_text.cpp 的 ensureTextAtlas / ensureTextVertexBuffer / **flushTextBatch**（TextPushConstants 携带窗口尺寸，vulkan_text.cpp:35-48）；
- 后端双目录 opengl/ + vulkan/，app 入口 glfw_app_main.cpp / sdl2_app_main.cpp，`ScopedRenderBackend`（render_backend.h:362）按窗口注入。

结合 corta 线索 7（DSL 组件层无批量文本原语、图表逐元素建 UI 节点）修正：**缺的是组件层「终端网格」一个新组件**——字形缓存 + dirty-rect（libvterm damage）→ 批顶点，底座（测量/整形/atlas/批渲染）零新增。主题联动（EUI-NEO 三档材质、主题色）天然免费；布局/焦点/滚动随框架。

### 路径 2：原生子窗口（HWND child + 自绘循环）

- 挂点存在但语义是**窗口级**：`eui_set_native_child_window(parentWindow, childWindow, enabled)`（core/platform/native_bridge.c:40，corta 线索 7）——原生子窗口，非内容区子视图；EUI-NEO 无内容区原生视图嵌入 API，AppKit 侧同类手术有两次失败记录（docs/平台能力.md:220-221，corta 线索 7）。
- 优点：IME/滚动/选区可用 Win32 原生机制，DirectWrite 全控制（Windows Terminal AtlasEngine 同路），性能天花板高。
- 代价：与 EUI-NEO 主题/材质/布局脱节（手动同步）；弹层 z 序/焦点管理复杂（EUI-NEO 嵌套弹层 z 序逃逸是修过的已知痛点区）；**Windows 独有路线**——macOS 侧 Corta 同样无法嵌 NSView 到内容区，等于两平台各养一套渲染，违背「平台差异拆文件、能力优先框架修」的方向。

### 结论

路径 1 贴合度显著更高（底座齐全 + 主题/布局免费 + 双平台同构：macOS=Corta 核心+网格组件，Windows=libvterm 核心+同一网格组件）；路径 2 仅作 fallback（若网格组件在极端输出下性能不达标再评估）。两平台共享「网格渲染组件」设计是最大架构红利。〔一手+推断〕

---

## Q4 输入与 IME

### ConPTY 输入编码回写

宿主把交互事件编码为 VT 字节写 inputWrite 管道（官方文档：hosting app 负责把用户交互 serialize 进写端）〔一手〕。编码面：

- **键盘 → VT 映射表（自写，业界通行，Corta 线索 4 同款分工：应用层编码、核心只收 `write(bytes)`）**：方向键 `ESC[A/B/C/D`、Home/End/PageUp/Down、F1-F12、修饰键 xterm 编码（`ESC[1;5A` 等）、Enter/Backspace/Tab、Ctrl+字母 C0；约百余行 C 表。规格参照微软官方 VT 序列文档（learn.microsoft.com/windows/console/console-virtual-terminal-sequences）与 xterm ctlseqs〔二手参照，编码本身为业界标准〕。
- **鼠标上报**：libvterm 自带——`VTERM_PROP_MOUSE` settermprop 回调告知启用模式（NONE/CLICK/DRAG/MOVE），宿主调 `vterm_mouse_move/button`，库生成 SGR/普通编码回写〔一手 include/vterm.h〕。
- **粘贴**：bracketed paste（`ESC[200~…ESC[201~`），libvterm `VTERM_PROP_PASTE` 属性回调协同启用〔一手〕。
- **焦点事件**：`ESC[I` / `ESC[O`，按子进程使能位发送〔推测：标准 xterm 行为〕。

### 中文 IME 在自绘窗口的落法〔一手 codegraph + corta 先例〕

- EUI-NEO Windows 平台层已有完整 IME 组合状态机：`queryImeComposition`（EUI-NEO/core/window/window_input_backend.cpp:81）→ `consumeTextInput`（core/input/input_state.h：setComposing / setCompositionText / compositionText / isComposing），window_only_probe.cpp 有探针测试；CAPABILITIES.md:25「IME macOS/Windows 完整」。
- 终端组件落法：组合期（isComposing）组合文本画在光标位 **overlay**，不进 grid、不进 PTY（Corta TerminalView+IME.swift:21 同款纪律，corta 线索 4）；commit 后按 UTF-8 编码 `write` 进 ConPTY。宽字符在 grid 中占 2 cell（libvterm VTermScreenCell.width==2，绘制时合并）〔一手〕。

---

## Q5 真实先例盘点（Windows 原生、非浏览器）

| 产品 | 架构 | 许可 | 可借鉴点 |
|---|---|---|---|
| **Windows Terminal**（官方） | ConPTY + 独立 parser/TerminalCore + AtlasEngine（DirectWrite 文本整形 + dx 渲染）〔二手〕 | MIT | AtlasEngine 的 DirectWrite 字形 atlas 思路；与 conhost 团队同源 = ConPTY 行为权威；NuGet redist 即出自此仓 |
| **mintty**（MSYS2 默认终端） | 原生 Win32 + GDI 自绘，xterm 兼容，支持 ConPTY〔二手〕 | **GPL-3.0-or-later**（禁止链代码，仅架构参照） | ConPTY 宿主行为参照；Ctrl+C 对原生 MinGW 程序的信号语义坑（SO 61523818）：原生 Windows 程序走 console API 非 POSIX 信号 |
| **alacritty** | Rust，2018 底起 Windows 全支持，ConPTY（1809+）+ OpenGL 渲染〔二手〕 | Apache-2.0 | 实证 ConPTY 路线可达日常质量；坑：OpenGL 驱动子集性能问题；undercurl 因 ConPTY 环境缺 terminfo 不生效（issue #8392）→ **SSH 到 Unix 主机时 TERM/terminfo 探测是 ConPTY 宿主通用坑**，解法=随包带 terminfo 或按 host 降级 |
| **ghostty** | Zig；**Windows 未支持**，官方「planned for the future」，discussion #2563 讨论中，无 ETA（2026-11 时点）〔一手官方 docs + 二手〕 | MIT | 无 Windows 先例可借；其拆出的 libghostty-vt 是解析器选型观察项（Zig 不可链） |
| wezterm（参照系） | Rust，Windows 支持成熟，自建跨平台 pty/渲染栈〔二手〕 | MIT | 跨平台终端「自建栈」的组织方式参照 |

共同启示：所有存活的原生 Windows 终端都是 **ConPTY + 自有解析 + 自绘网格** 三段式；无一用浏览器内核；老 OS conhost 兼容是普遍痛点（NuGet redist 即官方解）。

---

## Q6 方案建议（两套，供拍板，不预设唯一答案）

### 方案 A：ConPTY（kernel32）+ libvterm + EUI-NEO 自绘网格组件 —— 贴合度最高

- **组件选型**：PTY=OS 内置 ConPTY（wincon.h 声明、链 kernel32）；解析=libvterm 0.3.x 静态编入（CMake add_subdirectory 或 vendored）；渲染=新增 EUI-NEO「终端网格」组件（基于 text_measure/text_shaping/atlas/flushTextBatch 底座）；输入=自写键盘→VT 表 + libvterm 鼠标/粘贴编码。
- **集成方式**：平台拆文件纪律照走——`terminal_*_win.cpp`（ConPTY 桥：spawn/read/write/resize/terminate 五接口，Corta 线索 7 同款平台中立接口）+ 终端核心跨平台层（libvterm 封装，macOS/Windows 共享接口形态，与 Corta 会话模型互换）+ 网格组件共用。会话模型照抄 corta 线索 2：reader 线程（批上限 ~1MB）+ 写侧串行队列 + resize 防抖（Q1 坑⑤：读侧暂停窗口内同步调 ResizePseudoConsole）+ 关闭排空至 EOF。
- **许可证链**：MIT（libvterm）+ OS 内置系统组件（ConPTY）→ 静态链接零冲突，**单 exe 零外部 DLL 契约可保**。
- **风险清单**：① ConPTY resize/吞吐/关闭排空（Q1 表，均有成熟对策）；② 网格组件是主要工作量（M：字形缓存、dirty-rect、宽字合并、滚动回 ring buffer）；③ libvterm 上游更新慢 → 锁 vendored 副本 + neovim 补丁参考；④ SSH 远端 terminfo 坑（alacritty #8392 先例）→ POC 期验证 `TERM` 策略；⑤ EUI-NEO Windows 侧实际渲染后端（opengl vs vulkan 目录并存）由 developer 开工前确认（本调研见双后端 + glfw/sdl2 双入口，未逐一核验 Windows 选型路径〔待验证〕）。
- **工作量级**：**L**（可分期：PTY 桥+裸终端 S；网格组件 M；输入/IME/滚动回/主题 M；打磨与 QA M）。总体估 3-5 周纯开发量级〔推测，按组件分解外推〕。

### 方案 B：ConPTY + libvterm + HWND 原生子窗口自绘循环 —— 性能保底 fallback

- **组件选型**：同 A 的 PTY/解析；渲染改为 `eui_set_native_child_window`（native_bridge.c:40）挂 child HWND，内部 DirectWrite/GDI 自绘循环 + 原生滚动条 + 原生 IME（Imm32 直管）。
- **集成方式**：`terminal_view_win.cpp` 独立窗口类；EUI-NEO 页面区定位→同步 child HWND 矩形；输入事件经 EUI-NEO 转发或 HWND 自收。
- **许可证链**：同 A（libvterm MIT）。
- **风险清单**：① 与 EUI-NEO 主题/材质/弹层 z 序脱节，手动同步面大（EUI-NEO 嵌套弹层 z 序是修过的痛点区）；② **Windows 独有**——macOS 侧无对等路线（corta 线索 7：EUI-NEO 无内容区原生视图嵌入 API，AppKit 手术有失败记录），两平台各养一套渲染；③ 焦点/快捷键在两套窗口体系间的路由复杂度；④ eui_set_native_child_window 的窗口级语义与页内面板尺寸跟随需要验证〔待验证〕。
- **工作量级**：**L+**（在 A 之上叠加独立渲染/输入栈）。
- **定位**：仅当方案 A 网格组件实测性能不达标（极端输出场景）时切换；不建议双轨并行。

### 两案共同前提（拍板项见遗留）

解析器 = libvterm；PTY = ConPTY；浏览器路线已按硬约束排除（xterm.js/webview 不进入任何环节）。

---

## 遗留与待验证

1. **〔老板拍板〕NuGet redist 与零 DLL 契约**：`Microsoft.Windows.Console.ConPTY`（MIT）可绕开老 OS conhost 的性能/缺陷，但 conpty.dll+OpenConsole.exe 是外部文件，与「单 exe 零外部 DLL」冲突。Windows 11 目标下 OS 内置 conhost 已够用 → 建议默认 OS 内置、redist 作后续可选热升级（按需下载到数据目录动态加载，不进安装包），是否保留该口子请老板定。
2. 〔待验证〕EUI-NEO Windows 侧实际渲染后端路径（opengl/vulkan 目录并存 + glfw/sdl2 双入口），developer 开工前 codegraph 确认 `ScopedRenderBackend` 在 DevDesk Windows 构建中的选型。
3. 〔待验证〕libvterm CJK 宽字 + EUI-NEO text_shaping 在等宽网格下的配合（cell width==2 绘制合并、emoji/组合字符），POC 期用 `poc_` 用例实证。
4. 〔待验证〕NuGet 包内文件清单（conpty.dll/OpenConsole.exe）来自检索结果与 issue #17452 语境，nuget 页面无 payload 描述，采纳前开包核验。
5. 〔待验证〕微软 parser 若坚持评估：MinGW 构建实测（wil/terminal-inc 编译通过率）。
6. 〔观察〕libghostty-vt 进展（Neovim 是否真替换 libvterm）；Ghostty Windows 落地进度。

## 来源清单

精读（一手）：
- MS Learn《Creating a Pseudoconsole session》：https://learn.microsoft.com/en-us/windows/console/creating-a-pseudoconsole-session （2025-08 更新版）
- neovim/libvterm（nvim 分支）include/vterm.h：https://raw.githubusercontent.com/neovim/libvterm/nvim/include/vterm.h
- libvterm 上游页：https://www.leonerd.org.uk/code/libvterm/
- NuGet：https://www.nuget.org/packages/Microsoft.Windows.Console.ConPTY
- 仓内一手：EUI-NEO/core/render/render_backend.h:281,362；EUI-NEO/core/render/（text_measure.cpp/text_shaping.cpp/vulkan_text.cpp:35-48）；EUI-NEO/core/window/window_input_backend.cpp:81；core/platform/native_bridge.c:40（corta 线索 7）；docs/EUI-NEO-CAPABILITIES.md:25,296,456-463,633

扫描（二手）：
- microsoft/terminal issue #17452（NuGet 分发策略）、discussion #20391（架构匹配）、discussion #16879（resize 截断）、issue #1156（resize 同步）、issue #16985（reflow 重构）
- kichwacoders《ConPTY Performance in Eclipse Terminal》（2021，历史数据）
- mingw-w64 邮件列表 wincon.h ConPTY patch + SO 66419746（MinGW ConPTY 可用性）
- brew formulae libvterm（MIT 双源）、reddit r/neovim 1u10uup（libghostty-vt 动向）
- ghostty.org/docs/features + discussion #2563（Windows 未支持）
- alacritty 仓 + HN 18760971 + issue #8392（terminfo 坑）
- mintty.github.io + Wikipedia Mintty（GPL-3.0）+ MSYS2 docs/terminals + SO 61523818（Ctrl+C 语义）
- 池内复用（不重查）：corta 线索 1-7（need_id=122）

工具调用：28 次（预算 ≤40；含池工具 14、检索/精读 10、仓内 codegraph/ls 3、落盘 1）。
