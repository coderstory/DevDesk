# Markdown 编辑器体验升级（对标 Typora）— 调研与分阶段设计

- 日期：2026-09-22
- 状态：设计稿（未实施）
- 范围：`module_markdown` 的编辑体验 + 必要的 EUI-NEO 上游改造
- 依据：实读 `../EUI-NEO/components/{text,input,input_model,markdown,scrollview,theme,contextmenu,virtuallist}.h`、`core/render/text.{h,cpp}`、`core/input/input_types.h`、`core/runtime/runtime_input.h`、`core/app/{app_runner,frame_pacing}.h`，以及 `src/module_markdown/*`、`src/core/theme.{h,cpp}`

---

## 1. 需求

对标 Typora 补齐五项显性差距，并重点补齐**隐含的 UI/UX**——那些让 Typora "感觉好"但不易察觉的机制：

1. 交互范式（所见即所得感）
2. 编辑器内语法高亮
3. 表格编辑器
4. 大纲面板
5. 加粗/斜体等 Markdown 快捷键

**隐含体验优先**：打字到渲染的即时性、滚动同步、无干扰、光标行为、键盘优先流、新笔记即时性、保存的安心感。

**诚实原则**：本文所有"能做到/做不到"的结论都以源码为准；EUI-NEO 不够的地方直说，不设计空中楼阁；成本评级保守（小=纯 DevDesk 层 1-2 天量级；中=3-7 天含上游 PR；大=周级以上）。

---

## 2. EUI-NEO 能力盘点结论

### 2.1 多色 run（语法高亮的硬前提）——**原生不支持，但有成熟替代路径**

- `core::TextStyle`（`core/render/text_types.h:21`）是**整段单一样式**：一个 `text` + 一个 `fontFamily/fontSize/fontWeight/color`。`TextPrimitive` 没有任何 per-range span/rich-run API。
- **但是**：`components/markdown.h` 已经证明"一段文字内多样式"在 EUI-NEO 上是可实现的——它把 md4c 解析出的 runs 手工切成词级 `InlineSegment`（自测宽度、手工定位），每个 segment 一个 `ui.text`，外加手工换行和装饰线（`renderInlineRuns`/`renderInlineSegment`，markdown.h:1418-1488）。
- 结论：**"多色一段"在静态渲染侧已是 solved problem**；真正缺的是**编辑器内**的多色——`components::input` 渲染多行文本时每行只画一个 `ui.text`、单色 `style_.text`（input.h:401-413）。语法高亮需要上游改造 input（见 §5.1）。

### 2.2 input 组件架构（`input_model.h` 932 行 + `input.h` 517 行）

架构分层干净，扩展点明确：

- **InputModel 是无状态纯函数库**（光标移动、选区、undo/redo、行测量），`InputState` 存在 `ui_.state<InputState>(id_)` 里——**DevDesk 模块层可以直接读写这个状态**（include 即可），这是重要的逃生通道，但属于 hack，正规做法是上游 API。
- **增量排版**：`updateChangedParagraphs`（input_model.h:594）用前后缀 diff 只重排变化的段落，保留 ligature/UTF-8 上下文——**大文档输入性能是设计过的**，不是全量重排。
- **可视行裁剪**：渲染时只画视口内的行（input.h:393-394），行级 `dirtyKey`——滚动/打字只重绘受影响行。
- **光标/选区**：每行 metrics 带 `byteIndices`/`caretX` 数组，`caretX()` 二分查表；选区矩形按行生成、视口裁剪。软换行、硬换行、UTF-8 边界（`clampUtf8Boundary`）都有处理。
- **undo/redo**：内置 128 深快照栈；**IME 预编辑**完整支持（preedit 独立展示状态，不动文档/撤销史，input_model.h:659-685）；剪贴板走 `window::setClipboardText`。
- **已处理的键**（input.h:199-331）：Ctrl+A/C/X/V/Z/Y、方向键（含 Shift 选区、上下行 preferredCursorX）、Home/End（行首尾）、Delete/Backspace、Enter（多行插 \n）、Escape。
- **没处理的键**：Tab（`handled=false` 直接放行，且 EUI-NEO 无 Tab 焦点循环——Tab 现在完全无效，多行编辑里连缩进都没有）、Ctrl+B/I/K 等一切非内置快捷键。
- **InputBuilder 对外只有 `onChange/onEnter/onFocus`**，没有 onKeyEvent passthrough——模块层拦截不了按键。

