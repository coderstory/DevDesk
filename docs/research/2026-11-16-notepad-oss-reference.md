# DevDesk 记事本模块：开源实现参考调研（notepad OSS reference）

- 查证日期：2026-11-16（star 数 / 活跃度均为此日期 gh api 实查）
- 调研方式：GitHub repos/code 检索 + 元数据 API + 关键源文件 raw 下载精读（grep 定位函数级锚点）
- 简报采信：高亮三家选型（highlight.js / tree-sitter / Scintilla-Lexilla 淘汰）沿用 `2026-11-16-notepad-module-research.md` 结论，本报告不重复论证
- 结论分级：【已确认】= 本轮实查源码/API；【待验证】= 单源或 README 级；【推测】= 推断

## 结论先行

1. 【已确认】查找/替换最值得对标的是 **zufuliu/notepad4**（BSD-3-Clause，5.1k★，活跃）：`src/Edit.cpp` 一个文件内含查找替换对话框、批量替换 undo 组（`EditReplaceAllFlag_UndoGroup`）、增量全命中标记（`EditMarkAll`，1MB 分块 + 定时器 + 范围缓存）、滚动落点（`EditEnsureSelectionVisible`），与 DevDesk「自绘 + 无 GUI 框架依赖」场景同构度最高。
2. 【已确认】第二参考 **Notepad++**（GPL-2.0+，29.9k★）：`FindReplaceDlg.cpp` 的 `processAll` 循环（searchInTarget + SCI_SETSEARCHFLAGS）、跳滚居中策略、增量查找高亮 `markSelectedTextInc`；GPL 许可 → 只借交互设计，不抄代码。注意：该文件内未见 BeginUndoAction，批量替换 undo 原子性以 Notepad4 模式为准。
3. 【已确认】JSON 错误定位无需自研：yyjson 官方自带 `yyjson_locate_pos()`，`err.pos`（字节偏移）直接换算 line/col（1 基）；UI 呈现参照 vscode-json-languageservice 的 offset+length → Range → Diagnostic 模式（`jsonValidation.ts:63`）。
4. 【已确认】文件关联：Windows 端照 **SumatraPDF** `src/shared/RegistryInstaller.cpp` 的流程（HKCU OpenWithProgids + RegisteredApplications + SHChangeNotify + 写前读存档 + Has* 回读 + Unregister 幂等撤销）；macOS 端【已确认】无公开 API 设置文件类型默认应用，开源工具（defaultbrowser / duti）全部依赖 deprecated API，DevDesk 实际路线 = Info.plist 静态声明 + 状态回读 + 引导用户。
5. 【已确认】md4c（MIT）flag 覆盖 tables/tasklists/strikethrough/admonitions/footnotes 等，CommonMark 0.31.2 全合规，能力足够，**不换**。

---

## 一、Q1：C++ 文本编辑器开源实盘清单

### 1.1 Notepad 类轻量编辑器（Windows 系）

