# DevDesk Markdown Phase 3「块级实时渲染」设计（Live Preview）

- 日期：2026-09-27
- 状态：**设计草稿（未实施）**；前置「Phase 3 首块 ①～⑤」已合 master（`f3901a1`/`0bd5ab4`/`b2a2413`/`e3bb177`/`11e2566`）
- 范围：`module_markdown` 的**单栏块级实时渲染视图**（Obsidian Live Preview 形态）+ 必要的 DevDesk 侧纯函数层；**本期零 EUI-NEO 上游改动**（与 Phase 1/2 的差别）
- 依据（实读源码，非推断）：
  - `docs/superpowers/specs/2026-09-22-markdown-editor-ux-design.md` §3 方案 2（选定路线）、§4（隐含 UX 清单）、§7 Phase 3、§9 风险
  - `../EUI-NEO/components/input.h`、`input_model.h`、`mousearea.h`、`scrollview.h`、`core/dsl.h`、`core/runtime/runtime_input.h`
  - `src/module_markdown/*`（块模型/几何表/交互/高亮/键盘流现状）
  - `docs/KNOWN_LIMITATIONS.md` #9（上游解析缓存 LRU 8）、#8（组合重音跨行）

---

## 1. 需求

### 1.1 目标

在**单栏**中实现「光标所在块 = 源码编辑态，其余块 = 渲染态」的块级实时渲染（ux-design §3 方案 2 的本体），逼近 Typora 的"写作无源码噪音"体感：

1. 单栏内直接渲染，块间隙与渲染观感与现有预览一致（同源渲染器）；
2. 进入/离开任意块无需切换模式，光标移动即换块；
3. 点击渲染块 → 该块变源码态且光标落到对应行（复用 ⑤ 已有反查）；
4. 滚动、光标跟随、IME、快捷键、状态栏、大纲、标签、自动保存**全部不退化**；
5. 既有三态（编辑/双栏/预览）**一个都不删**，实时为新增第四态（可一键退回）。

### 1.2 非目标（明确不做什么）

| 不做 | 理由 |
|------|------|
| 真 WYSIWYG（contenteditable 式富文本引擎） | ux-design §3 方案 1 已**永久排除**（EUI-NEO 无 rich-run 原语、无富文本文档模型，月级工程） |
| 富文本 span 级样式编辑（在渲染态里直接改粗体边界） | 同上；渲染态只读，点击即整块变源码态 |
| 非活动块的**行内**内联编辑（只换该行的源码） | 需要行级块拆分，破坏「逐块 == 整篇」等价性（本设计唯一的正确性基石） |
| 跨块选区 / 多光标 / 块拖拽排序 | 与"块 = 独立编辑单元"模型冲突，收益远低于成本 |
| 表格网格编辑器（ux §5.5 V2） | 继续延后，不在本期 |
| 非活动块内链接可点 / 图片缩放 | 上游 `components::markdown` 无命中/指针 API（KNOWN #9 补⑤），本期不引入自绘 markdown |
| 上游改动（input 自动高、markdown 块级 API） | 本期目标"零上游依赖"；两者都只作为 R 项的兜底升级路径 |
| `markdown_service` / FTS / 标签 / watcher / 保存语义变更 | 存盘仍是**整篇文本**写入，服务层零改动 |
| 新增墙钟性能红线测试 | 既有 `md_preview_highlight:310` / `md_highlight:622` 的 `<8ms` 红线已是登记脆弱点（编排板 #8）；本期性能验证走**记录式日志**，见 §5 |

### 1.3 已就绪的前置资产（复用，不重建）

| 资产 | 位置 | 本期用途 |
|------|------|----------|
| 块级分块器（7 类块 + `stableId` 跨编辑身份 + 脏区间） | `md_block_model.{h,cpp}` | 活动块推导、渲染键控、脏块缓存 |
| 逐块渲染 + 等价性回归锚 | `markdown_module.cpp:1355+`、`md_block_model_render_equivalence` | 非活动块渲染（**与预览同一条路径**） |
| 块几何表（`blockTopY/blockHeight/blockFirstLine/blockLineCount` + 正反查） | `md_sync_map.{h,cpp}` | 滚动跟随、点击反查的同一张表 |
| 预览→编辑器跳转（行首字节） | `md_block_interact.{h,cpp}` | 点击渲染块定位 |
| 方项 hover 状态机 | `MdPreviewHoverState` | 渲染块 hover |
| 行级高亮器（块局部坐标可用） | `md_highlight.{h,cpp}` | 活动块源码态着色 |
| CodeFence 自绘 + 色表 | `md_preview_highlight.{h,cpp}`、`md_keyword_highlight` | 渲染块代码块多色 |
| 键盘流纯函数（`EditBuffer` 块局部语义） | `md_keyboard.{h,cpp}` | Tab/列表续行/包裹/表格跳格**原样复用** |
| 视图状态持久化（mode 已持久化） | `md_view_state.{h,cpp}` | 第四态落盘 |
| 无头 compose 测试基座 | `tests/test_modules/test_markdown.cpp` | 新增用例（见 §8.2） |

---

## 2. 现状差距（源码审计结论）

### 2.1 EUI-NEO 能力核对表（逐项带出处，只写确实支持的）