### 2.3 markdown 组件渲染管线（`markdown.h` 1719 行）

- 管线：md4c 解析 → `MarkdownBlock`（块级）+ `MarkdownRun`（行内 rich run：emphasis/strong/link/code/math/wiki…）→ 手工 inline 布局 → 每块一个 `ui.stack`、每 segment 一个 `ui.text`。
- **双层缓存**：文档级（按源字符串 hash，LRU）+ inline 布局级（按 runs+宽度+字号 key，1MB 容量）→ 同内容重复渲染近零成本。
- **块级实时渲染可复用性**：每个块独立成 `ui.stack`，理论上可逐块拼装"渲染块 + 编辑块"混排。但 `MarkdownBlock` **不携带源码 byte offset**——md4c 的 AST 落到 Block 模型时丢了位置信息。做"光标所在块显示源码"必须自己建 块↔源码区间 映射（自研分块扫描器，或上游给 Block 加 offset）。
- 代码块**无语法高亮**（单色 `codeText`），表格**等宽列**（`cellWidth = contentWidth / cellCount`），高度用平均字宽估算（`estimateMarkdownTextHeight`）——长代码块高度与实际可能有偏差。
- `MarkdownBuilder::estimateHeight` 是 public static（带缓存），可对任意前缀子串估高——**这是滚动同步按块锚点映射的基础**。

### 2.4 滚动同步

- `scrollView`：`offset(float)` 可编程设置、`bind(Signal<float>&)` 双向绑定、`onChange(float)` 滚动回调（scrollview.h:31-36, 55）——**外部驱动它的滚动位置是一等公民用法**。
- 编辑器侧：`InputState.verticalScroll` / `cursor` 可直接读（见 2.2）。
- 结论：**同步滚动双栏可行性高**，不需要上游改动。映射算法见 §5.3。

### 2.5 键盘事件

- `KeyEvent` 带完整修饰键（control/shift/alt/super + capsLock/numLock，`input_types.h:45`），`shortcut()` 跨平台封装（macOS=super，其余=control），有 Press/Repeat/Release。
- 分发语义（runtime_input.h:294-318）：按键先给**焦点元素**的 onKeyEvent，未处理（返回 false）才落到 **app 级全局 `onKeyEvent`**（`eui/app.h:22`，全 app 只有一个）。DevDesk 目前没设置全局 handler。
- Tab：焦点循环未实现、input 不消费——**按 Tab 当前什么都不发生**。

### 2.6 帧率与大文本性能

- FPS：app config `fps()` 可配，clamp 30-500，默认跟显示器刷新率 + vsync；Windows 用高精度 waitable timer 做帧间隔（frame_pacing.h）。**帧率不是瓶颈，每帧工作量才是**。
- 输入侧大文档：增量段落排版 + 可视行裁剪 + 行级 dirtyKey（2.2）→ **>5000 行输入侧问题不大**。
- 预览侧大文档：markdown 全文档渲染成 block 树（**无虚拟化**，block 元素无 dirtyKey，靠 runtime 按 id diff）；每敲一字 `contentKey`（含 `editorText_.size()`）变化 → scrollView 用**离屏 measureUi 把整棵 markdown 树重建一遍**来测高（scrollview.h:164-186）——大文档下这是每键双倍渲染。**>5000 行预览侧性能是已知风险，未实测**。
- 已发现的真 bug：预览 scrollView 的 `contentKey = path + text.size()`（markdown_module.cpp:455）——**同长度替换字符时高度缓存不失效**，预览内容更新但滚动高度滞留旧值。

### 2.7 杂项

- fontWeight：`docs/EUI-NEO-CAPABILITIES.md` 说"runtime 不切 face"，但 `text.cpp:310` 的 `resolveSystemUiFontPathForWeightImpl` 在 Windows 上已按权重选 Segoe UI 分文件——**至少系统默认字体族粗体是真切的，文档过时**；`Microsoft YaHei` 显式族名的权重切换待验证（不影响设计，粗体 700 在预览中已实际使用）。
- `contextMenu` 组件 API 完备（items + onSelect + 级联），可直接做"插入表格"菜单。
- `virtualList` 存在（固定行高虚拟化），大纲面板若标题极多可用，一般直接 column 即可。