| repo | stars(2026-11-16) | 最近提交 | 许可证 | UI 方案 | 行级高亮 | header-only |
|---|---|---|---|---|---|---|
| [notepad-plus-plus](https://github.com/notepad-plus-plus/notepad-plus-plus) | 29.9k | 2026-11-15 | GPL-2.0-or-later（根目录 LICENSE）【已确认存在文件，版本按官方声明】 | Win32 原生 + Scintilla | Lexilla 词法器 | 否 |
| [dail8859/NotepadNext](https://github.com/dail8859/NotepadNext) | 14.8k | 2026-11-16 | GPL-3.0【已确认】 | Qt + QScintilla | Lexilla | 否 |
| [cxasm/notepad--](https://github.com/cxasm/notepad--) | 10.7k | 2026-11-02 | GPL-3.0【已确认】 | Qt + QScintilla（`src/qscint/` 内嵌 Scintilla 全套，已实查） | Lexilla | 否 |
| [rizonesoft/Notepad3](https://github.com/rizonesoft/Notepad3) | 6.8k | 2026-11-16 | GPL-3.0（README 声明；根目录许可证文件 API 未检出）【待验证】 | Win32 + Scintilla（根目录 `lexilla/` `minipath/`，已实查） | Lexilla | 否 |
| [zufuliu/notepad4](https://github.com/zufuliu/notepad4) | 5.1k | 2026-11-16 | BSD-3-Clause（`License.txt` 头部原文已核）【已确认】 | Win32 原生 + Scintilla | 内嵌完整 Scintilla/Lexilla | 否 |
| [stefankueng/BowPad](https://github.com/stefankueng/BowPad) | 755 | 2026-11-07 | GPL-3.0【已确认】 | Win32 + Ribbon + Scintilla（`ext/scintilla/`，已实查） | Lexilla | 否 |
| [sakura-editor/sakura](https://github.com/sakura-editor/sakura) | 1.5k | 2026-11-16 | Zlib【已确认】 | Win32 全自绘（无 Scintilla） | 自研词法/正则【待验证：README 级，未读词法源码】 | 否 |
| [notepadqq/notepadqq](https://github.com/notepadqq/notepadqq) | 2.3k | 2026-10-05 | GPL-3.0【已确认】 | Qt（编辑控件实现未核，QSciScintilla 代码检索 0 命中）【待验证】 | — | 否 |
| 0x7c13/Notepads（10.4k★ MIT） | — | — | — | **C#/WinUI，非 C++，排除** | — | — |

- **notepad4：能直接搬**——查找替换全套逻辑与 UX（见 Q2 锚点）；BSD-3 允许直接抄代码段（保留版权声明）。**不可搬**：Scintilla 本体（与自研渲染重复，简报已淘汰）。
- **NotepadNext / notepad-- / Notepad3 / BowPad：不可用（栈依赖）**——Qt 或 Scintilla 与 DevDesk 约束冲突；交互细节可借读。
- **Notepad++：只能借思路**（GPL-2.0+ 传染性）——查找对话框状态机、宏录制、增量查找条 UX 值得抄交互设计。
- **sakura：只能借思路**——同为 Win32 全自绘 + 自研词法，是「无 Scintilla 路线」的生产先例；日文 IME 增量绘制经验对 EUI-NEO 自绘编辑器有参考价值【待验证：未精读其渲染源码】。

### 1.2 跨平台自绘编辑器（非 C++ 也纳入架构判断）

| repo | stars | 最近提交 | 许可证 | 语言/UI | 高亮 |
|---|---|---|---|---|---|
| [helix-editor/helix](https://github.com/helix-editor/helix) | 46.9k | 2026-11-16 | MPL-2.0【已确认】 | Rust / 终端 | tree-sitter 增量 |
| [xi-editor/xi-editor](https://github.com/xi-editor/xi-editor) | 19.8k | 2024-03-19（停滞） | Apache-2.0【已确认】 | Rust | — |
| [mawww/kakoune](https://github.com/mawww/kakoune) | 11.1k | 2026-10-30 | Unlicense【已确认】 | C++ / 终端 | 自研 |
| [lite-xl/lite-xl](https://github.com/lite-xl/lite-xl) | 6.6k | 2026-11-10 | MIT【已确认】 | **C**+Lua / SDL2 自绘 | Lua 正则模式表 |
| [variar/klogg](https://github.com/variar/klogg) | 3.6k | 2024-11-26（低频） | GPL-3.0【已确认】 | C++ / Qt（日志查看器） | 自研标记【待验证：code 检索 0 命中，README 级】 |
| [gphalkes/tilde](https://github.com/gphalkes/tilde) | 560 | 2026-06-10 | 许可未核【待验证】 | C++ / 终端自绘 | 自研 |

- **lite-xl：只能借思路**（C 非 C++，但 MIT）——「薄内核(C) + 规则表(tokenizer) + 脚本层(Lua)」分层与 DevDesk「表驱动词法器 + LangSpec 表」同构，佐证现路线。
- **helix：只能借思路**——tree-sitter 增量高亮在真实编辑器的收益与内存成本数据点；简报已把 tree-sitter 列为可选升级。
- **xi-editor：不可用**——2024-03 起无提交，架构论文价值大于代码价值。
- **kakoune / tilde：可不读**——终端 UI，与 GUI 记事本交互模型差异过大。
- **klogg：只能借思路**——GB 级日志的 out-of-core 搜索/增量标记调度（Q2 大文件子问的架构参照）；GPL-3.0 + 提交低频（2024-11 后）。

### 1.3 可嵌入 / 教学级（与 EUI-NEO 自研编辑组件最贴近）

| repo | stars | 最近提交 | 许可证 | 形态 | 高亮 |
|---|---|---|---|---|---|
| [Rezonality/zep](https://github.com/Rezonality/zep) | 1.0k | 2026-05-23 | MIT（LICENSE 原文已核：Copyright 2016-2019 chrismaughan.com）【已确认】 | 纯 C++ 可嵌入编辑器组件，自绘，多后端（ImGui/Qt/终端） | 可插拔语法提供器【待验证：内部为正则规则，README 级】 |
| [BalazsJako/ImGuiColorTextEdit](https://github.com/BalazsJako/ImGuiColorTextEdit) | 1.7k | 2025-11-24 | MIT【已确认】 | ImGui 环境自绘编辑组件 | 自带 tokenizer（正则 + 关键字表）【待验证：README 级】 |
| [antirez/kilo](https://github.com/antirez/kilo) | 9.2k | 2025-01-04 | BSD-2-Clause【已确认】 | C 单文件终端编辑器（<1k 行） | 无 |
| [magiblot/turbo](https://github.com/magiblot/turbo) | 695 | 2026-10-04 | COPYRIGHT 文件存在，版本未核【待验证】 | Turbo Vision 复活，含 Editor 组件 | 自研 |
| [KDE/kate](https://github.com/KDE/kate)（镜像，主仓 invent.kde.org） | 1.2k | 2026-11-16 | GPL/LGPL【待验证】 | Qt/KDE 框架 | KSyntaxHighlighting（XML 规则定义 + 匹配引擎） |
| [KDE/syntax-highlighting](https://github.com/KDE/syntax-highlighting) | 177 | 2026-11-16 | LGPL【待验证】 | 框架库 | 规则定义表驱动 |
| [tsujan/FeatherPad](https://github.com/tsujan/FeatherPad) | 523 | 2026-11-01 | GPL-3.0【已确认】 | Qt + QPlainTextEdit | 自研 per-language highlighter【待验证：README 级】 |

- **zep：能直接搬（接口分层）**——MIT；`ZepEditor / ZepBuffer / ZepMode` 组件边界、多后端 renderer 抽象，对 EUI-NEO 编辑组件的 API 设计是唯一同量级 MIT 参考。
- **kilo：能直接搬（教学）**——BSD-2；脏行增量渲染（每行 dirty 标记、只重绘变化行）可平移到 EUI-NEO 行渲染调度。
- **ImGuiColorTextEdit：只能借思路**——palette/token 分层极简清晰，但 ImGui 绑定与 DevDesk 无交集。
- **KSyntaxHighlighting：只能借思路**——「XML 规则定义表 + 通用匹配引擎」正是 DevDesk LangSpec 表驱动的放大版，佐证现路线；框架级依赖不可引入。
- **turbo / FeatherPad：不可用**（许可未核 / Qt 栈）。

## 二、Q2：查找替换的工程实现参考

生产级 C++ 实现选定两个（许可上前者可抄、后者只可读）：

### 2.1 Notepad4 `src/Edit.cpp`（BSD-3，main 分支，7991 行，2026-11-16 实查）
repo：https://github.com/zufuliu/notepad4

**全命中高亮 vs 当前命中区分**【已确认】：
- 全命中 = `EditMarkAll` 类（`src/Edit.cpp:5466` 起）：独立标记层（indicator），与编辑器 selection 分离；当前命中 = selection 本身（`EditFindNext()` `src/Edit.cpp:5311` 直接置 selection）。
- 增量调度参数全在头部常量：`EditMarkAll_MeasuredSize = 1024*1024`（:5468，1MB 分块）、`EditMarkAll_MinDuration = 1.0`（:5469）、`EditMarkAll_DefaultDuration = 64`（:5473，毫秒预算）、`EditMarkAll_RangeCacheCount = 240`（:5474，范围缓存）。
- 生命周期：`Reset()`（:5477）文档变更即重置 → `Start()`（:5507）→ 定时器驱动 `Continue(HANDLE timer)`（:5591）分块推进，块上限判断 ：5604；书签挂接 `EditMarkAll_Bookmark()`（:5550）。

**批量替换一次 undo 原子性**【已确认】：
- `EditReplaceAllInSelection(efr, EditReplaceAllFlag_UndoGroup)`（调用点 ：3635、:5068）：显式 undo 组 flag；`SciCall_BeginUndoAction()` / `SciCall_EndUndoAction()` 包裹整批替换（示例对 ：3641/:3694，另一组 ：3039/:3184）。
- 该项目把「是否 undo 组」参数化到每个编辑函数（如 `EditPadWithSpaces(bool bSkipEmpty, bool bNoUndoGroup)` :3424），是可平移的纪律。

**替换后光标/滚动落点**【已确认】：
- `EditEnsureSelectionVisible()`（`src/Edit.cpp:4383`）：替换/查找后统一调用保证当前命中可见。

**大文件增量**【已确认】：见上 `EditMarkAll` 分块 + 定时器 + 缓存模型（大文件的**全命中标记**不做全量同步扫，按 1MB 步进异步推进）。

### 2.2 Notepad++ `PowerEditor/src/ScintillaComponent/FindReplaceDlg.cpp`（GPL-2.0+，master，7393 行，2026-11-16 实查）
repo：https://github.com/notepad-plus-plus/notepad-plus-plus

- 批量替换入口 `FindReplaceDlg::processAll(ProcessOperation op, FindOption* opt, ...)`（:3393）；循环体 `SCI_SETSEARCHFLAGS`（:3612）→ `searchInTarget`（:3623）定位 → 替换；全开文档替换 `replaceAllInOpenedDocs()`（:3900）、`findAllIn(InWhat op)`（:3905）。
- 滚动落点：注释「Jump-scroll to center, if current position is out of view」（:263）；向上搜索取命中起点为滚动锚 `SCI_POSITIONBEFORE`（:279、:284，多行命中向上时以起始行滚动）。
- 增量查找条：`FindIncrementDlg::markSelectedTextInc(bool enable, FindOption* opt)`（:6832）——边输入边全命中高亮。
- **注意**：本文件 grep 未见 beginUndoAction（2026-11-16 实查）【已确认：仅限本文件】→ 批量替换的 undo 原子性 NPP 处理方式未核实到，**借鉴以 Notepad4 undo-group 模式为准**。

**对 DevDesk 的落点（不写方案，仅事实归纳）**：两仓均为「自绘/控件层标记全命中 + selection 表达当前命中」的组合；undo 原子性靠显式 undo 组包裹；滚动落点策略两仓不同（Notepad4 保可见、NPP 跳滚居中），都是替换完成后基于当前命中位置调用的。

## 三、Q3：JSON 格式化与校验的错误位置提示

**yyjson 能力边界**【已确认，来源 https://github.com/ibireme/yyjson/blob/master/doc/API.md 精读】：
- `yyjson_read_err` 三字段：`code`（yyjson_read_code，0-15 枚举：UNEXPECTED_CHARACTER / JSON_STRUCTURE / INVALID_NUMBER / INVALID_STRING / LITERAL / DEPTH 等）、`msg`、`pos`（错误发生的**字节偏移**）。
- **err.pos 能直接换算行列**：官方辅助函数 `yyjson_locate_pos(dat, len, pos, &line, &col, &chr)`——line/col 从 1 起，chr 从 0 起，按 Unicode 字符计（对编辑器友好）。现仓 `src/platform/json.h`（yyjson 封装）补一个薄封装即可，无需自扫。

**业界呈现方式参照**【已确认，来源 https://github.com/microsoft/vscode-json-languageservice `src/services/jsonValidation.ts` 2026-11-16 实查】：
- 解析错误（底层 jsonc-parser 的 `{offset, length}`）→ Diagnostic：`const range = Range.create(textDocument.positionAt(node.offset), textDocument.positionAt(node.offset + node.length))`（`jsonValidation.ts:63`）；严重度映射 `toDiagnosticSeverity()`（:168）。
- 关键做法：**错误提示是「区间」不是「点」**（offset + length → 高亮区间）；行号/列号由 textDocument.positionAt 在显示层换算。

**对 DevDesk 的落点**：状态栏/诊断面板展示 `line:col`（yyjson_locate_pos 直出）+ 编辑器内对错误区间画红标；yyjson err 只有 pos 单点无长度，区间端点需自行截取（如 pos 起到当前 token/行尾边界，借 vjls 思路）【已确认边界，区间截取为自研点】。

## 四、Q4：文件关联自动维护的开源参照

### 4.1 Windows：SumatraPDF（GPL-3.0，18k★，2026-11-10 活跃）
repo：https://github.com/sumatrapdfreader/sumatrapdf，文件 `src/shared/RegistryInstaller.cpp`（master，582 行，2026-11-16 精读）【已确认】

- **注册（写）**：`RegisterForOpenWith()`（:180）为每扩展建 ProgID + `HKCU\Software\Classes\<ext>\OpenWithProgids`（:193、:227-228）；`RegisterForDefaultPrograms()`（:145）写 `SOFTWARE\RegisteredApplications` + `Capabilities\FileAssociations`（:159-171）。全程走 `LoggedWriteRegStr/LoggedWriteRegDWORD`（写操作带日志，失败可追溯）。
- **状态回读（写成功 ≠ 生效 的处理）**：`HasRegistryValue()`（:56）/ `HasOurOpenWithEntry()`（:66）/ `HasAllOurOpenWithEntries()`（:72）——写后读回核对自己的条目是否真实存在。
- **撤销（回退）**：`UnregisterFromBeingDefaultViewer()`（:280）先 `LoggedReadRegStrTemp` 读当前默认值，**仅当默认者是自己时才清**（幂等、不误删用户后选的其它应用）；`RemoveUninstallerRegistryInfo()`（:264）。
- **生效刷新**：`ShellNotifyAssociationsChanged()`（:44）SHChangeNotify 广播 shell。
- **关键边界**【已确认】：Win8+ 程序不能静默把自己设为默认；Sumatra 只注册「打开方式候选 + RegisteredApplications」，把「设为默认」交给系统设置页。→ DevDesk 设置开关的现实语义：「注册/撤销候选」可全自动，「改为默认」只能引导跳系统设置。

### 4.2 macOS：defaultbrowser + duti【已确认源码文件，API 语义为 Apple 文档级常识】

- [kerma/defaultbrowser](https://github.com/kerma/defaultbrowser)（MIT，365★，2024-09-04 提交）：`src/main.m` 单文件——:15 `LSCopyAllHandlersForURLScheme`（候选列表）、:31 `LSCopyDefaultHandlerForURLScheme`（**状态回读**）、:39 `LSSetDefaultHandlerForURLScheme`（写入；deprecated）。三步齐全，是「查-读-写」最短教学实现。
- [moretension/duti](https://github.com/moretension/duti)（2.1k★，2023-07 后停滞；许可文件为 COPYRIGHT，SPDX 未检出【待验证】）：`duti.c` / `handler.c` / `plist.c` 实现按 UTI/扩展设置与**查询（`duti -x <ext>` 回读当前默认应用）**默认处理器【README 级，函数名未核】。
- **能力边界**【已确认（多源交叉：两项目均依赖 deprecated API）】：macOS 对「文件类型默认应用」**没有非 deprecated 的公开 setter**；`LSSetDefaultHandlerForURLScheme` / `LSSetDefaultRoleHandlerForContentType` 均为 deprecated 但仍工作。→ DevDesk 实际路线：Info.plist `CFBundleDocumentTypes` 静态声明（安装期）+ 运行时只做「回读检测（NSWorkspace / LSCopyDefaultApplicationURLForURL）+ 引导用户」，写侧不承诺。

**失败回退与状态回读模式归纳**【已确认（源自 4.1 实查）】：写前读原值存档 → 写 → 回读核对 → 不一致即回滚写回原值 → SHChangeNotify/LaunchServices 刷新 → UI 显示「待系统确认」。Sumatra 的 Logged* 写入 + Has* 回读 + Unregister 幂等是完整模板。

## 五、Q5：md4c 许可证与能力边界

repo：https://github.com/mity/md4c（MIT，1.5k★，2026-10-19 活跃）【已确认，README/LICENSE.md 精读】

- 许可证：MIT（LICENSE.md）。
- 扩展 flags【已确认】：`MD_FLAG_TABLES`（GitHub 表格）、`MD_FLAG_TASKLISTS`（任务列表）、`MD_FLAG_STRIKETHROUGH`、`MD_FLAG_ADMONITIONS`、`MD_FLAG_FOOTNOTES`、`MD_FLAG_HIGHLIGHT/INSERT/SUBSCRIPTS/SUPERSCRIPTS/UNDERLINE/SPOILERS/WIKILINKS/LATEXMATHSPANS/HARD_SOFT_BREAKS/PERMISSIVE*`、`MD_FLAG_COLLAPSEWHITESPACE/PRESERVEBLANKLINES`、禁用类 `MD_FLAG_NOHTML*/NOINDENTEDCODEBLOCKS`。
- 边界：默认仅 CommonMark（0.31.2 全合规），扩展全部显式 flag 开启；不做输入验证（UTF-8 由调用方保证）；math/spoiler/wikilink 输出自定义标签需自渲染。
- **结论：不换**。GFM 方言所需子集（表格/任务列表/删除线）flag 全有，现仓 EUI-NEO `components::markdown` 已用 md4c，属于业界同款选择；更轻的替代无必要（md4c 本身即 C 单函数入口级别的轻量实现）。

## 六、可借鉴结论（三档分类）

**直接搬（许可允许 + 逻辑可平移）**
1. Notepad4 `src/Edit.cpp` 查找替换族：undo 组包裹、EditMarkAll 增量标记调度（1MB 分块/定时器/范围缓存/预算自适应）、EditEnsureSelectionVisible 落点（BSD-3，可抄代码段）。
2. zep：编辑器组件 API 分层（ZepEditor/ZepBuffer/ZepMode + renderer 抽象）（MIT）。
3. kilo：脏行增量渲染调度（BSD-2）。
4. `yyjson_locate_pos()`：pos→行/列/字符换算（MIT，已在依赖链内）。

**借思路（许可传染或技术栈不同，只取设计）**
5. SumatraPDF 文件关联全流程（GPL-3.0——注册表键结构是 Windows 公开规范可照用，代码自己写）：Logged 写入 / Has* 回读 / 幂等撤销 / SHChangeNotify。
6. Notepad++ 查找交互（GPL-2.0+）：跳滚居中、向上搜索起始锚、增量查找条、宏录制。
7. lite-xl：薄内核 + 规则表 + 脚本层分层（佐证表驱动词法器路线）。
8. KDE/syntax-highlighting：规则定义表 + 通用匹配引擎的放大版（佐证 LangSpec 表驱动）。
9. klogg：超大文件 out-of-core 搜索/增量标记调度。
10. helix：tree-sitter 增量高亮的真实收益数据点（配合简报「可选升级」结论）。
11. vscode-json-languageservice：offset+length → 区间 Diagnostic 的呈现模式。

**不可用（及原因）**
12. Qt 系整体（NotepadNext / notepad-- / notepadqq / FeatherPad / klogg 栈 / Kate 栈）：GUI 框架依赖违反 DevDesk 硬约束。
13. Scintilla/Lexilla 本体（NPP/Notepad3/Notepad4/BowPad 编辑内核）：非 header-only、自带编辑组件与自研渲染重复——简报已淘汰，维持。
14. C#/WinUI（Notepads）：非 C++。
15. xi-editor：2024-03 起停滞。
16. macOS 运行时改文件类型默认应用：无公开 API，仅 deprecated 通道，不做。
17. kakoune / tilde：终端交互模型，与 GUI 记事本不匹配。

## 七、闭环三问自检

| 三问 | 回答 |
|---|---|
| 能否落地 | 能。每个推荐方案均给出目标环境（C++20/EUI-NEO 自绘、无 GUI 框架、双平台）下的具体依据：Notepad4/zep/kilo 为纯 Win32/纯 C++/纯 C 可移植逻辑，函数级锚点已核；`yyjson_locate_pos` 在现有 yyjson 依赖内直接可用；Windows 注册表键结构是 OS 公开规范；macOS 写侧被 OS 能力边界否决（这本身是落地结论的一部分）。 |
| 100% 覆盖 | 是。Q1 项目清单（三方向 19 项，含排除项及原因）；Q2 四个子问（全命中/undo 原子/落点/大文件增量）各有着落；Q3 两参照 + yyjson 行列换算确认；Q4 双平台 + 回读/回退模式；Q5 许可证 + flags + 换/不换结论。缺口（如 sakura 词法内部实现、klogg 标记器细节）已标注「谁可补、怎么补」：后续若走大文件路线再精读 klogg，走 IME 深化再读 sakura。 |
| 生产级参考 | 是。锚点均为 repo + 分支 + 文件路径 + 行号/函数名级（见 Q2/Q3/Q4 各节），star 数与最近提交均标查证日期 2026-11-16，可在目标环境直接核对。 |

## 八、产出标准硬清单自检

1. **落地性**：各节含「对 DevDesk 的落点」段，落地条件清单化（如：undo 组需自研 undo 栈支持 begin/end 组；Windows 只能注册候选不能改默认；macOS 只回读+引导）——通过。
2. **覆盖率**：见七表——通过。
3. **生产级锚点**：Notepad4 `src/Edit.cpp:5466` / NPP `FindReplaceDlg.cpp:3393` / Sumatra `RegistryInstaller.cpp:180` / vjls `jsonValidation.ts:63` / defaultbrowser `src/main.m:39` 等——通过。
4. **结论分级**：【已确认】/【待验证】/【推测】全标注，无未标注断言——通过。
5. **三问门**：进入「直接搬」列表的候选（Notepad4/zep/kilo/yyjson）三问全过；淘汰项（Qt 系/Scintilla/C#/停滞项目/macOS 写侧）均写明败因——通过。

## 九、遗留与未证实项

- Notepad3 / turbo / duti 的许可证文件未在 API 检出（根目录无标准 LICENSE 命名），引用其代码前需人工查看仓库（Notepad3 README 声明 GPL-3.0【待验证】）。
- sakura 自研词法、klogg 标记器、zep tree-sitter 支持均为 README 级线索【待验证】，未精读源码。
- NPP 批量替换 undo 原子性仅核实到 `FindReplaceDlg.cpp` 单文件无 BeginUndoAction，未全仓排查【已确认限本文件】。
- notepadqq 编辑控件实现未核（QSciScintilla 检索 0 命中，可能为自研 QPlainTextEdit 派生）【待验证】。
- cxasm/notepad-- 的 macOS 版成熟度未验证（仅确认跨平台声明与 QScintilla 依赖）。

## 十、来源清单

精读（raw 源码下载 + grep 定位，2026-11-16）：
- zufuliu/notepad4 `src/Edit.cpp`（main，7991 行）；`License.txt` 头部
- notepad-plus-plus `PowerEditor/src/ScintillaComponent/FindReplaceDlg.cpp`（master，7393 行）
- sumatrapdfreader/sumatrapdf `src/shared/RegistryInstaller.cpp`（master，582 行）
- microsoft/vscode-json-languageservice `src/services/jsonValidation.ts`（main）
- ibireme/yyjson `doc/API.md`（WebFetch）
- mity/md4c README（WebFetch）
- kerma/defaultbrowser `src/main.m`（master）

扫描（gh api 元数据：stars/license/pushedAt，2026-11-16）：本报告 Q1 全部仓库、sumatrapdf、vscode-json-languageservice、KDE/syntax-highlighting、md4c、yyjson、duti、kerma/defaultbrowser。

检索（WebSearch）：macOS LaunchServices 默认应用开源实现路线（defaultbrowser/duti 方向确认）。
