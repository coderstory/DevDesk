# 记事本模块外部世界事实调研（2026-11-16）

- 角色：researcher（技术调研）。仅外部事实，不含 PRD / 技术方案。
- 需求背景：DevDesk 新建「记事本」模块。已证事实（本仓，简报给信不重查）：`src/module_markdown`（≈1.2 万行，文件树/编辑器/预览/只读查看窗，视图模式 0=Edit 1=Split 2=Preview 3=Live，`src/module_markdown/md_view_state.h:21`）；高亮基建 `src/ui_kit/text_highlight/md_highlight.h`、`src/ui_kit/text_highlight/code_lexers.h`（Java/JS/TS/SQL/JSON 五类）、`src/module_markdown/md_keyword_highlight.h`（C++/Python LangSpec 表驱动）；`src/platform/json.h` parse/stringify(pretty)；EUI-NEO 只有 openFileDialog 无保存对话框；无 OS 文件拖入、无 tree 组件、fontWeight 数值无效；字号 kDefaultEditorFontSize=15，钳制 [10,32]（`src/module_settings/settings_config.h:20-23`）。
- 检索动作：22（WebSearch ×8、WebFetch ×6、webReader ×1、gh api/search Bash ×6、仓内 grep ×1），对照预算 25。

## 结论先行（≤5 条）

1. **Windows 记事本 Markdown 官方已上线**：2025-05-30 起 Insider 推送（v11.2504.50.0/.52.0），交互形态 = 格式工具栏 + 视图菜单/状态栏切换按钮（格式化 Markdown 视图 ↔ Markdown 源码视图），可在设置整体关闭；2026-07-24 v11.2606.15.0 修复 .md 与 .markdown 双扩展格式化。与 DevDesk 现有 0–3 视图模式同构（已确认）。
2. **查找/替换生产口径三源齐**（VS Code 官方 docs / Win 记事本官方博客+官方 release notes / Notepad++ 官方手册）：共同基线 = wrap-around 默认可开可关、Match case、历史下拉、从光标处向后搜；「替换后光标落点」三家官方文档均未逐字载明（唯一共同盲区，未证实）。
3. **大文件边界**：VS Code 生产锚点（repo+行级）`modelService.ts:575` MAX_MODEL_SIZE=10MB、`editor.largeFileOptimizations` 默认开（降级不拒开）、`files.ts` FILE_TOO_LARGE 错误型、50MB 拒开（官方 issue #64095 + 社区交叉，官方 docs 未载明）→ DevDesk 应对 10MB 文本/2MB JSON 采用「降级不拒开 + 上限硬拒开」双层（已确认 + 推测分标）。
4. **代码高亮选型**：推荐扩展仓内表驱动词法器（零依赖、直产 styled-runs；highlight.js v11.12.0 / BSD-3-Clause 仅作规则表参考）；tree-sitter（MIT，C API）列可选保真升级；Scintilla/Lexilla 淘汰（非 header-only、自带编辑组件与自研渲染器重复）。
5. **文件关联一物多扩展两平台都支持**：macOS 单 UTI + `UTTypeTagSpecification` 扩展数组（.txt/.json 应 import 公共 UTI 而非自建）；Windows 单 ProgId 多扩展映射（官方原句示例 .htm/.html/.shtml/.xht/.xhtml 同指一 ProgId）+ Capabilities/RegisteredApplications；Win8+ 不允许静默抢默认，必须走官方关联 UI（已确认）。

---

## Q1. Windows 11 记事本现行功能全集