---

## 3. 交互范式对比与分阶段推荐

### 方案 1：真 WYSIWYG（Typora 默认模式）——**不可行，排除**

Typora 的即时渲染本质是 contenteditable + 自研富文本引擎（光标直接在渲染后的文本上，`**` 输入即消失变成粗体）。EUI-NEO 缺一整套基础设施：

- 无 span/rich-run 文本原语（§2.1）；
- 无"同一布局既服务 caret 映射又服务渲染"的引擎——markdown 组件的 inline 布局是词级估算、gap=4 的近似，光标进去会跳；
- 无富文本文档模型（样式与字符双向同步、样式边界编辑语义）。

这是在 EUI-NEO 里造一个 CodeMirror/contenteditable 等价物，**月级以上工程**，且与"input 走 FreeType shaping + byte 级 caret"的现有路线冲突。诚实结论：不做。Typora 体验里 WYSIWYG 的*体感*（所见即所得、无源码噪音）由方案 2 以 80% 的效果、20% 的成本逼近。

### 方案 2：块级实时渲染（Obsidian Live Preview）——**Phase 3，中-大成本**

布局 = 一个纵向 block 列表：光标所在块渲染为**源码编辑态**（一个 multiline input 实例），其余块用 markdown 渲染态。EUI-NEO 现状支撑度：

- 可行：markdown 块渲染器可直接复用（§2.3）；小 input 实例轻量（状态就是个 InputState）。
- 缺口 1：块 ↔ 源码区间映射。md4c Block 无 offset——需自研分块扫描器（按空行+块前缀启发式，约 200-300 行纯函数，可单测）或上游给 markdown 组件加 offset 暴露。**自研更可控**：编辑器本来就要行级信息（大纲、高亮），一个"行分类扫描器"可以同时喂三处。
- 缺口 2：**undo 跨块断裂**。每个编辑块是独立 InputState，撤销栈不共享——切块后 Ctrl+Z 只撤当前块。这是 Phase 3 的核心设计难点（方案：模块层记块级快照栈，Ctrl+Z 走模块栈并整块替换）。
- 缺口 3：焦点流——上下箭头跨块、块尾 Enter 产生新块、点击渲染块切换为编辑态，都是模块层状态机，工作量在此。
- 成本：**大**（保守估：分块器 + 混排布局 + 焦点状态机 + 块级 undo，2-3 周含联调）。
- 体验收益：最接近 Typora 的"写作无源码噪音"。

### 方案 3：同步滚动双栏——**Phase 1，小成本**

现状已是编辑/预览双栏，只差两件事：滚动位置映射 + 光标所在处跟随。EUI-NEO 侧零改造（§2.4）。收益/成本比全场景最高。

### 方案 4：模式切换——**Phase 1 顺带，小成本**

segmented 控件（已有组件）三态：仅编辑 / 双栏 / 仅预览，加快捷键 Ctrl+Shift+P 循环。给"无干扰写作"提供最便宜的入口（Typora 的极简单栏 = 我们的"仅编辑"+隐去左栏）。

### 推荐路线（一句话）

**Phase 1 双栏同步滚动+模式切换+大纲+快捷键+保存反馈（1 周内，体验收益最大）→ Phase 2 编辑器语法高亮+表格 MVP（上游 input 改造，1-2 周）→ Phase 3 块级实时渲染（2-3 周，Obsidian 模式逼近 Typora）；真 WYSIWYG 永久排除。**

---

## 4. 隐含 UX 清单（现状 / 差距 / 成本 / 阶段）