| 能力 | 结论 | 出处 | 对本期的影响 |
|------|------|------|--------------|
| input 多实例可共存 | ✅ `ui.state<InputState>(id)` 按 id 惰性建、跨帧持久 | `dsl.h:1526`、`StateStore::get` | 单输入方案下只需 1 个；多实例可用但**不采用**（见 4.1 D1） |
| 编程式聚焦 | ✅ `ui.requestFocus(id)`，帧末消费；**该帧树中没有该元素则丢弃**；派发回调内/compose 内调用均安全 | `dsl.h:1516-1526` | 点击渲染块→聚焦活动块沿用 ⑤ 点击定位同款接线（回调内改状态 + 发请求）；"元素不存在即丢弃"使**状态变更必须先于消费该请求的那次 compose** 成为硬约束（V1/V2） |
| 状态清理 | ✅ `ui.releaseStateScope(id)` 按前缀释放；否则 `stateStore_` 只增不减 | `dsl.h:1532`、`StateStore::releasePrefix` | 块切换时释放旧块状态（R7） |
| 内容贴合高度可行 | ✅ 高度是显式 `.size(w,h)`；`contentHeight = 行数 × fontSize×1.2`；`maxVerticalScroll = max(0, contentHeight − viewportHeight)` | `input.h:51/128`、`input_model.h:141-153` | 传 `h == contentHeight` 即"内容贴合" |
| **贴合高度 ⇒ 滚轮自动穿透 + 无滚动条** | ✅ 滚轮处理器**仅在 `maxVerticalScroll > 0` 时才注册**；滚动条同样只在 `> 0` 时绘制 | `input.h:218-227`、`input.h:496` | **本期最关键收益**：活动块不吃滚轮，外层 scrollView 正常滚 |
| 纯函数行数测量 | ✅ `InputModel::measureLines(value, fontFamily, fontSize, viewportWidth)` public static、纯 | `input_model.h:728` | 精确行数 → 精确高度，无需 guess |
| 光标/选区/IME 可编程读写 | ✅ `InputState{cursor, selectionStart/End, followCaret, verticalScroll, textRevision, compositionText, preedit}` 模块可直接读写（既有做法） | `input_model.h:49-95`、`markdown_module.cpp:1028` | 光标锚、切块换文本 |
| 按键 passthrough 先于内置 | ✅ `.onKeyEvent` 返回 true 即拦截（含 Ctrl+Z/Y） | `input.h:229-232` | 模块层 undo / 边界键 |
| 行级高亮按行回调 | ✅ `.lineStyles((displayText, lineStartByte, lineNo) → runs)`，颜色 run 不改 layout/caret | `input.h:107`、styled-runs 设计 §5 | 活动块高亮（喂**块文本**，见 4.6） |
| 焦点回调 | ✅ `.onFocus(std::function<void(bool)>)`、`ui.isFocused(id)` | `input.h:91`、`dsl.h:1509` | 失焦提交、活动块判定 |
| 滚轮路由 | `hitTestScrollable` 取**最上层**带 `scrollStateId` 或 `onScroll` 的元素 | `runtime_input.h:67-69` | 透明 mouseArea 无 onScroll → 不劫持滚动（⑤ 已验证） |
| 元素树每帧重建、按 id 查找 | ✅ `ui.begin()` 每帧重建；`ui.find(id)` 帧内查 | 既有 compose 模式 | 渲染键控靠 id |
| input 内部滚动 | ✅ 存在（`state.verticalScroll`），但**内容贴合时无效** | `input.h:220-226` | 超大单块的兜底（R4） |

### 2.2 待验证项（**不确定，先标，附验证方法**）

| # | 待验证 | 为什么关键 | 验证方法 |
|---|--------|-----------|----------|
| V1 | **稳定 id 的 input 跨帧改变树中位置/父级**是否被 runtime 支持（元素身份、焦点保持、脏区正确） | 单输入方案的立身之本：活动块换位时 input 要"搬家" | M0 spike：无头 compose 两帧，把同一 id 的 input 从 column 第 k 位换到第 j 位，断言 `ui.find`、`ui.isFocused`、`ui.state<InputState>().text` 均存活，且键盘事件仍进该实例 |
| V2 | 焦点元素在上一帧的父级被合并/删除时，`focusedId_` 是否被清除 | 块合并（Backspace 跨块）瞬间可能导致焦点丢失 = 打字丢失 | M0 spike 同上 + 手工（Backspace 合并两段后继续打字）；兜底 = 切块帧再发一次同帧 `requestFocus` |
| V3 | input 作为 **scrollView content 内的流式元素**（非绝对定位）时：尺寸参与流式布局、pointer→caret 映射（`lastBounds`）、IME 候选框定位（`imeRect`）在滚动偏移下正确 | Live 的 input 必须在滚动容器内 | M0 spike：滚动到中途后点击 input 中部，断言 caret 落在点击处；IME 候选框贴近 caret（手工） |
| V4 | 换文本瞬间 `verticalScroll/followCaret` 残余是否造成一帧跳位 | 切块观感 | M0 spike + 手工：切块后首帧截图比对 |
| V5 | 逐块高度恒等（`estimateHeight` == 实际布局）在 Live 下更敏感（错位表现为覆盖层/输入框错位） | 继承自 ④/⑤ 的假设，编排板已列"未实测" | DEBUG 断言 + 日志（见 §5 第 4 行）；不符则仅影响覆盖层，编辑器落点不受影响 |

