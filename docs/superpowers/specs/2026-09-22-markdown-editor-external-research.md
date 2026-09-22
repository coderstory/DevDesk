# DevDesk Markdown 编辑器升级方案 — 技术选型调研报告

> 调研时间：2026-09-22 | 目标平台：Windows 11 | 技术栈：C++17 + MinGW-w64 + CMake + EUI-NEO（OpenGL/GLFW/FreeType）
> 硬约束：静态链接单 exe、零外部 DLL、Release 禁用异常/RTTI、资源离线打包

---

## 总览：三条路线对比与最终建议

| 路线 | 体验上限 | 工作量 | 体积增量 | 关键风险 | 结论 |
|---|---|---|---|---|---|
| **A. WebView2 + CodeMirror 6** | 高（可达 Typora 级） | 2-4 周（含 Live Preview overlay） | JS ~138KB gzip + WebView2 Runtime（系统自带） | Chromium 149+ IME 回归 bug；Runtime 版本碎片化 | **首选推荐** |
| **B. Scintilla 子窗口嵌入** | 中（源码+高亮，无 WYSIWYG） | 1-2 人月 | +1~8 MB（可裁剪 lexer） | OpenGL/GDI 渲染层叠；DPI 转发；无 WYSIWYG | 备选（有条件可行） |
| **C. 自研 EUI-NEO 富文本** | 中高（完全可控） | 3-6 人月 | +<500 KB | IME 集成深坑；布局+折行复杂度 | 长期投资方向，短期不推荐 |

**最终建议**：短期采用 **路线 A（WebView2 + CodeMirror 6 + 自研 Live Preview overlay）**，理由是中文 IME 最稳、体积最小、达到 Typora 级体验的路径清晰；同时将 **路线 C（自研富文本）** 作为中长期技术储备，因为 3-6 人月的投入可沉淀框架能力，后续所有文本场景受益。Scintilla（路线 B）作为 WebView2 不可用时的降级方案。

---

# 第一部分：WebView2 + JS 编辑器选型

## 1.1 六维对比总表

| 维度 | **Milkdown** | **TipTap** | **Vditor** | **Bytemd** | **CodeMirror 6** | **Monaco Editor** |
|---|---|---|---|---|---|---|
| **渲染模式** | WYSIWYG（ProseMirror 块级实时渲染，源码↔DOM 双向同步） | WYSIWYG（同 ProseMirror，headless，需自写 UI） | 三模式：WYSIWYG / IR 即时渲染（类 Typora）/ SV 分屏预览 | SV 分屏（CM5 编辑 + React 预览） | 纯源码 + 语法高亮，需自行叠加 Live Preview 层 | 纯源码 + 语法高亮（VS Code 同款） |
| **中文 IME 质量** | ⚠️ 有已知 bug：粗体/斜体等行内格式边界处 composition 时光标错位、文字落位错误。Markra 编辑器因此从 Milkdown 迁移到 CM6 | ⚠️ 同 ProseMirror 底层，同样问题；2025 年修复了 suggestion 插件中文输入（#1449），但行内格式边界 composition 问题未根治 | ✅ 原生中文场景（国产 B3log），IR 模式即 Typora 体验，中文输入为一等公民 | ⚠️ 基于 CM5，IME 表现一般，且项目停更 | ✅ CM6 重写了输入层（input 模块），IME 处理最成熟的源码编辑器；但 2026-06 暴露 Chromium 149+ 回归 bug | ✅ 最成熟（VS Code 同款输入管线），但同样受 Chromium 149+ 回归影响 |
| **离线打包** | ~40KB gzip（仅 core），完整 WYSIWYG 配置 ~200KB gzip；esbuild/rollup 单文件友好，无 worker | 模块化，核心轻量，tree-shake 好；按需选扩展可控制在 150-250KB gzip | vditor.min.js ~283KB min，但 lute.min.js（Markdown 引擎）~3.6MB；mermaid/katex 按需；离线需整包 dist 目录 | 基于 CM5 + React，~200-300KB；停更不建议 | ~138KB gzip 核心（含 markdown 支持），tree-shake 优秀，无 worker，单文件最简单 | 最重：全量 2-5MB gzip；核心 ~940KB gzip；必须打包多个 worker 文件（editor.worker.js / ts.worker.js 5.8MB 等），离线配置复杂 |
| **C++ 宿主通信** | 统一走 WebView2 消息通道，编辑器本身无关 | 同左 | 同左；内置 onchange 回调，天然适配 | 同左 | 同左；Editor.updateListener 回调 | 同左；onDidChangeModelContent 回调 |
| **License** | MIT | MIT（核心完全免费，Pro 扩展付费但 2025-06 已开源更多） | MIT | MIT | MIT | MIT |
| **维护活跃度** | 活跃但单人维护（Mirone/Meo）；v7.22.1（2026-08）；曾有停更传闻但已恢复 | 公司化运营（ueberdosis）；32k star；9.6M 周下载 | 活跃（B3log 组织，国产）；v3.11.1（2026-03） | 基本停更：v1.22.0（2025-02），上次实质性更新 2022 年 | 非常活跃：Marijn 本人维护；@codemirror/view 6.39 beta（2025-12） | 非常活跃：微软官方；0.54.0-dev（2025-09）；VS Code 同步迭代 |

## 1.2 WebView2 ↔ C++ 宿主通信方案（六编辑器通用）

```
宿主 → Web（注入内容/主题/命令）：
  ICoreWebView2::PostWebMessageAsJson(LR"({"cmd":"openFile","content":"..."})")

Web → 宿主（保存/字数统计）：
  window.chrome.webview.postMessage({ type: 'save', content: editor.getValue() })

初始化脚本注入（页面加载前执行）：
  ICoreWebView2::AddScriptToExecuteOnDocumentCreated(
    L"window.chrome.webview.addEventListener('message', e => window.__onHostMsg(e.data));",
    nullptr);

宿主接收：
  ICoreWebView2::add_WebMessageReceived(...)
```

- **打开文件**：`PostWebMessageAsJson` 注入 markdown 字符串 → 编辑器 `setContent()`
- **内容变更**：编辑器 `onChange` 防抖（300-500ms）→ `chrome.webview.postMessage` → 宿主写盘
- **主题同步**：宿主 Post `{"theme":"dark"}` → Web 侧切换 CSS 变量/编辑器主题
- **字数统计**：编辑器侧 `getMarkdown().length` 节流上报