| # | 机制 | Typora 的做法 | DevDesk 现状 | 差距 | 成本 | 阶段 |
|---|------|--------------|--------------|------|------|------|
| 1 | 打字→预览延迟 | 即时（本来就是渲染态） | **已即时**：无预览防抖，每帧 compose + markdown 源串缓存，输入当帧更新预览（400ms 是**落盘**防抖，不是预览防抖） | 小：同长度编辑时预览高度不刷新（§2.6 bug）；无"打字时预览闪动"问题 | 小 | P1（bug 修复） |
| 2 | 滚动同步 | N/A（单栏） | 两栏完全独立，各滚各的 | **大**：这是双栏伪 WYSIWYG 的命门。做块锚点映射（光标行→所在块→预览滚到该块） | 小-中 | P1 |
| 3 | 无干扰 | 默认无侧栏无工具栏，打字时 chrome 收敛 | 三栏常驻 + 顶栏标题/按钮 + 每栏小标题（"编辑中（自动保存）"） | 中：模式切换给"仅编辑"态；编辑态下隐去每栏的 filename 小标题，把信息移到状态栏 | 小 | P1 |
| 4 | 光标视觉 | 闪烁 caret、选区高亮清晰 | caret **不闪烁**（静态 1.5px 竖线）、选区 0.24 alpha、无当前行高亮 | 小-中：当前行高亮 = 模块层画一条 rect（InputState 有 cursorLine 可算）；闪烁需上游小改（动画 tick） | 小 / 中 | P1（行高亮）/ P2（闪烁，可放弃） |
| 5 | 键盘流 | Ctrl+B/I/K、回车续列表、Tab 缩进，全程不离键盘 | Tab 无效、无任何 Markdown 快捷键、回车不续列表 | **大**：这是"编辑 Markdown"与"编辑纯文本"的差别。阻塞点 = input 无按键 passthrough | 中（含上游小 PR） | P1 |
| 6 | 新笔记即时性 | Ctrl+N 直接可打字（默认名） | 点「+新建」→弹窗→输名→创建→打开→**手动点进编辑器**才可打字（约 6 步，且新建后编辑器没有焦点） | 中：新建后自动 focus 编辑器；支持默认名快速创建；弹窗输入框已有 onEnter ✓ | 小 | P1 |
| 7 | 保存的安心感 | 标题栏圆点 + 自动保存，状态可见 | 唯一反馈是**顶栏 subtitle 文字**（"已保存 xxx"），在视线外；无 dirty 指示，用户不知道"有没有存上" | 中：编辑器右下角状态点（●未保存/✓已保存+时间），close 中也表明自动保存语义 | 小 | P1 |
| 8 | 表格/代码块编辑 | 表格可视化、代码块高亮 | 手写管道语法；代码块预览单色 | 大（完整版）。分级：模板插入+Tab 跳格（MVP）→ 行列增删（V1）→ 可视化编辑（V2，依赖 Phase 3 块架构） | 中-大 | P2（MVP/V1）/ P3（V2） |
| 9 | 大纲导航 | 侧栏大纲，点击即跳、当前节高亮 | 无大纲。md4c Block 无 offset，但**行扫描 `^#{1,6} ` 即可**（标题识别不需要 md4c） | 中：面板 + 点击跳转（跳转要写 InputState，见 §5.3）+ 当前节高亮 | 小-中 | P1 |
| 10 | 大文档性能 | 惰性渲染 | 输入侧设计过（增量段落+可视行）；预览侧每键离屏全量测高、无虚拟化 | 未知：**>5000 行需实测**；若卡，缓解 = contentKey 加内容 hash 之外再按块数变化才重测 / 预览防抖 100ms（见 §9 开放问题） | 小（实测）/ 中（虚拟化，仅实测卡才做） | P1 实测 |
| 11 | 列表续行 | 回车自动续 `- `、`1.`，空项回车退出 | 直接插 `\n` | 中：需按键通道（同 #5），逻辑本身是纯函数可单测 | 小-中 | P1 |
| 12 | 选中即预览 | 选中文字时预览同步高亮（单栏才有） | 双栏无此机制 | 不做（双栏架构下成本>收益），Phase 3 后自然获得 | — | P3 附带 |

**最贵的三条**：#8 表格完整版（可视化编辑依赖块架构）、#5+#11+#9 的按键通道（一次上游 input PR 解锁一串体验）、#2 滚动同步做准（块锚点映射的精度决定双栏模式可信度）。

---

## 5. 功能项设计

### 5.1 编辑器内语法高亮（Phase 2，触发 EUI-NEO 上游 PR）

**上游改造范围（一个 PR，EUI-NEO 仓）**：