---

## 3. 交互设计

### 3.1 模式与入口

- 顶栏 segmented 从三项扩为四项：**编辑 | 实时 | 双栏 | 预览**（`md.mode`，`markdown_module.cpp:359`）。
- `ViewMode` 枚举**追加** `Live = 3`（既有 `Edit=0 / Split=1 / Preview=2` 语义与已持久化文件完全不变 → 免迁移）；segmented 视觉顺序与枚举顺序解耦，用显式映射表 `{Edit, Live, Split, Preview}` + 逆映射（`segmented.onChange` 不再 `static_cast`）。
- 快捷键 `Ctrl+Shift+P` 循环顺序：编辑 → 实时 → 双栏 → 预览 → 编辑。
- 第四态随 `md_view_state.json` 持久化（`kMaxMode` 2 → 3，`sanitized()` 域随增）。
- 左树 / 大纲浮动面板 / 标签栏 / 状态栏 / 顶栏动作：四态共享，**零改动**。

### 3.2 单栏块级布局

```
┌──────────────────────────────────────────────────────────┐
│  Markdown 笔记本        [编辑 | 实时 | 双栏 | 预览] ↻ +新建 │
├────────────┬─────────────────────────────────────────────┤
│ 🔍 搜索…    │  # 项目周报                    ← 渲染块（只读）│
│ [文件|大纲] │                                             │
│  📄 周报    │  本周进展                      ← 渲染块（只读）│
│  📄 会议    │                                             │
│            │  ## 本周进展▊                  ← 活动块：源码态│
│            │  - 完成滚动同步                  （高亮 + caret）│
│            │  - 修了高度缓存 bug                            │
│            │                                             │
│            │  ┌────────────────────┐        ← 渲染块（只读）│
│            │  │ cpp  void foo();   │                       │
│            │  └────────────────────┘                       │
│            │  ✓ 已保存 14:32      12:3 · 48字   ← 状态栏复用 │
└────────────┴─────────────────────────────────────────────┘
     240px            文本列 clamp 720px 居中（§kEditTextColumnWidth）
```

- 内容列宽度复用 `kEditTextColumnWidth`（720px，仅编辑态/实时态才 clamp 居中）。
- 块间隙复用 `MarkdownStyle.blockGap`（与预览逐块路径同源）。
- 渲染块沿用 ⑤ 的 wrapper 结构（固定高 stack + hover 底衬 + 透明 MouseArea），几何取自 `MdSyncMap` —— **Live 与预览共用同一套渲染与几何代码**，两者观感必须一致（手工点验 ⑨）。
- 活动块位置不放 wrapper 的渲染内容，改放 input（内容贴合高度），保留边框/圆角以标记"正在编辑"。
- 空文档 / 无笔记 / 加载失败：沿用现有 hint 文案路径。

### 3.3 光标与滚动同步

- **单一滚动权威**：外层 `scrollView`（`md.live.scroll`）。活动块 input 内容贴合 → 其内部无滚动、不吃滚轮（§2.1 证据）。
- `liveScrollY_` 由模块持有：用户滚轮 → `scrollView.onChange` 写回并置 `liveFollow_ = false`；打字 / 光标移动 / 点击定位 → 置 `liveFollow_ = true`。
- **光标跟随公式**：`caretDocY = syncMap_.blockTopY(k) + cursorLineInBlock × lineH`；仅当 caret 落在视口 `[0.15H, 0.75H]` 之外时把 offset 拉到 `caretDocY − 0.4H`（一次性，不逐帧强制，避免与用户滚动打架）。
- `contentKey` 必须用**内容 hash** + 宽度（照抄预览的 `previewTextHash_` 做法，`markdown_module.cpp:1313`）：ux-design §2.6 记录的既有 bug 是早期按 `text.size()` 键控——同长度替换字符时高度缓存不失效；Live 不得回退到该写法。
- 点击渲染块：复用 `mdResolvePreviewJump` 得目标行 → 文档偏移 → `setLiveCursor()` → 跟随滚动 + 同帧 `requestFocus`。
- 进入实时态（从编辑/双栏/预览切来）：把当前 `InputState.cursor`（或预览态 `activeOutlineIdx_`）作为初始文档偏移，对齐一次；`syncPreviewOnEnter_` 语义在实时态不适用（无第二栏）。

### 3.4 块边界输入行为

**核心决策：块边界不做"块结构操作"，只做普通文本编辑，块身份始终跟随光标。**