> 来源：[Microsoft Learn - Native/Web Interop](https://learn.microsoft.com/en-us/microsoft-edge/webview2/how-to/communicate-btwn-web-native)

## 1.3 推荐结论

### 首选：CodeMirror 6 + 自研 Live Preview Overlay

**理由：**

1. **中文 IME 最稳**：CM6 在 6.x 重写了整个输入层（`@codemirror/view` 的 input 模块），composition 事件处理是源码编辑器里最成熟的。Markra 编辑器（少数派 2026-07 文章）正是因为 Milkdown/ProseMirror 的中文格式边界 IME 问题，主动从 Milkdown 迁移到 CodeMirror 6 才解决。
2. **体积最小**：~138KB gzip（含 markdown 语法支持），是 Monaco 的 1/7，无任何 worker 文件需要额外管理。
3. **维护最可靠**：Marijn Haverbeke 本人长期维护，CM6 生态（lezer 语法树）成熟，Obsidian、Logseq 均采用。
4. **离线打包最简单**：esbuild/rollup 直接打成单个 JS bundle，无 CDN 依赖、无 worker 配置。
5. **达到 Typora 级体验的路径清晰**：参考 Galley Editor 方案，在 CM6 之上用 `Decoration` + `WidgetType` 做半 WYSIWYG overlay（行内代码/链接/标题实时渲染为富文本样式，编辑时光标回到源码层），比 ProseMirror 的纯 WYSIWYG 方案更可控。

**代价**：需要自行开发 Live Preview overlay 层，预估 2-4 周工作量。但换来的是 IME 稳定性和长期可维护性。

### 次选（快速落地）：Vditor IR 模式

如果希望最快上线、最少自研，Vditor 的 IR 模式开箱即类 Typora 体验，且是国产项目，中文输入为原生场景。代价是 lute.min.js 3.6MB 较大，定制性弱于 CM6。

### 不推荐

- **Milkdown / TipTap**：ProseMirror 底层在行内格式边界的中文 composition 有已确认 bug，且 Milkdown 单人维护风险高。
- **Monaco**：定位是代码 IDE，Markdown 笔记场景过重（2-5MB + 多 worker）。
- **Bytemd**：停更 2 年以上，无未来。

### 关于"双编辑器组合"

**不推荐 WYSIWYG + 源码双编辑器方案**（如 Milkdown/TipTap + CM6）：两套编辑器实例状态同步复杂、ProseMirror 侧 IME bug 引入、切换时光标位置恢复困难、包体积叠加。建议用单编辑器（CM6）+ Live Preview overlay 同时覆盖两种模式。

## 1.4 主要集成风险

| 风险 | 严重程度 | 说明 | 缓解建议 |
|---|---|---|---|
| **Chromium 149+ IME 回归 bug** | 🔴 高 | 2026-06 确认：Chromium 149.0.7827.103 引入回归，首次 IME 输入的 composition 事件被静默抑制，中文标点需按两次才出现。WebView2 Runtime 跟随 Edge 更新，所有 contenteditable 编辑器全部受影响 | ① 锁定 WebView2 Runtime 版本（通过 EBWebView 固定版本分发）；② 关注 Chromium 修复进度；③ 上线前用微软拼音 + QQ 拼音做回归测试 |
| ProseMirror 系中文格式边界光标错位 | 🟡 中 | Milkdown/TipTap 已知问题 | 选 CM6 即可规避 |
| Monaco worker 离线配置 | 🟡 中 | Monaco 运行时需要异步加载多个 worker 文件 | 选 CM6 可规避 |
| Vditor lute.min.js 体积 | 🟡 中 | lute Markdown 引擎 3.6MB | 仅打包 IR 模式所需，mermaid/katex 按需懒加载 |
| WebView2 消息往返延迟 | 🟢 低 | 高频 onChange 可能卡顿 | 编辑器侧 300-500ms 防抖 |
| WebView2 Runtime 版本碎片化 | 🟡 中 | Win11 自带 Runtime 版本不确定 | 用 Evergreen Bootstrapper 分发，或固定 Runtime 版本 |

## 1.5 关键来源

- ProseMirror 中文 IME issue：[discuss.prosemirror.net #8503](https://discuss.prosemirror.net/t/cursor-position-issue-during-chinese-text-composition/8503)
- CodeMirror 6 中文 IME：[discuss.codemirror.net #9741](https://discuss.codemirror.net/t/chinese-ime-punctuation-input-loses-every-other-keypress-requires-2-presses-per-character/9741)
- Chromium 149+ 回归 bug：[博客园报告](https://www.cnblogs.com/ale160/p/20411706/CHROME_IME_BUG_REPORT)
- Markra 从 Milkdown 迁移 CM6：[V2EX #1230382](https://v2ex.com/t/1230382)
- CM6 vs Monaco 体积：[@pyreon/code 基准测试](https://www.npmjs.com/package/@pyreon/code)

---

# 第二部分：Scintilla 嵌入可行性

## 2.1 总体结论：有条件可行

无致命阻塞点，但必须满足 4 个前提：

1. 只用 C 风格 `SendMessage` 消息协议，**不引入** `call/ScintillaCall.cxx`（它才是抛 `Scintilla::Failure` 异常的部分）。
2. Scintilla 核心 + Lexilla 用 `-std=c++17 -fno-exceptions -fno-rtti` 单独编译并静态链接；需要一次验证性构建确认无残留 `throw/try`。
3. 渲染形态是 **Win32 真子窗口**（不是 OpenGL 同表面叠加），EUI-NEO 的自绘控件不能浮在 Scintilla 矩形之上，除非也做成同级原生子窗口。
4. 必须把进程声明为 Per-Monitor V2 DPI 感知，并把 `WM_DPICHANGED`、`WM_SIZE` 转发给 Scintilla。

## 2.2 调研点逐项回答

### 调研点 1：ScintillaWin 子窗口嵌入 GLFW 窗口

**可行。** GLFW 官方提供 `glfwGetWin32Window(GLFWwindow*) → HWND`（since 3.0），使用前需 `#define GLFW_EXPOSE_NATIVE_WIN32` 并 `#include <GLFW/glfw3native.h>`。Scintilla 官方嵌入方式就是 `CreateWindowEx(0, "Scintilla", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPCHILDREN, x,y,w,h, hwndParent, ...)`，父窗口只需是合法 HWND。

**已知问题与对策：**

| 问题 | 说明 | 对策 |
|---|---|---|
| 输入焦点 | Scintilla 是真子 HWND，Win32 消息分发自动把键盘消息投递给拥有焦点的 HWND，无需手动转发 | 用户点 EUI-NEO 区域 vs Scintilla 区域时由 SetFocus 自然解决 |
| 渲染层叠 | ScintillaWin 在自己的子 HWND 里用 GDI 或 Direct2D/DirectWrite 自绘，不写入 GLFW 的 OpenGL framebuffer。Windows 窗口管理器负责合成 | 避免了"OpenGL 纹理和 GDI 在同一表面 Z-order 打架"；代价是 EUI-NEO 自绘 UI 不能盖在 Scintilla 矩形上 |
| DPI | Scintilla 已实现 Per-Monitor DPI 感知，官方明确要求转发 `WM_DPICHANGED` | 进程声明 PerMonitorV2，拦截 WM_DPICHANGED 转发给 Scintilla |
| MinGW-w64 | 曾有"wrap 模式文字不绘制"的 bug，官方已修 | MinGW-w64 是受支持目标 |

> 来源：[GLFW native access](https://www.glfw.org/docs/latest/group__native.html)、[Scintilla 嵌入教程](https://scintilla.sourceforge.io/Steps.html)

### 调研点 2：对 C++ 异常 / RTTI 的依赖

**技术上可行。** Scintilla 官方 Design 文档原文："a conservative subset of C++ is used in Scintilla with **no exception handling, run time type information** or use of the standard C++ library and with limited use of templates."

关键设计原则："**Exceptions should not be thrown over build boundaries as the two sides may be built with different compilers or incompatible exception options.**"——官方明确设计成调用方和被调用方可以用不兼容的异常选项分别编译，这正是 DevDesk `-fno-exceptions` 硬约束的官方背书。

真正用 `throw` 的只有可选 C++ 包装层 `call/ScintillaCall.cxx`（抛 `Scintilla::Failure`）。DevDesk 走 C `SendMessage` API 时根本不链接这个文件，错误用 `SCI_GETSTATUS` 读 `SC_STATUS_FAILURE / SC_STATUS_BADALLOC`。

RTTI：Scintilla 用虚函数分发，不用 `dynamic_cast/typeid`，`-fno-rtti` 预期可过。

**前提条件**：在 CMake 里把 `call/` 目录和 `ScintillaDLL.cxx` 排除，并做一次干净构建验证。

> 来源：[Scintilla Design](https://www.scintilla.org/Design.html)、[Scintilla Code Style](https://www.scintilla.org/SciCoding.html)

### 调研点 3：静态链接与体积

**可静态链接。** Scintilla 5 官方构建说明："`ScintillaDLL.cxx` provides the DllMain... **omit it when producing a static library or linking Scintilla statically**. On Win32, the standard makefiles build a `libscintilla` static library."

Lexilla 官方："Lexilla may be linked directly into an application or built into a static library... then there is no need to load a shared library and `CreateLexer` can be directly called."

**体积估算**：Scintilla 核心 + 全量 Lexilla（约 100 个 lexer）以 `-O3 -s` 静态链接，**exe 增量约 +3 ~ +8 MB**；裁剪 lexer 列表（只保留 Markdown/C++/JSON 等）可压到 **+1 ~ +3 MB**。

Windows 静态链接需要的系统库：`user32 gdi32`；如启用 Direct2D 还需 `d2d1 dwrite windowscodecs`（Win10 自带，不算外部 DLL）。可用 `DISABLE_D2D` 宏禁用 D2D、退回 GDI。

> 来源：[Scintilla 5 Migration](https://www.scintilla.org/Scintilla5Migration.html)、[Lexilla Doc](https://scintilla.org/LexillaDoc.html)

### 调研点 4：Markdown lexer 现成程度

**自带，质量够用。** Lexilla 内置 `LexMarkdown.cxx`，作者 Jon Strait，不是 Notepad++ 外挂的社区语法文件。

支持的高亮元素：H1–H6 标题、粗体/斜体/删除线（含 Pandoc 扩展）、有序/无序列表、inline code、fenced code block、链接、blockquote。近期持续修复非 ASCII 链接字符、emphasis span 识别等问题。

接入方式：`SCI_SETLEXERLANGUAGE(0, "markdown")`，或 Lexilla 5 推荐的 `CreateLexer("markdown")` + `SCI_SETILEXER`。

**局限**：不做 fenced code block 内部嵌套语言高亮，也不做 WYSIWYG 渲染。对"Markdown 源码编辑器 + 行号"的定位够用，但不要指望达到 VSCode 的 Markdown 体验。

> 来源：[Lexilla GitHub - LexMarkdown.cxx](https://github.com/ScintillaOrg/lexilla)

### 调研点 5：License

**HPN 风格，实质等同于 MIT。** 官方 License.txt 原文（覆盖 Lexilla / Scintilla / SciTE 三者）：

> Copyright 1998-2021 by Neil Hodgson. All Rights Reserved. Permission to use, copy, modify, and distribute this software and its documentation for any purpose and without fee is hereby granted, provided that the above copyright notice appear in all copies...

- ✅ 允许闭源商业产品静态链接（单 exe 分发无许可证污染）
- ✅ 不要求开源衍生作品（无 copyleft）
- ✅ 不要求公开修改后的源码
- ⚠️ 唯一义务：在所有分发副本（About 框 / 关于页 / 文档）中保留版权声明 + 许可声明
- Lexilla 与 Scintilla 共用同一份 License

> 来源：[Scintilla License.txt](https://www.scintilla.org/License.txt)

### 调研点 6：其他独立嵌入形态对比

| 方案 | 是否适用 | 说明 |
|---|---|---|
| QScintilla | ❌ | Qt 绑定，项目不用 Qt |
| gtksourceview | ❌ | GTK 绑定，Windows 下不友好 |
| ImGuiColorTextEdit | ⚠️ 理论契合 | 纯 C++、MIT、OpenGL 渲染、无 Win32 子窗口。但 DevDesk 用 EUI-NEO 而非 ImGui，引入等于嵌整个 ImGui；Markdown 不是一等公民 |
| Dear ImGui 自带编辑器 | ❌ | 只是 InputTextMultiline，无语法高亮 |

**结论**：在"成熟 Markdown 语法高亮 + 行号 + 不依赖浏览器内核"的硬约束下，Scintilla 是唯一工业级成熟选择；"纯 OpenGL 渲染、不依赖 Win32 子窗口"的替代品在 Markdown 方向上没有成熟开源实现。

## 2.3 集成路径要点

1. **CMake 组织**：把 `scintilla/src/win32` + `scintilla/src/*` 与 `lexilla/src/*` 编为两个 STATIC library；排除 `win32/ScintillaDLL.cxx` 和 `call/ScintillaCall.cxx`；统一加 `-std=c++17 -fno-exceptions -fno-rtti`；可选 `DISABLE_D2D`。
2. **进程 DPI 清单**：在 exe manifest 声明 `PerMonitorV2`。
3. **创建子窗口**：
   ```c
   HWND parent = glfwGetWin32Window(g_window);
   HWND hSci = CreateWindowExW(0, L"Scintilla", L"",
       WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPCHILDREN,
       x, y, w, h, parent, (HMENU)1001, GetModuleHandleW(NULL), NULL);
   ```
4. **装 lexer**：调用 `LexillaAccess.cxx` 里的 `RegisterLexer()`，然后 `SendMessage(hSci, SCI_SETLEXERLANGUAGE, 0, (LPARAM)"markdown")`。
5. **样式**：`SCI_STYLESETFONT / SCI_STYLESETFORE` 对 `SCE_MARKDOWN_*` 样式 ID 逐个配色；行号用 `SCI_SETMARGINTYPEN(0, SC_MARGIN_NUMBER)`。
6. **消息转发**：在 EUI-NEO 的 Win32 WndProc 里拦截 `WM_SIZE` → `MoveWindow(hSci, ...)`；`WM_DPICHANGED` → 转发；`WM_CLOSE` → 先 `DestroyWindow(hSci)`。
7. **通知回传**：Scintilla 通过 `WM_COMMAND` 把 `SCN_*` 结构塞给父窗口 WndProc，接 `SCN_MODIFIED / SCN_UPDATEUI` 同步 EUI-NEO 状态。
8. **渲染后端**：默认 GDI 最稳；高分屏清晰文字用 `SCI_SETTECHNOLOGY(hSci, SC_TECHNOLOGY_DIRECTWRITE, 0)`。

---

# 第三部分：开源编辑器 UX 借鉴 + MarkText 源码结构导读

## 3.1 编辑范式对比

| 编辑器 | 技术栈 | 默认范式 | 可切换模式 | 关键行为 |
|---|---|---|---|---|
| **MarkText** | Electron + Vue 3 + Muya 引擎 | WYSIWYG 单栏（实时渲染，无分屏） | ① Source Code（CodeMirror 5）② Typewriter（行居中）③ Focus（仅当前段高亮） | 主界面是 contenteditable WYSIWYG；源码模式是独立 CodeMirror 实例，切换时序列化/反序列化 markdown |
| **Apostrophe** | GTK 4 | 源码编辑 + 实时预览面板 | Focus Mode（居中+去饱和其他段落） | 用 pandoc 做 Markdown 后端 |
| **ghostwriter** | Qt 6 | 双栏：左侧纯文本 + 右侧 HTML 实时预览 | ① 全屏无干扰 ② Focus Mode（高亮当前行/句子/段落/三档） | 内置 cmark-gfm，预览异步重渲 |
| **Zettlr** | Electron + Vue 3 + CodeMirror | WYSIWYM（所见即所意）：源码中内联渲染粗体/标题/链接 | v4.0 起可切换"纯语法视图"和"预览模式" | 定位学术写作，不做完整 WYSIWYG |
| **Obsidian** | Electron（自研编辑器） | Live Preview：格式化文本内联显示，光标进入时才露出原始语法 | ① Live Preview ② Source Mode ③ Reading View；可分屏双栏 | Ctrl/Cmd+E 切换编辑/预览 |
| **Typora** | Electron（自研） | 经典 WYSIWYG：输入即渲染，无分屏 | 无源码/预览切换 | 输入 `## ` 后空格，标题立刻渲染 |

### ✅ 可直接采用的设计

- **双模式架构**：以 WYSIWYG 为主，保留"源码模式"作为逃生舱。MarkText 的做法是两套独立编辑器实例（Muya contenteditable + CodeMirror），切换时序列化/反序列化 markdown + 光标位置，而不是试图在同一编辑器里做双视图。
- **Focus Mode 四档**：ghostwriter 提供"当前行 / 当前句子 / 当前段落 / 三行"四档亮度调节，比 MarkText/Apostrophe 的"整段高亮"更细粒度。
- **Obsidian 的"光标进入才显示语法"**：既保持排版干净，又在编辑时暴露语法，比 Typora 的纯 WYSIWYG 更适合技术写作。

## 3.2 打字→渲染延迟的处理策略

| 编辑器 | 渲染策略 | 防抖/节流 | 光标保持 |
|---|---|---|---|
| **MarkText / Muya** | 块级增量渲染：文档是块树，输入只重渲当前块 | RxJS 合并 DOM 事件流后路由到 activeContentBlock | 编辑操作走 ot-json1 + ot-text-unicode，光标由数据层驱动而非 DOM 后修复 |
| **Typora** | 即时块级渲染，输入即重排当前块 | 输入事件同步触发当前块重新解析 | 光标锚定在块内部 offset |
| **Obsidian** | Live Preview 基于自研 CM6 衍生引擎，局部装饰更新 | 输入事件同步驱动 decoration 层 | 光标在编辑时保持，切换时用行号映射恢复 |
| **ghostwriter** | 纯文本编辑 + 独立 HTML 预览（WebEngine） | 预览异步重渲，避免阻塞输入 | 编辑区 QPlainTextEdit，光标天然稳定 |
| **Apostrophe** | 源码编辑 + pandoc 渲染预览 | pandoc 外部进程，预览必然有节流 | 同 ghostwriter |
| **Zettlr** | CodeMirror + 内联 decoration | CM6 viewport 渲染只可视区重渲 | CM6 原生光标管理 |

### ✅ 可直接采用的设计

1. **块级增量渲染是核心**：文档抽象为块树（paragraph / heading / table / code-block…），每次输入只把 op 应用到当前块，在该块的 inline 层做 vdom diff。这是长文档不卡的根本原因。
2. **用 OP（操作转换）而不是全文 diff**：每次输入是一个结构化 op，光标可以随 op 一起传递，不存在"重渲后找光标"的问题。
3. **RxJS 合并输入事件**：`compositionstart/end`（中文输入法）、`beforeinput`、`keydown` 合并成一条流再分发，避免 IME 组合期间重复触发渲染。
4. **预览与编辑解耦**：如果做双栏预览，预览必须异步/节流（ghostwriter 模式），绝不能让重渲阻塞输入主线程。

## 3.3 滚动同步实现方式

| 编辑器 | 架构 | 同步算法 |
|---|---|---|
| MarkText | 单栏 WYSIWYG，不需要双栏同步 | TOC 跳转用 CSS selector `#${slug}` 滚动 |
| ghostwriter | 双栏（编辑器 + Qt WebEngine 预览） | 预览跟随光标，按"当前行位置比例"映射 |
| Zettlr | 三栏（文件树 + CM 编辑 + 预览） | 基于 CodeMirror 行号→预览 DOM 元素映射 |
| Apostrophe | 编辑器 + WebKit 预览 | 按位置比例 |
| Obsidian | 可分屏双栏 | 分屏时两视图同步滚动，基于行号/块位置映射 |

### 两种工程方案

- **方案 A：百分比映射（简单但粗糙）** — `preview.scrollTop = (editor.scrollTop / (editor.scrollHeight - editor.clientHeight)) * (preview.scrollHeight - preview.clientHeight)`。10 行实现，但两边行高不一致时会漂移。
- **方案 B：块/行锚点映射（精确）** — 编辑区每个块有唯一 ID，预览区对应块也带同一 ID；滚动时二分查找"当前可视区最上方的块"，跳到预览区同 ID 块的 `offsetTop`。用 `requestAnimationFrame` 标志位防止双向反馈循环。

### ✅ 可直接采用的设计

- **块级锚点映射**：每个 markdown 块在解析时分配稳定 slug（MarkText 用 `WeakMap<block, uuid>` 缓存）；双栏同步时用"当前视口最上方块的 slug"作为锚点，而不是 scrollTop 百分比。
- **用 rAF 标志位防反馈循环**：滚动 A 区→设置同步锁→操作 B 区→下一帧解锁。

## 3.4 表格编辑交互细节

| 编辑器 | 增删行列 | 列宽拖拽 | 键盘导航 | 单元格内多行 |
|---|---|---|---|---|
| **MarkText** | 点击表格后浮动工具栏：tableChessboard（选格）、tableColumnToolbar（列工具）、tableDragBar（拖拽条）、tableRowColumMenu（行列菜单）；Ctrl+Shift+T 插入表格，Ctrl+Enter 下方插行 | 有 | Tab / Shift+Tab 切格；Enter 换行 | 支持 |
| **Typora** | 聚焦表格后弹出工具栏；右键菜单；Ctrl+Enter 下方插行；末格 Tab 自动新增行；Ctrl+Shift+Backspace 删当前行 | 有 | Tab 下一格、Shift+Tab 上一格 | Shift+Enter 单元格内换行 |
| **Obsidian** | 表格在 Live Preview 下类似 Notion：点击表格出现工具条 | 有 | Tab / Shift+Tab | 支持 |
| **ghostwriter** | 纯文本编辑，靠 `|` 管道符 | 无 | 文本导航 | 文本换行 |
| **Zettlr** | 类似 CodeMirror 表格插件行为 | 部分 | Tab 导航 | 文本 |
| **Apostrophe** | 工具栏插入表格 | 弱 | 文本 | 文本 |

### ✅ 可直接采用的设计

1. **悬浮工具栏 + 列宽拖拽 + 棋盘选格三件套**：MarkText 把表格 UI 拆成 4 个独立 float 组件，都继承 `baseFloat`/`baseScrollFloat`，用 `@floating-ui/dom` 定位。每个工具只在需要时出现，不污染主编辑区。
2. **键盘导航约定**（跨编辑器共识）：`Tab` 下一格里、`Shift+Tab` 上一格、末格 Tab 自动加新行、`Ctrl/Cmd+Enter` 下方插行、`Ctrl+Shift+Backspace` 删行。这套键位已经是用户肌肉记忆，照抄即可。
3. **列宽拖拽**：用 `<col>` 元素 + 拖拽手柄，而不是改每个 `<td>` 的宽度，避免布局重排。

## 3.5 大纲面板与跳转

| 编辑器 | 面板位置 | 跳转行为 | 当前位置高亮 | 层级缩进 |
|---|---|---|---|---|
| **MarkText** | 左侧 sidebar 第三项（TOC 面板） | 点击 emit scroll-to-header 事件带 slug；WYSIWYG 用 `#${slug}` CSS selector 滚动；源码模式转行号再 scroll | 源码模式下有 | Element Plus el-tree，indent=10 |
| **Typora** | 右侧 sidebar，Ctrl+Shift+1 切换 | 点击跳转 | 有 | 缩进 |
| **Obsidian** | 右侧边栏 Outline 核心插件 | 点击跳转；**支持在大纲里拖拽标题来重排文档章节** | 有（滚动时高亮） | 按标题层级缩进 |
| **ghostwriter** | Outline 窗口（可独立） | 点击跳转 | 有 | 缩进 |
| **Zettlr** | 左侧或右侧边栏 | 点击跳转 | 有 | 缩进 |

### ✅ 可直接采用的设计

1. **slug 稳定化**：MarkText 给每个 heading block 用 `WeakMap<block, string>` 缓存 stable slug，内容编辑后 slug 不变，TOC 面板不会因为用户改了标题文字就丢失展开状态和滚动位置。
2. **TOC 与编辑器解耦**：用事件总线（`bus.emit('scroll-to-header', slug)`），TOC 组件不直接操作 DOM，editor 负责滚动——WYSIWYG 和源码两种模式可以各自监听同一事件并做不同处理。
3. **滚动时高亮当前标题**：用 `IntersectionObserver` 监听所有 heading DOM，root 设为编辑器滚动容器，视口内最上方的 heading 即为当前项。
4. **（加分项）大纲拖拽重排**：Obsidian Outline 允许在大纲里拖标题直接改文档顺序。

## 3.6 MarkText 的 WYSIWYG 源码实现结构

> ⚠️ MarkText 已在 2024–2025 年重构为 **pnpm monorepo**，老教程里的 `src/editor/`、`src/codeMirror/` 目录已不存在。

### 顶层 monorepo 结构

```
marktext/  (develop 分支)
├── packages/
│   ├── desktop/       ← Electron 应用壳
│   │   └── src/
│   │       ├── main/          主进程（IO、窗口、自动更新）
│   │       ├── preload/      预加载脚本
│   │       ├── renderer/     Vue 3 渲染进程（编辑器 UI、Pinia store）
│   │       │   └── src/
│   │       │       ├── components/    Vue 组件
│   │       │       ├── store/         Pinia（editor.ts / preferences.ts）
│   │       │       ├── pages/         路由页
│   │       │       └── codeMirror/   源码模式的 CM5 封装
│   │       └── shared/        跨进程类型与 IPC 契约
│   ├── muya/          ← TypeScript 重写的编辑器引擎（@muyajs/core，现役）
│   │   └── src/
│   │       ├── muya.ts            公开入口，Muya 类
│   │       ├── editor/index.ts    Editor 运行时
│   │       ├── block/             块树
│   │       ├── state/             JSONState + markdown 序列化
│   │       ├── inlineRenderer/    行内渲染（lexer + snabbdom）
│   │       └── ui/                浮动工具（工具栏/表格/图片…）
│   ├── muyajs/        ← 老版 JS 引擎（正在退役）
│   └── website/       ← 官网 + 开发文档（ARCHITECTURE.md / PERFORMANCE.md）
```

技术栈：Electron 42 + Vue 3 + Pinia 3 + Element Plus；编辑器引擎 `@muyajs/core` 基于 **ot-json1 + ot-text-unicode + snabbdom + marked@16 + rxjs**。

### 块树（Block Tree）—— 核心数据结构

所有块都继承自 `TreeNode → Parent → (Content | Format)`，位于 `packages/muya/src/block/base/`：

- `Parent`：持有 `children` 链表（`LinkedList`）和 `attachments` 列表
- `Content`：叶子节点，持有实际文本
- `Format`：扩展 `Content`，加行内格式处理

具体块类型：`src/block/commonMark/`（paragraph / heading / blockquote / code-block / hr）、`src/block/gfm/table/`（table / row / cell）、`src/block/gfm/taskList*`、`src/block/extra/`（math / footnote / frontmatter）、`src/block/scrollPage/`（根节点 ScrollPage）。

注册机制：所有块必须在 `src/block/index.ts::registerBlocks()` 注册，`Editor.init()` 启动时调用。

### 块渲染循环与增量更新

```
DOM 事件 (click/input/keydown/composition)
   ↓  RxJS 合并
Editor 路由到 activeContentBlock 的 handler
   ↓
构造 ot-json1 操作 (ops)
   ↓
Editor.updateContents(ops, selection, source)
   ↓
ot-json1.apply 到 JSONState（数据源）
   ↓
hand-rolled pick/drop walk：
   ├─ block.replaceWith / container.insertBefore
   └─ otText.type.apply()  ← inline 文本编辑是嵌套的 ot-text-unicode op
   ↓
snabbdom diff 当前块的 inline vdom → 只 patch 当前块 DOM
   ↓
cursor 随 op 一起传入，不存在"重渲后找回光标"
```

关键设计：
- **JSONState**（`src/state/index.ts`）是文档唯一真相源，暴露 ot-json1 的 `invert/compose/transform`，架构上预留了协同编辑能力。
- **Markdown 往返**：`markdownToState.ts`（marked 解析）、`stateToMarkdown.ts`（序列化回 markdown）。
- **inline 渲染**：`src/inlineRenderer/` 用自定义 lexer/rules 把行内内容 tokenize，再用 snabbdom 渲染成 vdom。

### 光标管理

- `Editor` 持有 `Selection` 模块和 `activeContentBlock`（当前聚焦的叶子块）。
- 光标位置是结构化的（`{anchor: {line, ch}, focus: {line, ch}}`），不是浏览器原生 Selection。
- 源码模式切换时，`sourceCode.vue` 用 `adjustCursor()` 把 CodeMirror 的 `{line, ch}` 光标转成 muya 的 index cursor，反之亦然。
- 切换 tab 或外部文件变化时，快照 scrollTop，在 `setValue()` 后 `nextTick` + `requestAnimationFrame` 两次恢复。

### 与 CodeMirror 的关系

| 项 | 答案 |
|---|---|
| WYSIWYG 模式依赖 CodeMirror 吗？ | **不依赖**。WYSIWYG 是 Muya 自己的 contenteditable 渲染，snabbdom 直接 patch DOM |
| 源码模式用哪个 CodeMirror？ | **CodeMirror 5**。使用 `cm.on('cursorActivity')`、`cm.setOption()`、`viewportMargin: Infinity` 等 CM5 API |
| 两种模式如何切换？ | **两套独立实例**。切到源码模式时从 Muya 导出 markdown 文本传给 CodeMirror `setValue()`；切回时把 CodeMirror 当前文本 + 光标回灌给 Muya 重新解析 |
| 官方对源码模式的定位 | ARCHITECTURE.md 原话："MarkText's source-code editor is provided by CodeMirror and **not well optimized nor feature rich**. It's not part of Muya" |

### 关键文件路径速查表

| 模块 | 文件路径 | 作用 |
|---|---|---|
| 引擎架构指南 | `packages/muya/CLAUDE.md` | 块树、Editor、OT、inline renderer、UI 插件 |
| Muya 入口 | `packages/muya/src/muya.ts` | Muya 类，use(Plugin) 注册插件 |
| Editor 运行时 | `packages/muya/src/editor/index.ts` | JSONState / InlineRenderer / Selection / updateContents |
| 块基类 | `packages/muya/src/block/base/{parent,content,format}.ts` | 块树基类 |
| 根滚动容器 | `packages/muya/src/block/scrollPage/index.ts` | ScrollPage，块树根节点 |
| 表格块 | `packages/muya/src/block/gfm/table/{table,row,cell}.ts` | GFM 表格数据结构 |
| 文档状态 | `packages/muya/src/state/index.ts` | JSONState（ot-json1） |
| TOC 提取 | `packages/muya/src/state/getTOC.ts` | 遍历 heading block，生成 slug |
| 行内渲染 | `packages/muya/src/inlineRenderer/` | lexer/rules + snabbdom vdom |
| 表格 UI 组件 | `packages/muya/src/ui/{tableChessboard,tableColumnToolbar,tableDragBar,tableRowColumMenu}/` | 表格四件套 |
| WYSIWYG 编辑组件 | `packages/desktop/src/renderer/src/components/editorWithTabs/editor.vue` | Vue 侧 Muya 宿主 |
| 源码模式组件 | `packages/desktop/src/renderer/src/components/editorWithTabs/sourceCode.vue` | CM5 封装 + 与 Muya 同步 |
| 大纲面板 | `packages/desktop/src/renderer/src/components/sideBar/toc.vue` | el-tree 渲染 TOC |

### 渲染流程一句话总结

> MarkText 把 Markdown 解析成一棵块树，用 ot-json1 记录编辑操作；用户输入时，事件经 RxJS 合并后路由到当前活跃块，新的 op 同时更新 JSONState 和块树 DOM（snabbdom 只 diff 当前块的 inline 层）；光标作为 op 的一部分一起传递，从不"事后找回"。源码模式是外挂的 CodeMirror 5，与 WYSIWYG 完全解耦，靠 markdown 文本序列化互通。

---

# 第四部分：自研框架补富文本的成本评估

## 4.1 六框架对比总表

| 框架 | 段内多样式 Run 的 API 形态 | 编辑组件（光标/选区/IME） | 静态链接体积增量 | License |
|---|---|---|---|---|
| **Dear ImGui** | 无原生 run 概念。TextColored() 本质是 PushStyleColor + Text() 的糖，整段一个色。段内多色需手动拆成多次 TextUnformatted() 或借助第三方：ImGuiColorTextEdit、imgui_md、UntitledImGuiTextUtils。1.92 起有 ImHTML 实验性方案 | InputText / InputTextMultiline，内置光标、选区、undo/redo。**IME 支持薄弱**：组合区不下划线高亮，候选窗定位需自行处理；中文字体需预加载 glyph ranges | ~200–500 KB | MIT |
| **RmlUi** | 原生 HTML/CSS 模型。`<span style="color:red">`、`<em>`、`<strong>` 等 inline 元素天然支持段内混排；样式由 RML + RCSS 驱动，可通过 C++ API 动态修改 inline style。6 个框架中最成熟的富文本模型 | 自带 `<input type="text">`、`<textarea>`，有 GetSelection/SetSelection API。**IME 有专门文档页**：通过 SetImeContext() 接入，组合文本下划线高亮、ActivateKeyboard 上报光标像素位置 | ~1–3 MB | MIT |
| **Nuklear** | 无段内多色。nk_label() / nk_text() 每次调用绑定一个 nk_style_text，要混排必须手动拆成多次调用并手动推进 x 坐标。无 span/run 抽象 | nk_edit_string() / nk_edit_buffer()，内置 undo/redo。**无 IME 组合区支持**，依赖后端把文本事件喂进来 | ~100–300 KB | MIT / Public Domain 双许可 |
| **NanoGUI** | 无段内多色。底层 NanoVG 的 nvgText() 每次调用一个 NVGcolor + font handle；Label 控件绑定单一 font/color。要混排需自行在 draw() 里多次调 nvgFillColor + nvgText | TextBox 控件，单行、功能简单。无多行编辑器，无 IME 组合区处理 | ~500 KB–1 MB | BSD-style（BSD 3-Clause 等效） |
| **Slint** | 1.16（2026-04）才引入富文本：新增 StyledText 元素 + styled-text 类型 + @markdown(...) 宏，支持粗体/斜体/下划线/颜色/超链接/列表。此前 Text 元素只有单一样式，段内混排需拆成多个 Text 元素用 HorizontalLayout 拼 | 内置 LineEdit / TextEdit，光标、选区、IME 均由运行时处理 | RAM <300 KB；桌面可执行文件 ~6–15 MB（Rust 运行时 + C++ binding） | **GPLv3 / Royalty-Free / 商业付费** 三许可。闭源商用必须买商业 license 或用 Royalty-Free（仅限桌面/移动/Web，不允许嵌入式） |
| **Flutter 文本层** | 最成熟的 run 模型：TextSpan / InlineSpan 树形结构，Text.rich() 接收一棵 span 树，每个 TextSpan 自带 TextStyle，可嵌套子 span，样式继承向下传递；WidgetSpan 支持段内嵌入图片/控件。底层 SkParagraph 把段落按"同 font+script+direction"切成 text runs，再用 HarfBuzz 做字形 shaping、ICU 做行断点分词 | EditableText + RenderEditable：完整光标闪烁、选区（base/extent）、composing range（IME 组合区）、通过 TextInput platform channel 与系统 IME 通信 | flutter_windows.dll ~45 MB，最小桌面 release 包 ~20–30 MB。完全不适合嵌入单 exe | BSD 3-Clause |

## 4.2 "多色 run 文本"的通用数据结构模式

业界收敛出两种范式：

**范式 A — 树状 span（Flutter / RmlUi）**
```
TextBlock
 ├─ TextSpan { text: "Hello", style: default }
 │    ├─ TextSpan { text: " world", style: {italic, color: red} }
 │    └─ TextSpan { text: "!", style: {bold, fontSize: 20} }
```
适合富文档/Markdown 渲染，样式可继承、可嵌套。

**范式 B — 扁平 run 数组（Scintilla / 编辑器内部）**
```cpp
struct TextRun {
    int start_offset;   // UTF-8 字节偏移
    int end_offset;
    uint32_t style;     // 位掩码或 style index
};
struct TextBlock {
    std::string text;
    std::vector<TextRun> runs;  // 按 start_offset 排序，互不重叠
};
```
适合代码编辑器/语法高亮，O(log n) 二分查找某偏移属于哪个 run。

**EUI-NEO 建议用范式 B**——语法高亮场景下 run 由 lexer 按 token 产出，扁平数组比树状更简单、缓存友好，且与 Scintilla 的 style 模型一致。

## 4.3 最小改造路径 — 层次化模块分解

从"FreeType 单段单色"到"多色 run + 基础编辑光标"，需要 6 层：

| 层 | 模块 | 职责 | 复杂度 | 工期 |
|---|---|---|---|---|
| **L1 数据结构** | TextRun / TextBlock | 扁平 run 数组，UTF-8 偏移定位，run 合并/分裂 | 低 | ~3 天 |
| **L2 文本布局** | Run 级 FreeType 布局 | 每个 run 独立 FT_Set_Pixel_Sizes + FT_Load_Glyph，产出字形 quad 列表；按可用宽度折行（word wrap），CJK 按字符折行 | 中高 | ~3–4 周 |
| **L3 光标映射** | offset ↔ pixel 双向映射 | 布局完成后缓存每行每字符的 x 坐标表；点击像素 → 最近字符偏移（二分）；光标偏移 → 像素 x | 中 | ~1–2 周 |
| **L4 选区绘制** | 选区矩形 | 起始/结束偏移，跨多行时逐行画半透明矩形 | 低 | ~3 天 |
| **L5 键盘导航** | 光标移动/选词 | 左右箭头按字符（CJK 按 grapheme cluster），上下箭头跨行，Shift+方向扩展选区，双击选词，剪贴板（Win32 OpenClipboard） | 中高 | ~2–3 周 |
| **L6 IME 集成** | Win32 IMM/TSF | ImmSetCompositionWindow 定位候选窗；WM_IME_* 消息处理；组合文本下划线高亮；composition 区间用临时 run 表达 | 高 | ~3–5 周 |

> **不含**：undo/redo、语法高亮 lexer、滚动条、等宽字体度量、BiDi（阿拉伯/希伯来混排）。这些是第二阶段。

## 4.4 工作量量级判断

### 结论：中—高（3–6 人月）

| 方案 | 人月区间 | 假设 |
|---|---|---|
| **自研（EUI-NEO 层补齐）** | **3–6 人月** | 1 名熟练 C++ 图形程序员；已有 FreeType 字形渲染管线和 OpenGL quad 批渲染；Windows 单平台；目标是"语法高亮 + 基础光标/选区/中文 IME"，不含 undo/redo 和高级编辑功能 |
| 嵌入 Scintilla（仅编辑器控件） | 1–2 人月 | 需要把 Scintilla 的 Win32 原生渲染接入 EUI-NEO 的 GLFW/OpenGL 窗口 |
| 嵌入 WebView2（Edge Chromium） | 2–3 周（JS 侧）+ 通信桥接 | 必须用 WebView2 的进程模型 |

**3–6 人月的拆解依据：**
- L1+L4（数据结构 + 选区）：~1 周，极低风险
- L2（run 布局 + 折行）：~3–4 周，**主要风险点**——CJK 折行、混合字号 baseline 对齐、字形 advance 宽度缓存
- L3+L5（光标映射 + 键盘导航）：~2–3 周，中等风险
- L6（Win32 IMM）：~3–5 周，**第二大风险点**——GLFW 已经吃掉了窗口消息循环，需要在 GLFW 之外拦截 `WM_IME_*`，或者改用 raw Win32 WndProc 桥接；组合区下划线、候选窗跟随、TSF vs IMM 选择都是坑

## 4.5 同类框架 API 设计参照推荐

### 首选：Flutter 的 TextSpan / InlineSpan 树模型

**理由：**
1. **样式继承语义清晰**：父 span 的 TextStyle 未覆盖的字段自动向下继承，避免重复声明；语法高亮时"默认色 + token 覆盖"天然契合。
2. **TextRange（base/extent）比 [start, end] 更通用**：base 是锚点、extent 是浮动端，天然支持从右往左选区、Shift 方向键扩展选区。
3. **composing range 与 selection 并列**：`TextEditingValue = { text, selection, composing }`，IME 组合区就是第三个 range，不需要特殊处理。

### 次选：Scintilla 的 Style 位掩码模型

**理由：**
1. 扁平 `vector<StyleRange>` + style index 表，比树状更适合语法高亮（lexer 产出 token 流，直接映射成 run）。
2. Scintilla 用 `SCI_STYLESETFORE` 等注册 style 槽位，run 里只存 `int styleIndex`，数据量极小。
3. Scintilla 的 `Document` + `Editor` + `ViewStyle` 三层分离（数据/交互/渲染）值得照搬。

> **建议**：API 层用 Flutter TextSpan 的树语义（给业务代码用），内部存储用 Scintilla 的扁平 run 数组（给渲染层用）。两者转换在 layout 阶段做。

## 4.6 自研 vs 嵌入路线决策建议

| 路线 | 可行性 | 体积 | 风险 | License 风险 | 结论 |
|---|---|---|---|---|---|
| **自研补齐 EUI-NEO** | ✅ 与现有 OpenGL/GLFW/FreeType 架构一致 | +<500 KB | 中（L2 布局 + L6 IME 是深坑） | 无 | 长期投资方向 |
| 嵌入 Scintilla | ⚠️ Win32 原生控件，与 OpenGL 渲染管线冲突 | +1–2 MB | 高（渲染管线割裂） | 无 | 备选降级方案 |
| 嵌入 WebView2 | ⚠️ 打破"单 exe + 无浏览器内核"硬约束（取决于是否接受系统 Runtime） | Runtime 依赖系统 | 中高（进程模型、消息桥） | 无 | 短期首选（见第一部分） |
| 嵌入 RmlUi | ⚠️ 完整 UI 框架，嵌入等于替换 EUI-NEO | +1–3 MB | 中（架构重写） | 无 | 不推荐 |
| 嵌入 Slint | ❌ GPLv3 对闭源商用不友好；Rust 运行时与 MinGW-w64 融合成本高 | +6–15 MB | 高 | **高** | 排除 |

### 最终建议

**短期（0-3 个月）**：采用 WebView2 + CodeMirror 6 路线，快速达到 Typora 级体验。理由：中文 IME 最稳、体积最小、达到目标体验的路径清晰。首要工程风险是 Chromium 149+ 的中文 IME 回归 bug，需通过锁定 WebView2 Runtime 版本来规避。

**中长期（3-12 个月）**：启动 EUI-NEO 富文本能力自研（3-6 人月），作为框架能力沉淀。理由：
1. EUI-NEO 已经是 OpenGL/GLFW/FreeType 栈，run 级渲染只是现有字形管线的"多次调用 + 宽度累加"。
2. 硬约束（单 exe、禁用异常/RTTI、MinGW-w64）天然排除了 Slint（Rust）、Flutter（45 MB dll）。
3. 3-6 人月的投入，换来的是框架能力的沉淀（后续做 Markdown 预览、日志着色、聊天消息气泡都受益）。
4. **L6 IME 是最大不确定性**，建议先用 1 周做 Win32 IMM 最小验证（在现有 GLFW 窗口里跑通"输入拼音→候选窗→提交汉字→下划线消失"），再决定是走 IMM 还是 TSF。

**降级方案**：如果 WebView2 路线因 Runtime 版本或公司政策不可用，采用 Scintilla 子窗口嵌入（有条件可行，见第二部分），获得源码+高亮+行号的基础编辑体验，但放弃 WYSIWYG。

---

## 附录：关键来源索引

### 第一部分
- [Microsoft Learn - WebView2 Native/Web Interop](https://learn.microsoft.com/en-us/microsoft-edge/webview2/how-to/communicate-btwn-web-native)
- [ProseMirror 中文 IME issue #8503](https://discuss.prosemirror.net/t/cursor-position-issue-during-chinese-text-composition/8503)
- [CodeMirror 6 中文 IME #9741](https://discuss.codemirror.net/t/chinese-ime-punctuation-input-loses-every-other-keypress-requires-2-presses-per-character/9741)
- [Chromium 149+ IME 回归 bug 报告](https://www.cnblogs.com/ale160/p/20411706/CHROME_IME_BUG_REPORT)
- [Markra 从 Milkdown 迁移 CM6 - V2EX](https://v2ex.com/t/1230382)
- [Milkdown LICENSE](https://github.com/Milkdown/milkdown/blob/main/LICENSE)
- [TipTap LICENSE](https://github.com/ueberdosis/tiptap/blob/main/LICENSE.md)
- [Vditor LICENSE](https://github.com/Vanessa219/vditor/blob/master/LICENSE)
- [Monaco LICENSE](https://github.com/microsoft/monaco-editor/blob/main/LICENSE.md)

### 第二部分
- [Scintilla Design](https://www.scintilla.org/Design.html)
- [Scintilla Code Style](https://www.scintilla.org/SciCoding.html)
- [Scintilla 5 Migration](https://www.scintilla.org/Scintilla5Migration.html)
- [Lexilla Doc](https://scintilla.org/LexillaDoc.html)
- [Scintilla License.txt](https://www.scintilla.org/License.txt)
- [GLFW native access](https://www.glfw.org/docs/latest/group__native.html)
- [Scintilla 嵌入教程](https://scintilla.sourceforge.io/Steps.html)
- [Lexilla GitHub - LexMarkdown.cxx](https://github.com/ScintillaOrg/lexilla)
- [ImGuiColorTextEdit](https://github.com/BalazsJako/ImGuiColorTextEdit)

### 第三部分
- [MarkText GitHub](https://github.com/marktext/marktext)（develop 分支，pnpm monorepo）
- [MarkText ARCHITECTURE.md](https://github.com/marktext/marktext/blob/develop/packages/website/content/docs/dev/ARCHITECTURE.md)
- [Muya 引擎 CLAUDE.md](https://github.com/marktext/marktext/blob/develop/packages/muya/CLAUDE.md)

### 第四部分
- [Dear ImGui API / FAQ](https://pixtur.github.io/mkdocs-for-imgui/site/api-imgui/ImGui--Dear-ImGui-end-user/)
- [RmlUi IME 文档](https://mikke89.github.io/RmlUiDoc/pages/cpp_manual/ime.html)
- [Nuklear 源码](https://immediate-mode-ui.github.io/Nuklear/)
- [NanoGUI License](https://nanogui.readthedocs.io/en/latest/contributing.html)
- [Slint 1.16 富文本发布](https://www.slint.dev/blog/slint-1.16-released)
- [Slint License](https://slint.dev/community)
- [Flutter TextSpan API](https://api.flutter.dev/flutter/widgets/Text-class.html)
- [Flutter EditableText](https://api.flutter.dev/flutter/widgets/EditableText-class.html)