1. `InputState` 增加 `std::vector<StyledRun> lineStyles`（或回调 `std::function<std::vector<StyledRun>(const std::string&, int lineStart)>`，由使用方注入，EUI-NEO 不懂 Markdown——保持框架中立）。
2. input 渲染行时按 runs 切成多个 `ui.text` segment，手工定位（模式照抄 markdown 组件的 `renderInlineSegment`）。
3. **caretX/选区映射**：每行 metrics 改为按 run 分别 shaping 后拼接（`byteIndices`/`caretX` 数组跨 run 缝合）。注意：跨 run 边界丢失 kerning/ligature，等宽/中文场景误差可忽略，需在 PR 里注明这个近似。
4. 纯 Markdown 之外的场景（日志着色、代码编辑）同样受益——符合"通用 UI 能力缺失 → 修上游"政策，PR 附 example + 测试。

**DevDesk 侧**：一个 ~200 行的行级词法高亮器（纯函数：输入行文本，输出 styled ranges）：标题标记 `#`、粗体 `**`、斜体 `*`、行内代码 `` ` ``、链接 `[]()`、列表符号 `-`/`*`/`1.`、引用 `>`、表格管道 `|`。不引第三方，状态机扫行即可（跨行构造如代码块围栏 ``` 用"行分类扫描器"的块状态，与大纲/Phase 3 分块器共用——**一鱼三吃**）。

**着色方案（对齐 Theme token，明暗两套）**：

| 元素 | Dark 值 | 来源 token | Light 值 |
|------|---------|-----------|----------|
| 正文 | `#FAF9F5` | textPrimary | `#1F1D1A` |
| 标题文本 | `#FAF9F5` + weight 700 | textPrimary | `#1F1D1A` + 700 |
| 标记符号（`#` `**` `\|` `>`） | `#9A948A` | textTertiary | `#9A948A` |
| 行内代码 | `#788C5D`（底 `#3A2620`） | success / primarySoftBg | `#5E7048`（底 `#F5E6DE`） |
| 链接 | `#6A9BCC` | info | `#4A7AAB` |
| 斜体 | `#6A9BCC` | info（斜体字形暂无 → 用颜色区分） | `#4A7AAB` |
| 列表符号 | `#D97757` | primary | `#D97757` |
| 引用行 | `#C9C5BD` | textSecondary | `#6B6660` |

原则：标记符号一律弱化（tertiary），内容主体保持正文色，语义色只给内容（代码=success、链接=info）——和预览侧 `MarkdownStyle` 的用色语义一致，双栏同屏不冲突。斜体无 italic 字形（EUI-NEO 未支持斜轴），以颜色替代，PR 文档注明。

**代码块语法高亮（编辑器+预览）**：v1 只做围栏内**统一 codeText 色 + 语言标签着色**（预览已显示 lang 首行）。真语言 tokenize（关键字/字符串/注释多色）需要每语言状态机，**列为 P3 或不做**（成本大、收益边际递减，Typora 靠 CodeMirror 才有）。诚实评级：大。

### 5.2 快捷键（Phase 1，依赖一个小的上游 PR 或全局 handler 备选）

| 键 | 行为 | 实现要点 |
|----|------|----------|
| Ctrl+B / Ctrl+I | 选区包裹 `**` / `*`（再按=去包裹） | 纯文本变换：`wrapSelection(text, sel, marker)` 可单测 |
| Ctrl+K | 选区 → `[sel](url)`，光标落在 url 处 | 同上 |
| Enter | 列表续行：上一行是 `- `/`* `/`N. ` → 新行自动补前缀；空列表项 → 删除前缀退出 | `listPrefixFor(line)` 纯函数 |
| Tab / Shift+Tab | 多行编辑内插入 4 空格缩进 / 反缩进（选区多行整块缩进） | 上游 input 原生不支持 Tab（§2.5），必须随 PR 加 |
| Ctrl+Shift+P | 循环切换 编辑/双栏/预览 | 模块层，无依赖 |

**实现通道二选一**：

- **首选（上游 PR #1，小）**：`InputBuilder::onKeyEvent(std::function<bool(const KeyEvent&, InputState&)>)` passthrough——内置处理之前调用，返回 true 即拦截。通用能力（任何嵌入 input 的业务都需要），一个 PR 附 example+测试。同时在这个 PR 里补 multiline Tab 缩进。
- **备选（零上游依赖，hack）**：app 级 `onKeyEvent`（全 app 唯一）+ Router 分发到当前模块，模块直接改 `ui.state<InputState>("md.editor.input")`（状态是公开可访问的）。能用，但绕过 input 的 undo 快照栈（需手动 `pushUndoState`），且全 app 单 handler 的架构会被越用越脏。**若 PR #1 排期受阻可先用此通道落地 P1 快捷键，PR 合入后迁移**。