- Enter：列表行先走 `applyEnterList`（既有）；其余情况在块内插 `\n`。**按一次 Enter 不新建块**；连续 Enter 产生空行后，分块器重算自然把光标之后的内容划到新块（活动块随光标偏移自动切换，零特判）。理由：保持「逐块 == 整篇」等价性（本设计唯一正确性基石），且符合 Markdown 源码语义。
- Backspace（文档偏移 == 块起点）：删前一个 `\n`；若因此与上一块粘连，光标偏移落入上一块 → 活动块自动跟随（同 Enter，无特判）。
- Delete（文档偏移 == 块终点）：删后一个 `\n`，同理。
- ↑ / ↓ 跨块：光标在活动块**首行**按 ↑（末行按 ↓）→ 跳上一块末行 / 下一块首行（列尽量保持）；首块 ↑ / 末块 ↓ **不消费**（交回 input 内置）。纯函数 `mdLiveVerticalBlockTarget()` 返回目标文档偏移或"不消费"。
- 块内按键：Tab/Shift+Tab 缩进与表格跳格、Ctrl+B/I/K、Enter 续列表 —— 全部走既有 `md_keyboard` 纯函数（其 `EditBuffer` 本就是块局部语义，**零改动**）。
- 跨块选区 / Shift+↑↓ 跨块：不做（非目标）；Shift+方向键在块内照旧。
- IME：`compositionText` 非空期间**冻结块推导**（不换活动块、不换文本），组合结束后一次性重算 —— 防止组字中途被换文本撕裂。

---

## 4. 技术方案

### 4.1 关键选型（含被排除方案）

| # | 决策 | 排除的方案与理由 |
|---|------|------------------|
| D1 | **单输入跟随活动块**：全局只有一个 `md.live.input`（稳定 id），块切换时把它"搬"到活动块位置、换入该块文本 | ❌ 每块一个 input（`md.live.input.<stableId>`）：焦点必须在块身份变化瞬间迁移，而 `requestFocus` 在"目标元素不在本帧树中"时被**丢弃**（`dsl.h:1521`）——打字拆块/合并块的瞬间 = 丢焦点 = 丢输入；IME 在飞时换 id 更危险；且 `stateStore_` 只增不减，须逐块 release。单输入：焦点**永不迁移**，文本流向单点可控 |
| D2 | **内容贴合高度**（`h = inset×2 + measureLines(...).size() × fontSize×1.2 + 1px 余量`），不用内部滚动 | ❌ 固定高 + 内部滚动：会注册滚轮处理器（`maxVerticalScroll > 0`）→ 吞掉外层滚动 = 滚轮"死区"；且块高与内容不符会破坏几何表恒等 |
| D3 | **块身份锚 = 文档字节偏移**（`cursorByte` → `MdBlockModel::blockIndexAt`）；`stableId` 只作渲染键与缓存键 | ❌ 用 `stableId` 作活动块身份：拆块/并块时身份会变，需额外迁移逻辑；偏移锚天然跟随光标 |
| D4 | **文本流向单向**：编辑时 `InputState.text` 是活动块局部文本的唯一权威，`onChange` → 拼整篇（前 + 局部 + 后）→ `setEditorText()`（既有唯一写点）；仅切块/撤销/跳转时**反向写入** | ❌ 双向同步：每键都回写输入框文本会与框架自身编辑/IME 打架（光标跳、组字断） |
| D5 | **撤销走模块层快照栈**（整篇文本 + 光标偏移），在 `onKeyEvent` 拦截 Ctrl+Z/Y | ❌ 依赖框架 per-instance 栈（128 深、跨块断裂 —— ux-design §3 缺口 2 已点名）：单输入方案下它跨块混叠，语义不可解释 |
| D6 | 新增**第四态**，三态全保留 | ❌ 直接改造"编辑"态：Live 的失败面（焦点/IME/滚动锚定）需要用户一键退回；ux-design §9.4 的降级承诺要求双栏长期可用 |
| D7 | **零上游改动** | ❌ 上游加"input 自动高"/"markdown 块级 API"：本期用 `measureLines` + 外层 scrollView 已能达成；二者仅作 R 项兜底 |
| D8 | 复用既有分块器/几何表/反查/键盘流/高亮，新纯函数层只写"活动块 + 高度 + 跨块 + undo" | ❌ 新造行级块模型：会与 ①～⑤ 已锁定的等价性回归锚分叉 |

### 4.2 数据流（单点写，共 3 个）

```
用户击键 → input 内置编辑 → onChange(块局部文本)
   → syncDocumentFromLiveInput(): editorText_ = 前段 + 局部 + 后段
   → setEditorText()  ← 既有唯一文档写点（顺带 blockModel_.update + rev++ + 脏标记 + 落盘防抖）
   → 下一帧 compose: 分块器新 blocks → 活动块 = blockIndexAt(liveCursorByte_)

点击渲染块 / 大纲 / 跨块 ↑↓ / Ctrl+Z
   → setLiveCursor(docByte)  ← 唯一光标写点（更新 cursorByte、按需换 activity、写 input 状态、requestFocus、滚动跟随）
```

- `setLiveCursor()` 与编辑态共用的 `commitEditBuffer()`（`markdown_module.cpp:1028`）职责边界：前者是**文档级**光标权威（实时态），后者是 input 状态的写回；实时态下键盘流仍用 `commitEditBuffer` 写回活动块局部，随后 `syncDocumentFromLiveInput()` 上抛。
- 状态栏 `(行:列:字)` 缓存、保存状态、脏标记、外部冲突 toast：**全部沿用**，键仍是 `(editorTextRev_, cursorByte)`。

### 4.3 块级解析：增量与失效