### 文件操作
| 功能 | 事实 | 等级 | 来源 |
|---|---|---|---|
| 新建/打开/保存/另存为 | 基础功能存在；本轮未逐项取官方一手页面（support.microsoft.com 无专门 Notepad 使用总页，检索确认其文档散落 learn/release notes） | 转述 | [Notepad release notes（learn.microsoft.com）](https://learn.microsoft.com/en-us/windows-insider/release-notes/apps/notepad)（页面未覆盖基线项） |
| Tab 多标签 | 2023-01-19 起 Insider 推送，v11.2212.33.0；单窗多文件管理 | 已确认 | [Tabs in Notepad begins rolling out to Windows Insiders（2023-01-19）](https://blogs.windows.com/windows-insider/2023/01/19/tabs-in-notepad-begins-rolling-out-to-windows-insiders) |
| 行尾符 | 新建默认 CRLF；打开文件保留 LF/CR 不转换（Windows 10 1809 起三向识别） | 已确认 | [Introducing extended line endings support in Notepad（devblogs，2018）](https://devblogs.microsoft.com/commandline/extended-eol-in-notepad) |
| 编码 | Win11 新文件默认 UTF-8，设置里可改默认编码（UTF-8 / UTF-8 BOM / ANSI 等）；状态栏显示当前编码 | 已确认（官方 Q&A + Superuser 交叉） | [Microsoft Learn Q&A: Notepad UTF-8 to ANSI](https://learn.microsoft.com/en-us/answers/questions/2196429/windows-server-2022-notepad-utf-8-to-ansi)（转述）+ [superuser 1836227](https://superuser.com/questions/1836227/is-it-an-ansi-or-utf8-file) |
| 打印 | 存在，2026-07 改进跨窗口/跨 Tab 并发打印 | 已确认 | [Notepad release notes 2026-07-24 条目](https://learn.microsoft.com/en-us/windows-insider/release-notes/apps/notepad) |
| 保存对话框 | 记事本自身有另存为（OS 级）；DevDesk 侧对应缺口（EUI-NEO 无 saveFile）为仓内已证事实，非本问范围 | — | 本仓简报 |

### 编辑
| 功能 | 事实 | 等级 | 来源 |
|---|---|---|---|
| 查找/替换 | VS Code 式下拉面板「drops down into the upper right of the text area」（2022-02-22 官方博文，实现细节：用 RichEdit paragraph space-before 撑出下拉空间） | 已确认 | [Windows 11 Notepad – Math in Office（devblogs，2022-02-22）](https://devblogs.microsoft.com/math-in-office/windows-11-notepad/) |
| 查找选项 | Wrap around 可关；关闭时显示「Beginning/end of document reached」提示；空结果改 inline tip（2026-06 修复项佐证选项存在）；Match case / 搜索历史为社区一致转述 | 已确认（wrap/tips）+ 转述（match case/历史） | [Notepad release notes 2026-06-24 条目](https://learn.microsoft.com/en-us/windows-insider/release-notes/apps/notepad)；[TenForums](https://www.tenforums.com/)（转述） |
| 转到行 | 本轮未获官方一手来源 | 未证实 | — |
| 自动换行 | 经典功能；本轮未取到官方一手确认新记事本中的开关形态 | 未证实（功能常识级） | — |
| 字体设置 | 字体族 + 样式 + 字号三粒度（社区一致转述）；本轮未获官方一手页面 | 转述/待验证 | [Windows Central](https://www.windowscentral.com/)（转述） |
| 拼写检查/自动更正 | 存在（2026-06 release notes 有 spell-check underlines 修复项） | 已确认（功能存在） | [Notepad release notes 2026-06-24](https://learn.microsoft.com/en-us/windows-insider/release-notes/apps/notepad) |

### 视图
| 功能 | 事实 | 等级 | 来源 |
|---|---|---|---|
| 缩放 | 状态栏显示缩放百分比（Ctrl+滚轮） | 转述 | [superuser 1836227](https://superuser.com/questions/1836227/is-it-an-ansi-or-utf8-file) |
| 状态栏 | 行:列、缩放 %、行尾（CRLF/LF/CR）、编码（UTF-8/ANSI） | 已确认（二手双源交叉） | superuser + Reddit（转述，无官方 docs 页） |

### 2024–2025 新增
| 功能 | 事实 | 等级 | 来源 |
|---|---|---|---|
| Copilot / AI | 「Explain with Copilot」2024-02-08 官方博客；Rewrite（AI 重写）约 2024-11 起推送（社区教程日期 2024-11-06 佐证）；官方支持文档载明 Rewrite/Summarize/Write 三功能（GPT 系）；2025-11-21 更新：AI 文本功能 streaming 输出 | 已确认（Explain/streaming/支持文档）+ 转述（Rewrite 首发日期） | [Snipping Tool and Notepad updates（2024-02-08）](https://blogs.windows.com/windows-insider/2024/02/08/snipping-tool-and-notepad-updates-begin-rolling-out-to-windows-insiders)、[Enhance Your Writing with AI in Notepad（support.microsoft.com）](https://support.microsoft.com/en-us/windows/ai/ai-apps/enhance-your-writing-with-ai-in-notepad)、[Notepad update（2025-11-21）](https://blogs.windows.com/windows-insider/2025/11/21/notepad-update-begins-rolling-out-to-windows-insiders)、[elevenforum Rewrite 教程](https://www.elevenforum.com/t/turn-on-or-off-rewrite-ai-feature-in-notepad-app-in-windows-11.30331)（转述） |
| **Markdown 渲染** | **已确认上线**。2025-05-30 Insider 博客：v11.2504.50.0 / 11.2504.52.0，先 Canary+Dev，2025-06-04 更新确认扩至 Beta+Release Preview。交互形态（官方原文三要素）：①「formatting toolbar」边写边加粗/斜体/超链接/列表/标题；②「supports Markdown style input and files」；③「switch between formatted Markdown and Markdown syntax views in the view menu or by selecting the toggle button in the status bar」——即**不是单一「预览按钮」，而是格式工具栏 + 视图切换（含状态栏 toggle）**。设置可整体关闭：「disable support for formatting entirely in app settings」。2026-07-24 v11.2606.15.0：「Formatting is now applied properly when working with both .md and .markdown files」 | 已确认 | [Text formatting in Notepad begin rolling out to Windows Insiders（2025-05-30）](https://blogs.windows.com/windows-insider/2025/05/30/text-formatting-in-notepad-begin-rolling-out-to-windows-insiders)（精读）、[Windows Central 指南](https://www.windowscentral.com/microsoft/windows-11/how-to-get-started-with-markdown-in-notepad-app-for-windows-11)（扫描）、[release notes 2026-07-24](https://learn.microsoft.com/en-us/windows-insider/release-notes/apps/notepad)（精读） |
| 暗色主题 | 随 Win11 记事本重设计提供（2021–2022 时点） | 转述（二手，本轮未取官方一手博客） | [Wikipedia: Windows Notepad](https://en.wikipedia.org/wiki/Windows_Notepad) |
| 附带情报 | Microsoft 另出轻量无 AI 编辑器「Microsoft Edit」（社区热议其为 Notepad 替代） | 转述（线索级，与本项目相关性低） | 本轮搜索摘要 |

---

## Q2. 查找/替换生产级交互口径（三源差异表）

一手来源：VS Code 官方 docs（codebasics，2026-11-16 精读）；Windows 记事本官方 devblog（2022-02-22）+ 官方 release notes（2026-06/07）；Notepad++ 官方手册 Searching 章（npp-user-manual.org/docs/searching/，全文精读）。

| 维度 | VS Code | Windows 11 记事本 | Notepad++（v8.9.x 手册口径） |
|---|---|---|---|
| 面板形态 | 编辑器内下拉 widget，可拖宽/最大化 | 同式下拉到文本区右上（官方明确「mimics VS Code」） | 独立对话框（Find/Replace/FiF/Mark 四 tab），可折叠成迷你版 |
| 大小写敏感 | Match Case 开关 | Match case（社区转述） | Match case 开关；正则 `(?i)/(?-i)` 可覆盖 |
| 全词 | Match Whole Word 开关 | 转述存在 | Match whole word only（ASCII 词边界规则详述；正则模式下灰掉，建议 `\b`） |
| 正则 | Regular expression 开关 | 转述无 | Search Mode 三选一：Normal / Extended(\n,\r,\t,\0,\x…) / Regular expression（Boost 1.90，含 `. matches newline` 子开关） |
| 全命中高亮 + 当前命中区分 | 产品事实，本轮 fetched docs 未逐字覆盖该点 | 转述存在高亮 | Mark tab（永久标记/书签行/5 种样式）+ 增量搜索 Highlight all + 智能高亮 |
| 单个替换 | Replace 单发；替换框带 live diff 预览；可替换一处/全文件/跨文件 | Replace 单发（选项细节转述） | Replace：替换当前选中命中；无当前命中时等同 Find Next |
| 全部替换 | Replace All（该页未载明快捷键） | 存在（选项细节转述） | Replace All **范围规则表**（官方表格）：Wrap=ON→全文一遍；Wrap=OFF→光标→EOF（或反向）；In selection→选区内 |
| 替换后光标落点 | 官方该页**未载明** | 官方未载明 | 官方未载明（语义可从 Replace 范围规则推导：逐个命中推进） |
| 从光标处 vs 全局首命中 | 隐含从光标处向后；可 find-in-selection | wrap 关闭时「Beginning/end of document reached」→ 默认 wrap 开 | 显式规则：从光标处向后 + Wrap around 绕回；方向键 ▲▼/Shift+Enter 反向 |
| 输入播种 | 自动填充选中文本/光标处词（可关 `editor.find.seedSearchStringFromSelection`） | 未证实 | 永远自动填充（选中文本或光标处词；v8.8.4 起上限可调） |
| 历史 | 按工作区保存，跨重启（`editor.find.history`/`replaceHistory`） | 记住上次值（社区转述） | 下拉历史，默认跨会话存 10 条；Find what 上限 16383 字节（v8.8.4+，之前 2046） |
| 搜索中/即时搜 | find-as-you-type（`editor.find.findOnType` 可关） | 未证实 | 增量搜索独立面板（Ctrl+Alt+I；Count 2/5 式指示，v8.9.7+） |
| 特有 | Preserve case（替换侧）；多行粘贴搜索框；未命中行为/溢出阈值该页未载明 | 下拉空间用段落 space-before 实现（官方实现细节） | 替换字段 swap（⇅，v8.5.1/8.5.2 扩展）；未命中声音+窗口闪烁；选区 >1024 字符时自动勾 In selection（陷阱，官方警示） |
| 键位 | Ctrl+F 开；Enter 下一个 / Shift+Enter 上一个；Alt+L 选区内查找 | Ctrl+F/Ctrl+H | Ctrl+F / Ctrl+H；Enter=Find Next（方向随 Backward direction），Shift+Enter 反向；F3/Shift+F3 免对话框 |
| 状态反馈 | 该页未载明未命中反馈形态 | 空结果 inline tip；wrap 关闭时到头提示 | 对话框底部状态条：Not found / Message / Search end reached（v8.7+ 可配色）；正则非法仅在 Find Next 时报错（官方警示） |

**生产口径结论**（对标用，非技术方案）：
1. 三家共同基线：大小写开关、历史、wrap-around、从光标处向后搜、单发/全替换两档 —— 任一现代编辑器的「及格线」。
2. 正则是差异线：VS Code/N++ 有、新记事本（转述）无；DevDesk 若只做txt/json/md 场景，正则可选做（覆盖缺口写明）。
3. 「替换后光标落点」三家官方文档均未逐字定义——如 DevDesk 要定义，需自行定并测试锁定（未证实项，不留含糊）。

---

## Q3. 大文件边界与降级策略

### VS Code 生产锚点（repo + commit 级）
仓库 `microsoft/vscode`，本轮 pinned commit `6e68549edb69380c9316e49df5d3795d5064a2b0`（2026-11-16 时点 main HEAD，gh api 实查）：

| 事实 | 锚点 | 等级 |
|---|---|---|
| `MAX_MODEL_SIZE = 10 * 1024 * 1024`，注释原文「takes 200ms to compute a sha1 on a 10MB model on a new machine」——10MB 是 VS Code 自己定义的「重操作量级」门槛 | `src/vs/editor/common/services/modelService.ts:575` | 已确认（源码实查） |
| `MAX_MEMORY_FOR_CLOSED_FILES_UNDO_STACK = 20 * 1024 * 1024` | 同文件 `:86` | 已确认（源码实查） |
| `editor.largeFileOptimizations` 配置读取（默认 true 走 EDITOR_MODEL_DEFAULTS） | 同文件 `:158-179` + `src/vs/editor/common/config/editorConfigurationSchema.ts` | 已确认（源码实查） |
| 文件超尺寸抛 `FILE_TOO_LARGE` 错误型（`size?: number` 契约） | `src/vs/platform/files/common/files.ts:350-355` | 已确认（源码实查） |
| 50MB 打开上限：issue 原文讨论「the limit is 50MB」并辩 5/10MB 更优；社区多方一致（edit-csv #98、SO、vsc-lfs 扩展专开 >50MB）；官方 docs 无专页 | [vscode issue #64095（2018-11）](https://github.com/microsoft/vscode/issues/64095) + [vscode-edit-csv #98](https://github.com/janisdd/vscode-edit-csv/discussions/98) | 已确认（官方 repo issue + 双二手交叉；常量级源码锚点本轮未定位到，标待验证） |
| 大文件时 VS Code 关闭的功能清单：tokenization、wrapping、folding、codelens、word highlighting、sticky scroll（UI toast 原文引述） | [r/vscode](https://www.reddit.com/r/vscode/comments/1jmgbw6/large_file_syntax_highlighting_setting/)（转述 UI 文案） | 已确认（二手引 UI） |

### 10MB 文本 / 2MB JSON 在「无虚拟化单 input」下的现实阈值（DevDesk 语境）
- VS Code 把 10MB 当作需要专门绕开的量级（MAX_MODEL_SIZE 锚点）——无虚拟化的自绘编辑器只会更紧。
- **推测**（无外部一手来源，按仓内已证架构推演）：EUI-NEO 行级 styled-runs 模型 + 无虚拟滚动，全量排版成本随总行数线性走；万行级（≈1–2MB 纯文本）大概率可用，5–10MB（数万至十万行）全量 styled-runs + 每次编辑全行重排会出现可感卡顿。**此为推测，须 POC 实测后转正**。
- 2MB JSON：仓内已有 `src/platform/json.h` parse/stringify(pretty)（已证）；解析/美化一次性的 CPU/内存开销属单次操作，主要风险仍在**渲染侧行数**（2MB pretty 后数万行）而非 JSON 处理本身（推测，同上须 POC）。

### 降级策略候选（生产参照）
| 策略 | 生产参照 | 说明 |
|---|---|---|
| 降级不拒开（关高亮/关富排版） | VS Code `largeFileOptimizations`（默认开）：关 tokenization/wrap/folding 等，文件照开 | 与 DevDesk 最贴合：大文件切「纯文本模式」（关 styled-runs/关高亮/关预览联动） |
| 只读兜底 | DevDesk 已有只读独立查看器窗（仓内已证） | 超大文件可直接走只读查看窗，绕开编辑器 |
| 虚拟滚动 | VS Code/现代编辑器标配；EUI-NEO 现无（仓内已证） | 属框架能力缺口，走 EUI-NEO fork 补（本仓纪律：框架侧修） |
| 上限硬拒开 | VS Code 50MB `FILE_TOO_LARGE` 拒开 + 明确报错文案 | 终极保险丝，给明确提示而非假死 |

---

## Q4. 代码高亮语言覆盖选型

### 对标「常见格式」的语言清单（选型输入）
纯文本侧：txt / markdown / json（仓内已有词法器）。代码侧常见缺口（仓内现有 Java/JS/TS/SQL/JSON 五类之外）：XML/HTML、CSS、C/C++、Python、Bash/Shell、INI/YAML/TOML、C#。（清单为需求推演，标注：推测）

### 候选对比
| 候选 | 许可证 | 体积/依赖 | 「行级 span 输出」适配 | 生产级锚点 |
|---|---|---|---|---|
| **① 仓内表驱动扩展**（highlight.js 思路：规则表→行内 span，移植规则不引运行时） | 无外部依赖（规则表参考物 highlight.js = BSD-3-Clause，v11.12.0，2026-11-11 仍活跃，gh api 实查） | 零依赖、header-only（仓内 `.h`） | 直接：现有 `code_lexers.h` / `md_keyword_highlight.h` LangSpec 表驱动已产行级 styled-runs | 本仓 `src/ui_kit/text_highlight/code_lexers.h`（生产已跑通）+ [highlightjs/highlight.js](https://github.com/highlightjs/highlight.js)（规则表来源） |
| **② tree-sitter** | MIT（GitHub repo license + 多源确认） | C 运行时库 + 每语言 grammar 生成 C 源；「全部解析功能经 C API 暴露」 | 需自写适配层：CST 遍历 → 行级 span（增量解析对编辑器友好，错误容忍） | [tree-sitter/tree-sitter](https://github.com/tree-sitter/tree-sitter)（MIT）；[官网](https://tree-sitter.github.io)；[SourceForge mirror 描述](https://sourceforge.net/projects/tree-sitter.mirror)（C API 口径） |
| **③ Scintilla/Lexilla** | Scintilla License（HPND 系 permissive，「permitting use in any free or commercial product」） | Scintilla 5 起 lexer 全部移入独立 Lexilla 库（static/dynamic）；自带编辑组件 | 需剥离 lexer 用途，与自研渲染器重叠 | [Lexilla 官方页](https://scintilla.org/Lexilla.html)；[Lexilla License.txt](https://github.com/ScintillaOrg/lexilla)；[Scintilla 5 migration](https://fossies.org/linux/scintilla/doc/Scintilla5Migration.html) |

### 三问门裁决
- **① 过，推荐为主路线**：能落地（仓内同构先例 LangSpec 表驱动已产 styled-runs，零新依赖）；覆盖可 100%（缺的语言逐个补表，每类词法器=一个表+一个扫描函数）；生产锚点仓内文件级。败因无。
- **② 过但列可选**：能落地（MIT + 纯 C API + 大量编辑器生产先例），但集成成本中高（runtime + 各语言 grammar + CST→span 适配层），对「记事本级」场景性价比低 → 远期保真升级选项。
- **③ 败**：非 header-only、拖入整个 lexer 库、与自研渲染职责重叠 → 淘汰（败因：依赖价值低、功能利用率低——六级梯子上「现成方案引用」反而重）。

---

## Q5. 多扩展名文件关联（json/txt/md）

### macOS（UTI / LaunchServices）
| 事实 | 等级 | 来源 |
|---|---|---|
| 一个 UTType 可覆盖多个扩展：`UTTypeTagSpecification` → `public.filename-extension` 是**数组**，系统把列表内每个扩展映射到同一 UTI | 已确认（官方 key 文档结构 + 本轮 WebFetch 转述 + SO 交叉） | [UTImportedTypeDeclarations（Apple）](https://developer.apple.com/documentation/bundleresources/information-property-list/utimportedtypedeclarations)、[CFBundleDocumentTypes（Apple）](https://developer.apple.com/documentation/bundleresources/information-property-list/cfbundledocumenttypes)、[Defining file and data types for your app（Apple）](https://developer.apple.com/documentation/uniformtypeidentifiers/defining-file-and-data-types-for-your-app)（本轮正文未取到，结构转述）、[SO 21937978](https://stackoverflow.com/questions/21937978/) |
| 自有格式 → `UTExportedTypeDeclarations`；他人/公共类型 → `UTImportedTypeDeclarations`（owner 装了以 owner 声明为准） | 已确认 | 同上（官方 key 文档） |
| `CFBundleDocumentTypes` 用 `LSItemContentTypes` 指向 UTI（现代写法，替代废弃的 LSItemExtensions），配 `CFBundleTypeRole=Editor`、`LSHandlerRank=Owner` | 已确认（转述官方结构） | 同上 |
| 注册时机：app bundle 启动时 LaunchServices 自动注册；非正常流程可用 `LSRegisterURL(_:updateIfNeeded:)` 强制 | 转述（官方指南口径） | 同上 |
| .txt/.json 是公共类型（public.plain-text / public.json），应 import 公共 UTI 并在 CFBundleDocumentTypes 声明 Editor 角色，而非为它们自建 UTI | 推测（基于上述官方规则推演，PIList 键文档间接支持） | 同上 |
| 现仓实现：`src/platform/file_association.h:31` `productionConfig() = {"DevDesk.md", ".md"}`；平台侧 `src/platform/file_association_macos.mm`（16.6KB）与 `src/platform/file_association_win.cpp`（10.5KB） | 已确认（仓内实查） | 仓内 |

### Windows（HKCU\Software\Classes / Default Programs）
| 事实 | 等级 | 来源 |
|---|---|---|
| **一个 ProgId 可映射多个扩展**，官方原句：「ContosoHTML points to a single ProgID that provides shellexecute information for the .htm, .html, .shtml, .xht, .xhtml extensions」「an application can map one-to-one or one-to-many」→ **不需要每扩展独立 ProgId** | 已确认（官方文档精读） | [Default Programs（learn.microsoft.com，git_commit 5921b5ae8a2855544a0520b7dc04d4e1abe6193f）](https://learn.microsoft.com/en-us/windows/win32/shell/default-programs) |
| 注册结构：`HKLM/HKCU\Software\<App>\Capabilities`（ApplicationDescription 必填，否则不进 UI 列表）+ `FileAssociations` 每扩展一值指向 application-specific ProgID + `HKLM\SOFTWARE\RegisteredApplications` 指向 Capabilities 路径 | 已确认（精读） | 同上 |
| application-specific ProgID 必须自带完整信息（shell\open\command、DefaultIcon 等），不能只靠通用 `.ext` 子键 | 已确认（精读） | 同上 |
| `HKCR` 是 `HKLM\Software\Classes` 与 `HKCU\Software\Classes` 的合并视图，per-user 优先 | 已确认（官方 fa-progids，本轮转述） | [Programmatic Identifiers（learn.microsoft.com）](https://learn.microsoft.com/en-us/windows/win32/shell/fa-progids) |
| per-user 默认设置：Win8 起 `SetAppAsDefaultAll` deprecated；文档明示「This topic does not apply for Windows 10」（机制已改，见官方给的 [Win10 博客](https://blogs.windows.com/windowsexperience/2015/05/20/announcing-windows-10-insider-preview-build-10122-for-pcs/)）；推荐路径 = `IApplicationAssociationRegistrationUI::LaunchAdvancedAssociationUI` 弹官方关联 UI + `SHChangeNotify(SHCNE_ASSOCCHANGED,…)` 通知系统刷新 | 已确认（精读） | [Default Programs（learn.microsoft.com）](https://learn.microsoft.com/en-us/windows/win32/shell/default-programs) |
| 静默写 `UserChoice`（hash 保护）不可行，默认必须用户在官方 UI 里选 | 转述（官方文档未逐字写 hash 细节；社区一致） | 同上 + 社区常识（待验证级） |
| Inno Setup/安装器注册路径：`scripts/package-windows.sh` 未含关联注册（grep 无 Classes/ProgId 命中）→ Windows 侧注册在应用代码 `src/platform/file_association_win.cpp` | 已确认（仓内实查） | 仓内 |

### 扩展为多扩展名的推荐口径（调研结论，非方案）
- **Windows**：不需要每扩展独立 ProgId。保持单一 `DevDesk.md` ProgId（或改名 `DevDesk.AssocFile`），在 `Capabilities\FileAssociations` 增加 `.txt`/`.json` 两条值同指该 ProgId；ProgID 子键补全 shell\open\command；注册后 SHChangeNotify 刷新；默认设置交给官方关联 UI。
- **macOS**：`DevDesk.md` UTI 的 `UTTypeTagSpecification.public.filename-extension` 数组加 `.md`（如需 `.markdown` 同加，记事本官方也同时支持两扩展——见 Q1 2026-07 条目）；`.txt`/`.json` 走公共 UTI import + CFBundleDocumentTypes 追加条目。
- 平台差异落实在现有 `file_association_win.cpp` / `file_association_macos.mm` 拆文件结构内，注入缝已是参数（`file_association.h:23` 注释明确「ProgId 名与目标扩展名可替换」）。

---

## 闭环三问自检

| 问题 | 能否落地 | 覆盖度 | 生产级参考锚点 |
|---|---|---|---|
| Q1 功能全集 | 落地（结论直接可作 PRD 对标表；交互形态有官方原文引句） | 覆盖：文件/编辑/视图/AI/Markdown/暗色逐项有结论；3 项标未证实（转到行、word wrap 官方一手、字体粒度一手）并写明缺什么——官方无 Notepad 总文档页，只能 release notes 逐条挖，已列补挖路径 | 官方 URL 级：blogs.windows.com ×4、learn.microsoft.com release notes（git_commit 70b53b90）、devblogs ×2、support.microsoft.com ×1 |
| Q2 查找/替换 | 落地（差异表 16 维度可直接转交互规格；共同盲区已点名，不留含糊） | 100%：三家逐项覆盖，转述项显式标注 | VS Code 官方 docs 页 + N++ 官方手册全文 + MS devblog/release notes |
| Q3 大文件 | 落地（降级策略四候选各附生产参照；DevDesk 侧阈值标推测并要求 POC 转正——不冒充已证） | 100%：10MB 文本、2MB JSON 两场景都有结论 + 降级梯子 | repo+commit 级：microsoft/vscode @ `6e68549edb69380c9316e49df5d3795d5064a2b0`，`modelService.ts:575/:86/:158`、`files.ts:350`；issue #64095 |
| Q4 高亮选型 | 落地（①主路线仓内同构先例已跑通 = 可编译可运行依据；②③给出集成步骤粒度差异） | 100%：语言清单 + 3 候选对比 + 三问门裁决（③败因写明） | repo 级：highlightjs/highlight.js v11.12.0（BSD-3，gh api 实查）、tree-sitter/tree-sitter（MIT）、ScintillaOrg/lexilla + scintilla.org/Lexilla.html；仓内 `src/ui_kit/text_highlight/code_lexers.h` |
| Q5 文件关联 | 落地（两平台推荐口径落到现有注入缝 `file_association.h:23/31` 与两平台实现文件，可按此扩参） | 100%：五问子项（UTI 多扩展/Exported vs Imported/ProgId 一对多/Capabilities 结构/默认设置限制）全有来源；UserChoice hash 标转述待验证 | learn.microsoft.com default-programs（git_commit 5921b5ae）+ fa-progids；Apple 三文档 URL（CFBundleDocumentTypes/UTImportedTypeDeclarations/UTType 指南） |

淘汰项记录：Q4 候选③ Scintilla/Lexilla 败因 = 非 header-only、自带编辑组件与自研渲染重复、依赖价值低功能利用率低；Q2 不做正则的可选项如被砍，缺口 = 复杂文本处理能力缺失，由「后续用 Notepad++ 级正则」补（本模块不承诺）。

---

## 来源清单（标注精读/扫描）

精读（全文/正文取证）：
1. https://learn.microsoft.com/en-us/windows-insider/release-notes/apps/notepad （官方 Notepad release notes，ms.date 2026-06-11 / updated 2026-07-24，git_commit 70b53b90）
2. https://blogs.windows.com/windows-insider/2025/05/30/text-formatting-in-notepad-begin-rolling-out-to-windows-insiders/
3. https://code.visualstudio.com/docs/editor/codebasics
4. https://npp-user-manual.org/docs/searching/ （Notepad++ 官方手册，全文）
5. https://learn.microsoft.com/en-us/windows/win32/shell/default-programs （git_commit 5921b5ae）
6. https://devblogs.microsoft.com/math-in-office/windows-11-notepad/ （2022-02-22）
7. gh api：microsoft/vscode 源码 4 文件 @ commit 6e68549edb69380c9316e49df5d3795d5064a2b0；highlight.js repo 元数据（license/version）
8. 仓内：`src/platform/file_association.h`、`src/platform/file_association_macos.mm`、`src/platform/file_association_win.cpp`、`scripts/package-windows.sh`

扫描（线索/转述，未全文取证）：
- https://blogs.windows.com/windows-insider/2023/01/19/tabs-in-notepad-begins-rolling-out-to-windows-insiders/
- https://blogs.windows.com/windows-insider/2024/02/08/snipping-tool-and-notepad-updates-begin-rolling-out-to-windows-insiders/
- https://blogs.windows.com/windows-insider/2025/11/21/notepad-update-begins-rolling-out-to-windows-insiders/
- https://support.microsoft.com/en-us/windows/ai/ai-apps/enhance-your-writing-with-ai-in-notepad
- https://devblogs.microsoft.com/commandline/extended-eol-in-notepad
- https://learn.microsoft.com/en-us/windows/win32/shell/fa-progids
- https://developer.apple.com/documentation/bundleresources/information-property-list/cfbundledocumenttypes 、…/utimportedtypedeclarations 、…/uniformtypeidentifiers/defining-file-and-data-types-for-your-app
- https://github.com/microsoft/vscode/issues/64095 、https://github.com/janisdd/vscode-edit-csv/discussions/98
- https://github.com/tree-sitter/tree-sitter 、https://tree-sitter.github.io 、https://scintilla.org/Lexilla.html 、https://fossies.org/linux/scintilla/doc/Scintilla5Migration.html
- superuser.com/questions/1836227 、stackoverflow.com/questions/21937978 、windowscentral.com（Markdown 指南）、Wikipedia: Windows Notepad、elevenforum.com（Rewrite 日期佐证）