### 5.3 滚动同步（Phase 1，纯 DevDesk 层）

- **映射算法（块锚点）**：行扫描器产出 `(lineNo → blockIndex)` 表；markdown 侧用 `estimateHeight(源码前缀至块 i)` 得每块预览 y（public API 带缓存，§2.3）。编辑器滚动/光标变化时：找光标（或视口顶行）所在块 i → 预览 offset 设为块 i 的 y（带平滑 clamp）。
- 反向（滚预览带动编辑器）v1 不做——双向映射易震荡，Typora 单栏无此问题，双栏下单向（编辑驱动预览）已覆盖 90% 场景。
- 编辑器滚动读取：`ui.state<InputState>("md.editor.input").verticalScroll / cursor`（每帧 compose 顺手读，无性能成本）；预览驱动：`scrollView.offset(y)` 直接传值。
- **精度风险**：`estimateHeight` 是估算（平均字宽），累积误差可能到半屏——缓解：滚动目标 y 用"锚点块 y - 视口高 20%"并 clamp；若实测漂移大，P2 给 markdown 组件上游 PR 加"块高度实测回调"（渲染后回填真实 frame 高度替换估算）。

### 5.4 大纲面板（Phase 1）

- 数据：行扫描 `^#{1,6}\s` 生成 `(level, lineNo, title)`——**不用 md4c**（Block 无 offset，且大纲只需行级识别）。扫描器在 `onChange` 时增量维护（只重扫变化段落，照抄 `updateChangedParagraphs` 的前后缀 diff 思路）。
- UI：左栏顶部 segmented（`文件 / 大纲`）切换，大纲条目按 level 缩进 16px/级，行高 30px，当前节（光标行所在节）高亮 `primarySoftBg` + 标题 `primary`。
- 点击跳转：写 `InputState.cursor` 到目标行行首 + `followCaret=true`（编辑器自动滚到光标，已有机制 `syncVerticalScroll`）→ 预览随滚动同步链路自动跟随。跳转 API 走 PR #1 顺带的 `.cursor(int)` / `.scrollToLine(int)`（或暂用状态直写的备选通道）。
- 标题极多（>300 条）时换 `virtualList`（固定行高，现成）。

### 5.5 表格编辑器（分级）

- **MVP（P2）**：编辑器右键（contextMenu，现成组件）"插入表格"→ 级联选行列（3×3 起）→ 在光标处插入管道模板（表头行 + 分隔行 + 2 空行）。Tab 在表格行内 = 跳到下一单元格（在管道间移动光标，纯字节查找，配合 PR #1 的 Tab 处理）。**成本小**。
- **V1（P2 尾）**：预览侧表格块悬浮显示"+行 +列 −行 −列"小按钮（markdown 表格已是独立 stack，加 MouseArea 即可），操作 = 模块层对源码对应行做文本变换（在行尾补 `\| 新格`、复制行插入）。因为编辑在源码侧、显示在预览侧，**无需富表格控件**。成本中。
- **V2（P3，依赖块架构）**：表格块进入编辑态时渲染为真正的网格编辑器（每格一个小 input）。成本大，Phase 3 之后再评估。

### 5.6 工具栏 / 状态栏

- **不加格式工具栏**——Typora 是无工具栏极简派，快捷键 + 右键菜单已覆盖。加工具栏是把"键盘流"问题用鼠标方案绕，方向反了。
- **加轻状态栏**（P1）：编辑器底部 22px 行：左 = 保存状态（`● 未保存` / `✓ 已保存 14:32`，色用 warning→success 过渡），右 = `行:列 · N 字`。数据全部来自 InputState，成本小。保存状态把"自动保存"从 subtitle 文字升级为视线内的持续反馈（隐含 UX #7）。

### 5.7 模式切换与无干扰（P1）

- 顶栏 segmented：`✎ 编辑 | ⇔ 双栏 | 👁 预览`；`Ctrl+Shift+P` 循环。
- 仅编辑态：隐藏右栏，编辑器全宽 + 居中 720px 文本列（Typora 的写作宽度）；左栏保留可折叠。
- 双栏态（默认，当前布局）：加滚动同步。
- 仅预览态：阅读用，隐藏编辑器。
- 各态下都去掉栏内小标题（"xxx · 编辑中（自动保存）"），信息归状态栏——chrome 密度对齐 Typora。