- 分块 = 既有 `MdBlockModel::update()`（文本相等 → 零重算；否则 O(n) 行扫描 + `(kind, hash)` 双端对齐继承 `stableId`）。**本期不新增分块增量**（区段重扫留给实测超预算再做，见 `md_block_model.h` 头注）。
- 渲染增量 = 逐块 `components::markdown`（键控 id `md.preview.block.<stableId>`）+ CodeFence 自绘缓存（`MdPreviewCodeCache`，键 `(stableId, hash)`）——与预览**同一条路径**，非活动块零新代码。
- 失效条件：① 主题切换 → 高亮/色表重注入（既有机制）；② 宽度变化 → 几何表重建（`MdSyncMap::rebuild` 键含宽度）；③ 字号档变化 → 高度公式输入变 → 活动块高度重算（同帧）；④ 文本变化 → `rev++` → 分块 + 脏块重解析。
- 上游解析缓存 LRU 8 的既有边界（KNOWN #9）**不新增问题**：Live 的非活动块渲染量与预览态完全相同；Live 不引入新的 md4c 调用路径。

### 4.4 渲染层与框架边界

- **DevDesk 负责**：分块、活动块判定、文本上下行、几何/滚动、undo、键位语义、高亮色表、CodeFence 自绘。
- **EUI-NEO 负责**：input 的排版/shaping/caret/选区/IME/undo 内置栈（未用）/滚轮；markdown 的 md4c 解析与块内布局；scrollView 滚动。
- 渲染块与活动块的**互斥**：活动块位置只出 input，不出渲染内容（避免重复渲染与命中歧义）。
- 输入框视觉：沿用编辑态 input 样式（`InputStyle` + `componentTheme`），仅额外保留块边框以标识"正在编辑"。

### 4.5 撤销 / 重做

- `MdLiveUndoStack`（`md_live_edit.h`）：快照 = `(整篇 text, cursorByte)`，容量 200（内存 ≈ 200 × 文档大小，仅在实时态推栈；文档 > 2MB 时降为 50，登记日志）。
- 推栈时机：`syncDocumentFromLiveInput()` 后（**合并连续同块输入**：同一活动块且距上次推栈 < 600ms 视为一次编辑，避免逐键一栈）、块切换前、模式切换前、点击定位不入栈。
- Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z：`onKeyEvent` 拦截（内置栈因此不会被消费），撤销后 `setEditorText()` + `setLiveCursor()` 一次到位。
- 退出实时态不清栈（内存随模式退出释放）。

### 4.6 与现有高亮、预览自绘、全文索引的关系

| 关联方 | 关系 |
|--------|------|
| 编辑器行级高亮 `MdHighlighter` | **独立实例** `liveHighlighter_`，喂**块文本**（块局部坐标 == input 坐标）。理由：`runsForLine` 返回全文档坐标（`lineStartByte + 偏移`），且围栏状态按传入文本等值缓存 —— 与预览/编辑态**混用一个实例**会因两种坐标系交替而抖动。分块器保证围栏不跨块 → 块内围栏状态自洽，高亮语义不退化 |
| CodeFence 渲染块多色 | 复用 `md_preview_highlight` + `MdPreviewCodeCache` + 既有色表，stableId+hash 键控，不新增词法器 |
| 预览态 | 与其共用逐块渲染代码与几何表；两模式观感差异只有"是否单栏/是否有活动块" |
| `markdown_service` / FTS5 索引 | **零改动**：索引与搜索作用于磁盘文件与整篇文本，与视图无关 |
| 保存 / 外部冲突 / watcher | **零改动**：仍走 `editorText_` + 400ms 防抖 + `lastKnownMtime_` 冲突 toast |
| 大纲 / 标签 / 跨模块跳转 | **零改动**（大纲跳转在实时态改调 `setLiveCursor`，其余不变） |

### 4.7 文件改动清单

| 文件 | 动作 | 规模 |
|------|------|------|
| `src/module_markdown/md_live_edit.h` / `.cpp` | **新增**：活动块推导、块局部偏移、高度公式（测量函数注入）、跨块移动目标、undo 栈 —— 纯函数/轻状态机，不 include EUI-NEO 组件头（可单测） | ~70 / ~200 行 |
| `src/module_markdown/markdown_module.cpp` | 扩展：`ViewMode::Live` + 模式映射表 + `drawLiveColumn()` + 接线（`setLiveCursor` / `syncDocumentFromLiveInput` / `liveHighlighter_` / undo 拦截 / 滚动锚定） | ~430 行（新增方法，不动既有渲染装配） |
| `src/module_markdown/md_view_state.h/.cpp` | `kMaxMode` 2→3 + mode 语义注释 | 2 行 |
| `src/module_markdown/CMakeLists.txt` | 登记新源文件 | 1 行 |
| `tests/CMakeLists.txt` | 登记新测试文件（若按文件枚举） | 1 行 |
| `tests/test_modules/test_md_live_edit.cpp` | **新增**：纯函数用例 | ~300 行 |
| `tests/test_modules/test_markdown.cpp` | 追加：实时态无头 UI 用例 | ~180 行 |
| `docs/KNOWN_LIMITATIONS.md` | 追加实时态边界（长块封顶/IME 冻结语义/上游 LRU 8 交叉引用） | ~20 行 |
| `docs/superpowers/plans/2026-09-22-backlog.md` | Phase 3 条目拆解回填（实施期） | 少量 |

**接口签名（伪代码级，实施时以头文件为准）**

```cpp
// md_live_edit.h —— 纯函数层（可单测）
namespace module::markdown {
struct MdLiveUndoStack {                    // 快照 = (整篇文本, 文档光标偏移)
    void push(std::string text, size_t cursorByte, bool sameBlock, int64_t nowMs);
    bool undo(std::string& text, size_t& cursorByte);
    bool redo(std::string& text, size_t& cursorByte);
};
size_t mdLiveActiveBlockIndex(const MdBlockModel& model, size_t cursorByte);
size_t mdLiveBlockLocalOffset(const MdBlock& block, size_t cursorByte);
using LineCountFn = std::function<int(const std::string& blockText, float textWidth)>;
float  mdLiveInputHeight(const std::string& blockText, float textWidth,
                         float fontSize, float inset, const LineCountFn&);   // 内容贴合
constexpr size_t kMdLiveNoConsume = static_cast<size_t>(-1);
size_t mdLiveVerticalBlockTarget(const std::string& text,
                                 const std::vector<MdBlock>& blocks,
                                 size_t cursorByte, int direction);          // 上下跨块
} // namespace module::markdown
```

---

## 5. 性能目标与验证方法

| # | 指标 | 目标 | 验证方法（**记录式，不设墙钟红线**） |
|---|------|------|--------------------------------------|
| 1 | 分块重算耗时 | 200KB / 5000 行文档 `MdBlockModel::update` ≤ 5ms | DEBUG 日志打印 `[live] segment n=… ms=… dirty=…`（扩展既有脏区间日志）；人工/脚本扫日志复核；无硬断言（避开已知脆弱点） |
| 2 | 键入→成帧延迟 | 200KB 文档连续打字 ≥ 55fps（1600×1000，Win11） | 现有 debug 标题 FPS 计数 + 新增 `[live] frame compose ms` 日志；人工连打 30 秒记录最低帧率 |
| 3 | 滚动帧率 | 2000 块文档连续滚动 ≥ 55fps | 同上（滚轮连续 10 秒） |
| 4 | **高度恒等（正确性兼性能）** | 活动 input `maxVerticalScroll == 0`（内容贴合无内部滚动 = 不吃滚轮） | 每帧 DEBUG 校验并打印 `[live] input over=N`（N>0 即 WARN）；手工点验时 grep 日志零命中（同 §2.2 V5） |
| 5 | 单块高度上限 | 封顶 `0.7 × 视口高`（超出则该块内部滚动，接受滚轮死区） | 合成 5000 行围栏用例：日志显示封顶生效 + 该块内滚轮行为符合预期（手工） |
| 6 | 状态无泄漏 | 切块 50 次后 `stateStore_` 条目数不增 | M0 起在 `releaseStateScope` 调用点打 `[live] release scope=…` 日志计数；条目总数无公开 API → **待验证**（改用日志计数近似 + 长会话内存观察） |
| 7 | 长会话内存 | 连开 20 篇不同笔记、每篇编辑 10 块，无单调增长 | 复用 `test_streaming_stress` 的 psapi 采样模式做一份 live 版采样脚本（或任务管理器人工观察） |
| 8 | 撤销栈内存 | 200 快照且文档 ≤ 2MB 时增量 ≤ 400MB 上限可控（实际按文档大小线性） | 单测断言栈深上限与降级规则；内存观察同 #7 |

---

## 6. 风险与未知（每项给验证方法）

| # | 风险 | 影响 | 验证 / 缓解 |
|---|------|------|-------------|
| R1 | V1：稳定 id 的 input 跨帧换位不被支持 | 单输入方案不成立 | M0 spike（§2.2 V1）；兜底方案 B：input 放**独立 overlay 层**（不随块移动，靠几何表绝对定位到活动块坐标）；兜底 C：上游 PR（input 允许 wrapContentHeight） |
| R2 | V2：块合并/删除导致焦点被清 | 打字丢失 | M0 spike + 手工；缓解：切块帧同帧补发 `requestFocus`（目标已在当帧树中） |
| R3 | IME 与换文本冲突（组字中被换块/重分块） | 中文输入丢字 | 冻结规则（§3.4）+ 手工拼音测试（含预编辑中点其他块）+ 冻结谓词单测 |
| R4 | 超大单块（数千行围栏/松散长列表）高度封顶后：滚轮死区 + 该块全行绘制 | 局部体验退化、帧率跌 | §5 #5 合成用例 + 帧时间日志；接受"块内滚动"为标准行为并写入 KNOWN_LIMITATIONS |
| R5 | V4：切块瞬间一帧跳位（`verticalScroll/followCaret` 残余） | 观感 | M0 spike + 手工截图比对；缓解：换文本时显式归零 `verticalScroll` 并置 `followCaret=false` |
| R6 | 滚动锚定双权威（用户滚动 vs 打字跟随）抖动 | "打字跟手"体感 | 手工清单 ④；`liveFollow_` 状态机 + 只在越界时拉一次；策略本身交用户拍板（§9 Q2） |
| R7 | 每块 `releaseStateScope` 遗漏 → 状态累积 | 长会话内存增长 | §5 #6 日志计数 + #7 内存观察；单测锁定"每次换块必释放旧块前缀"的顺序 |
| R8 | 上游 `cachedMarkdownDocument` LRU 8（KNOWN #9） | >8 块无解析增量收益（成本中性） | 复用既有结论；不新增问题；升级路径已登记（上游 PR ①②③） |
| R9 | 模块层 undo 与用户习惯不符（vs 系统/框架撤销） | 用户困惑 | 手工点验 + §9 相关开放问题；缓解：撤销粒度按"同一块 600ms 合并"更接近逐次编辑直觉 |
| R10 | 字号档/字体切换后高度公式与组件不同源 | 贴合失败 → 内部滚动 + 吃滚轮 | 高度公式显式传入与 input 相同的 `fontFamily/fontSize/inset`（单一常量）；100%/130% 两档手工点验（清单 ⑩） |
| R11 | 第四态带来的模式组合爆炸（编辑/实时/双栏/预览 × 大纲/标签/冲突/toast） | 回归面 | 无头用例覆盖四态循环 + 关键组合（大纲跳转、冲突 toast、标签筛选各一）；手工清单 ①⑪ |