---

## 6. UI 视觉方案

### 6.1 线框（双栏默认态）

```
┌──────────────────────────────────────────────────────────────────────────┐
│  Markdown 笔记本                        [✎ 编辑|⇔ 双栏|👁 预览]  ↻  +新建 │ 64px
├────────────┬──────────────────────────────┬──────────────────────────────┤
│ 🔍 搜索…    │                              │                              │
│────────────│  # 项目周报                    │  项目周报          (h1 24px) │
│ [文件|大纲] │                              │                              │
│  📄 周报    │  ## 本周进展  ← 光标在此块     │  本周进展          (h2 20px) │
│  📄 会议    │  - 完成滚动同步                │  • 完成滚动同步               │
│  📄 ideas  │  - 修了高度缓▊                 │  • 修了高度缓存 bug           │
│            │    存 bug                     │                              │
│ ── 大纲 ──  │  ```cpp                       │  ┌────────────────────┐     │
│ ▸ 项目周报  │  void foo();                  │  │ cpp                │     │
│   本周进展 ◀│  ```                          │  │ void foo();        │     │
│   风险      │                              │  └────────────────────┘     │
│   下周计划  │  | 任务 | 状态 |              │  ┌──────┬──────┐           │
│            │  |---|---|                    │  │ 任务 │ 状态 │  ← +行+列  │
│            │                              │  ├──────┼──────┤           │
│            │                              │  │ ...  │ ...  │           │
│            │                              │  └──────┴──────┘           │
│            │                              │              ▲ 同步滚动跟随   │
│            │ ✓ 已保存 14:32     12:3 · 48字│                              │
│            │ ────────── 状态栏 22px ────── │                              │
└────────────┴──────────────────────────────┴──────────────────────────────┘
     240px              均分剩余                    均分剩余
```

仅编辑态：右栏消失，编辑器全宽、文本列 clamp 720px 居中，状态栏随行号列底部固定。仅预览态：同布局，预览全宽 760px 居中。

### 6.2 编辑器语法高亮着色示例（Dark）

```
# 项目周报          ← '#' #9A948A / '项目周报' #FAF9F5 w700
## 本周进展         ← '##' #9A948A / 正文 #FAF9F5
- 完成滚动同步      ← '-' #D97757 / 正文 #FAF9F5
`std::string`      ← 整段 #788C5D，底色 #3A2620（圆角 4px）
**重要**           ← '**' #9A948A / '重要' #FAF9F5 w700
[链接](url)        ← '[]()' #9A948A / '链接' #6A9BCC
> 引用行           ← '>' #9A948A / 正文 #C9C5BD
| a | b |          ← '|' #9A948A / 单元格 #FAF9F5
```

### 6.3 配色对齐

全部取自 `core/theme.h` 现有 token（§5.1 表），不新增颜色语义；高亮底色用 `primarySoftBg`（`#3A2620`/`#F5E6DE`）保证与品牌 Clay 体系一致。当前行高亮 = `hover`（`#353231`）30% 透明度，避免与选区色（primary 24% alpha）打架。

---

## 7. 变更清单预估（按阶段）

**Phase 1（全部 DevDesk 层 + 可选小上游 PR，估 4-6 个工作日）**

| 变更 | 文件 | 规模 |
|------|------|------|
| 滚动同步（块锚点映射） | `markdown_module.cpp` 新增 ~120 行 | 中 |
| 行分类扫描器（大纲+高亮+分块共用底座） | 新文件 `md_line_scanner.{h,cpp}` ~250 行 | 中 |
| 大纲面板（segmented + 列表 + 跳转 + 当前节高亮） | `markdown_module.cpp` ~150 行 | 中 |
| 快捷键 + 列表续行 + Tab 缩进 | `markdown_module.cpp` ~150 行（走通道 A 或 B） | 中 |
| 模式切换三态 + 无干扰清理 | `markdown_module.cpp` ~80 行 | 小 |
| 保存状态栏 + 新建即聚焦 + contentKey bug 修复 | `markdown_module.cpp` ~60 行 | 小 |
| （上游 PR #1：InputBuilder onKeyEvent passthrough + Tab + cursor/scrollTo API） | EUI-NEO `input.h` ~80 行 | 小-中 |