---

## 7. 里程碑拆解（≤2 天粒度，标依赖）

> 依赖链：`M0 → M2 → M3 → {M4, M5, M6} → M7 → M8 → M9 → M10`；`M1` 只依赖 M0。合计 **11.5 人日（≈2.3 周）**。

| # | 里程碑 | 估时 | 依赖 | 交付物 / 验收标准 |
|---|--------|------|------|-------------------|
| M0 | **技术验证 spike（门禁）** | 0.5d | — | V1～V4 四项逐条结论（无头 compose 用例 + 手工击键/IME 记录）；任一项不成立 → 触发 R1 兜底方案重估并回报用户，**不进入 M1** |
| M1 | 第四态骨架（只读） | 1d | M0 | `ViewMode::Live` + 映射表 + segmented 四项 + `Ctrl+Shift+P` 四态 + 持久化（`kMaxMode`）+ `drawLiveColumn` 只渲染全部块（无 input）；验收：无头用例断言四态循环与元素存在性、旧 state 文件 mode 0-2 语义不变 |
| M2 | 活动块输入 + 内容贴合高度 | 1.5d | M1 | 单输入 `md.live.input` 就位、内容贴合高度公式（`measureLines`）、`onChange → syncDocumentFromLiveInput → setEditorText` 上行、`blockIndexAt` 活动块推导；验收：输入回写文档一致（无头断言 `editorText_` 等价于同编辑在编辑态的结果）、`[live] input over=0` 日志零命中 |
| M3 | 焦点/切换状态机 + IME 冻结 | 1.5d | M2 | 点渲染块定位（复用 `mdResolvePreviewJump`）+ 同帧 `requestFocus`、失焦提交、切块换文本 + `releaseStateScope`、IME 冻结谓词；验收：连续切块 20 次不丢字（无头驱动 + 手工）、组字中切块被拒 |
| M4 | 块边界键（无块结构操作） | 1d | M3 | Enter/Backspace/Delete/↑↓ 边界语义按 §3.4；验收：`mdLiveVerticalBlockTarget` 与边界偏移的单测（首块/末块/空文档/cursor==size/CRLF）+ 手工 |
| M5 | 块级 undo/redo | 1d | M3 | `MdLiveUndoStack` + Ctrl+Z/Y/Shift+Z 拦截 + 600ms 同块合并；验收：跨块撤销序列单测（推栈顺序/合并/上限/降级） + 手工多块撤销 |
| M6 | 滚动锚定与光标跟随 | 1d | M3 | 外层 scrollView 单向权威、`liveFollow_` 状态机、越界拉一次、`contentKey` 用内容 hash；验收：手工清单 ③④ + 日志无高度失配 |
| M7 | 高亮与视觉一致 | 1d | M3 | `liveHighlighter_`（块文本坐标）、渲染块 CodeFence 自绘复用、活动块边框/背景；验收：截屏与预览态逐块比对（块间隙/圆角/色表一致） |
| M8 | 联动收口 | 1d | M4,M5,M6,M7 | 大纲跳转走 `setLiveCursor`、状态栏 `(行:列:字)`、标签栏、Ctrl+S、搜索筛选、跨模块跳转；验收：定向无头用例 + 手工 |
| M9 | 性能实测与调优 | 1d | M8 | §5 #1～#5 五项记录式验收；不达标则按优先级调优（封顶/长块/缓存），并在 KNOWN_LIMITATIONS 登记 |
| M10 | 测试补齐 + 文档 + 点验 | 1d | M9 | 新增用例全绿（原基线零回归）、KNOWN_LIMITATIONS/backlog/CHANGELOG 更新、手工点验清单交付用户 |

---

## 8. 测试策略

### 8.1 纯函数单测（`tests/test_modules/test_md_live_edit.cpp`，`REGISTER_TEST` 风格）

| 覆盖对象 | 用例要点 |
|----------|----------|
| `mdLiveActiveBlockIndex` / `mdLiveBlockLocalOffset` | 空文档、单块、cursor==size（归末块）、块首/块尾字节、CRLF、中文多字节边界 |
| `mdLiveInputHeight` | 注入假测量函数（N 行 → N×lineH + inset×2 + 1）；0 行块、1 行、超宽折行计数、字号/inset 变化 |
| `mdLiveVerticalBlockTarget` | 首块 ↑ / 末块 ↓ 不消费（`kMdLiveNoConsume`）；中间块上/下；空块序列；光标不在块首行/末行（不消费） |
| 边界编辑偏移 | Enter 于段尾/段中/列表行（应走 `applyEnterList`）、Backspace 于块首（删前换行）、Delete 于块尾、跨空行的归属 |
| `MdLiveUndoStack` | 推栈/撤销/重做序列、600ms 同块合并、容量上限与降级（>2MB → 50）、模式切换边界快照 |
| 视图状态 | `mode = 3` 通过 `sanitized()`；越界（4/-1）回落 0；旧文件 0-2 语义不变 |

### 8.2 无头 UI 测试（`test_markdown.cpp` 追加，走既有 `MdUiEnv` + `ui.begin/compose` 基座）

1. **四态循环**：Ctrl+Shift+P 逐态断言可见元素集合（实时态：`md.live.scroll` 存在、`md.live.input` 恰好 1 个、`md.preview.scroll` 在实时态不出现）。
2. **点击定位**：`ui.find("md.live.blk.<id>.hit")->onClick(...)` → 断言 `ui.state<InputState>("md.live.input").text` == 该块源码、`ui.isFocused("md.live.input")`、活动块 wrapper 内出现 input。
3. **上行一致性**：直接改 `InputState.text` + 触发 `onChange` → 断言再次 compose 后块序列与文档等价（与编辑态同编辑的文本逐字节相等）。
4. **高度贴合**：断言 `ui.find("md.live.input")` 的高度 == 公式期望值（用同一 `measureLines` 计算），容差 ±1px。
5. **跨块 ↑↓**：构造三块文档 → 活动块 1 首行按 ↑ 消费且活动块变 0；活动块 0 首行按 ↑ 不消费（`mod->onKeyEvent` 返回 false）。
6. **撤销**：两次跨块编辑后 Ctrl+Z → 文档回退到上一快照、活动块与光标同步。
7. **入口回归**：既有 `md_module_compose_phase1` / `md_module_compose_tree` 等用例必须零改动通过（三态语义不变）。

### 8.3 手工视觉点验清单（项目铁律：未截屏 = 未完成）

| # | 点验项 |
|---|--------|
| ① | 四态切换观感（segmented 四项不挤压顶栏；实时态文本列居中 720px） |
| ② | 点渲染块 → 变源码态、caret 落在点击行、无需二次点击 |
| ③ | 打字跟手：块底部连续打字，caret 始终可见 |
| ④ | 用户先滚动再打字 → 观察是否回拉（对齐 §9 Q2 的最终策略） |
| ⑤ | 中文拼音输入上屏（含预编辑中点其他块 → 应被拒/安全） |
| ⑥ | Enter 造空行 → 光标跟进新块；Backspace 合并两段后继续打字不丢焦点 |
| ⑦ | 跨块 ↑↓ 落点（列保持、块边界行精确） |
| ⑧ | 长围栏块（>2000 行）封顶行为 + 块内滚动 |
| ⑨ | 双栏 ↔ 实时观感一致性（块间隙、圆角、代码块色表、表格） |
| ⑩ | 100% / 130% 字号两档：高度仍贴合（无内部滚动条出现） |
| ⑪ | 外部编辑器改文件 → 冲突 toast 三选项仍工作 |
| ⑫ | 5000 行文档滚动/打字帧率（对照 §5 #2/#3 目标值） |

---

## 9. 开放问题（实现前需拍板）

1. **「实时」是否取代「编辑」成为默认模式？** A 保持默认「编辑」，实时为可选第四态（建议，先 opt-in）/ B 实时为默认（更像 Typora）/ C 默认隐藏实时，设置页开关启用。
2. **打字时的滚动锚定策略**：A 打字永远把 caret 拉回视口（最强跟手）/ B 仅当 caret 越出视口时才拉一次（建议）/ C 完全不动，仅点击定位时对齐。
3. **Enter 语义**：A 块内换行，空行才分段（源码保真，建议 —— 保住「逐块 == 整篇」等价性）/ B Typora 式"按一次 Enter 即分段"（需引入块结构操作，等价性回归锚需重新论证）。
4. **实时稳定后原双栏/预览的去留**：A 三态 + 实时全保留（建议）/ B 实时稳定后隐藏双栏 / C 只保留编辑 + 实时（预览降为只读跳转）。

---

## 附：记录（实施期回填）

| 里程碑 | 状态 | commit / 证据 |
|--------|------|---------------|
| M0 spike | 未启动（本文件为设计草稿） | — |

---

## 已定决策（2026-09-27，用户拍板）

| # | 决策项 | 结论 | 含义 |
|---|--------|------|------|
| D-A | 实时渲染态位置 | **第四态 opt-in** | 不取代「编辑」，作为可切换的第四态；出问题可回退 |
| D-B | 打字时滚动锚定 | **越界才拉** | 仅当 caret 越出视口时拉一次，其余时候不干预滚动 |
| D-C | Enter 语义 | **块内换行，空行才分段** | 保住「逐块 == 整篇」等价性，解析器无需改动 |
| D-D | 现有四态去留 | **四态全留** | 编辑 / 双栏 / 预览 / 实时 共存，由用户选择 |

以上四条为实施前置约束；spec 第 4 节列出的 4 项框架待验证行为，仍按 M0 门禁 spike 逐个实测。