**Phase 2（上游 PR #2 + DevDesk 高亮器，估 1-2 周）**

| 变更 | 文件 | 规模 |
|------|------|------|
| 上游 PR #2：input styled runs + caret 跨 run 缝合 | EUI-NEO `input_model.h/input.h` ~300 行（含测试） | 中-大 |
| DevDesk 行级 Markdown 高亮器 | `md_highlighter.{h,cpp}` ~200 行 | 中 |
| 表格 MVP（右键插入 + Tab 跳格） | `markdown_module.cpp` ~100 行 | 小 |
| 表格 V1（预览侧行列增删按钮） | `markdown_module.cpp` ~150 行 | 中 |
| caret 闪烁（上游小改，可选） | EUI-NEO input | 小 |

**Phase 3（块级实时渲染，估 2-3 周）**

| 变更 | 文件 | 规模 |
|------|------|------|
| 块↔源码区间映射（基于行扫描器扩展） | `md_line_scanner` 扩展 ~150 行 | 中 |
| 混排布局（渲染块 + 活跃块 input） | `markdown_module.cpp` 重构编辑区 ~400 行 | 大 |
| 焦点/光标跨块状态机 | 同上 ~200 行 | 大 |
| 块级 undo（模块层快照栈） | 同上 ~100 行 | 中 |
| 预览性能实测 + 视情况虚拟化/防抖 | EUI-NEO markdown（可选上游 PR #3） | 未知 |

---

## 8. 测试计划

- **纯函数全单测**（doctest 迷你框架，REGISTER_TEST 风格照现有 109 用例）：`wrapSelection`/`listPrefixFor`/行扫描器（标题/代码围栏/表格行识别/边界：文首文尾、CRLF、UTF-8 中文标题）、滚动映射表生成、表格源码变换（增删行列 round-trip）。
- **上游 PR 各自带** EUI-NEO example + 测试（PR 准备清单：example、向后兼容、docs、独立编译）。
- **集成手测脚本 + 截屏**（项目铁律：未截屏=未完成）：三态模式切换、滚动同步跟随、大纲跳转、快捷键包裹/续行、表格插入、保存状态流转（输入→400ms→落盘→状态点变化）、5000 行文档打字帧率（开 debug title FPS 统计）。
- **回归**：109 基线用例全绿；IME 中文输入（拼音预编辑→上屏）在每个阶段手测——input 状态被模块直写的通道必须过 IME 场景。

---

## 9. 开放问题与风险

1. **预览大文档性能未实测**（§2.6）：每键离屏全量测高 + 无虚拟化，>5000 行可能掉帧。P1 先实测；缓解选项：预览重排防抖 100ms（牺牲一点 #1 的即时性）、contentKey 按块结构而非 size、极端情况才做虚拟化。
2. **滚动同步精度**：estimateHeight 估算误差可能累积到可见漂移。P1 用锚点+clamp 缓解，实测不行则 P2 上游加块高实测回调。
3. **undo 语义分裂风险**：模块层直写 InputState（大纲跳转、快捷键变换）若忘记 `pushUndoState`，Ctrl+Z 行异常。规约：所有文本变换入口统一走一个 `editText(fn)` 帮助函数，先快照后改。
4. **Phase 3 块级 undo/焦点状态机**是本项目最大不确定性，若 2 周内原型不顺，降级承诺：保留双栏模式为长期形态（Obsidian 用户中 Live Preview 与双栏并存也是常态）。
5. **斜体无字形**：EUI-NEO 无 italic 轴，用颜色替代是否有歧义（info 蓝既表链接又表斜体）——P2 截屏评审时定，必要时斜体改用下划线装饰（markdown 组件已有 decor rect 先例）。
6. **YaHei 权重切换未验证**（§2.7）：只影响编辑器粗体显示效果，不阻塞设计，P2 实测。
7. **上游 PR 排期**：PR #1/#2 都改 EUI-NEO 核心 input，需遵守上游政策（example+测试+向后兼容）；若上游节奏不可控，PR #1 有 app 级 handler 备选通道，PR #2 没有备选（高亮必须进 input 渲染）——**PR #2 是整个路线图唯一无退路的依赖**。
