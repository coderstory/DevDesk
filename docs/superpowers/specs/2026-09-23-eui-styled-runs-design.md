# EUI-NEO Styled Text Runs 设计（Markdown Phase 2 语法高亮的上游前置）

- 日期：2026-09-23
- 状态：Phase A/B 已合入 EUI-NEO main（b7e0e9f）；**Phase C 已落地**（DevDesk `feat/md-highlight`，2026-09-23，§8 表回填）；Phase D 未启动
- 范围：EUI-NEO 仓（上游 PR）+ DevDesk 侧行级高亮器；**本文档只做设计，不改代码**
- 上游依据：
  - `docs/superpowers/specs/2026-09-22-markdown-editor-ux-design.md` §5.1（编辑器语法高亮上游改造范围）、§6.2（着色表）
  - `docs/superpowers/specs/2026-09-22-markdown-editor-external-research.md` §4.2/§4.5（API 范式结论：**业务层 Flutter TextSpan 树语义 + 内部 Scintilla 式扁平 run 数组**）
  - `src/module_markdown/md_line_scanner.h`（W6 结论：md4c Block 无源码 offset，行级扫描器是既定底座）

---

## 1. 需求

### 1.1 主需求：Markdown 编辑器语法高亮（DevDesk Phase 2）

编辑器（`components::input` multiline）当前每行渲染一个单色 `ui.text`，无法在一段文字内混色。需要：

- 行内多色 run：标题标记 `#`、粗体 `**`、行内代码 `` ` ``、链接 `[]()`、列表符号、引用 `>`、表格管道 `|` 各自着色（完整色表见 ux-design §6.2）
- 高亮与光标 / 选区 / IME 组合区共存，caret 像素定位精度不能退化
- 高亮随编辑增量更新（改一行只重算一行），万行文档不掉帧

### 1.2 次级需求（顺带受益，不额外开发）

- **静态富文本**：markdown 预览组件目前靠"多个 `ui.text` segment 手工定位"实现混色（`EUI-NEO/components/markdown.h:1418-1460` `renderInlineRuns/renderInlineSegment`），styled runs 落地后可部分收敛（仅颜色差异的 segment 合并为单元素）
  - **Phase C 评估结论（2026-09-23）：不做**。segment 间不只是颜色差异——代码 run 是 monospace 小字号、标题有独立字号/字重，颜色 run（v1 唯一维度）无法合并异字体/异字号段；只有"同字体同字号仅色差"的 run（如链接文字 vs 正文）能收敛，而那要先重构逐段测量+手工换行的 inline 布局才能定位收益，工作量 > 半天。待 Phase D（weight run）落地后重评——届时代码段字号可并入 run 维度，收敛空间更大。
- **未来代码块虚拟化 / 日志着色**：同一 run 机制直接复用（框架中立，EUI-NEO 不懂 Markdown）
- **批渲染收益**：现状 OpenGL 文本批在颜色变化时 flush（`opengl_text.cpp:320-331`），预览侧多色文本每换色一次断一批；per-vertex color 后该 flush 消失

### 1.3 非目标（明确排除）

- 行内混字号 / 混字体 / baseline 对齐（Flutter InlineSpan 全家桶）——v1 不做，见 §3.4
- 富文档 span 树嵌套语义（WidgetSpan、样式继承链）——编辑器场景不需要
- 真语言 tokenize（代码块关键字高亮）——ux-design §5.1 已列为 P3 或不做
- BiDi、下划线/删除线装饰——斜体以颜色替代（EUI-NEO 无 italic 轴，ux-design §6 风险 5）

---

## 2. 现状差距（源码审计结论）

### 2.1 文本渲染链路（`EUI-NEO/core/render/text.cpp`）

```
TextStyle{ text, fontFamily, fontSize, fontWeight, color, ... }   ← 单一样式，整段一个色
  → TextPrimitive::Impl::rebuildLayout()   (text.cpp:1727)
      按 \n 切段 → shapeText() 逐码点 shaping（UTF-8，fallback 字体栈）
      → Line{ vector<LaidOutGlyph>, width, inkTop/Bottom }
  → rebuildVertices()  (text.cpp:1785)
      每个 glyph 产 6 顶点 × 5 float（x,y,u,v,colored 标志）→ vertices_
  → render() → RenderBackend::drawText(TextDrawCommand)  (render_backend.h:40)
      TextDrawCommand{ vertices, color } ← **整个 command 一个 color**
```

三层缓存（styled runs 的影响点）：

| 缓存 | 位置 | key | runs 影响 |
|------|------|-----|----------|
| FontStackCache（16 槽 LRU） | text.cpp:100-108 | fontPath+weight+size | 无（颜色无关） |
| TextSizeCache（1024 槽 LRU） | text.cpp:110-159 | **全文 text** + fontFamily/size/maxWidth/lineHeight/weight/wrap | **颜色 run 零影响**（advance 不依赖颜色）；weight run 需扩展 key（§4.3） |
| 共享 glyph atlas（灰 2048² + 彩 1024²） | text.cpp:171-207 | face路径+size+glyphIndex | 无 |

**关键发现（本设计最大简化来源）**：`shapeWithFallback()`（text.cpp:961-986）逐码点 shaping，glyph 的 `advance/xOffset` 完全由字体度量决定、与颜色无关。因此**纯颜色 run 不需要任何 layout/shaping 改动**——只是把"每个 glyph 用哪个颜色写顶点"的问题。这把上游 PR 的核心风险（L2 run 级布局，research §4.3 估 3-4 周）从 v1 里整个砍掉了。

### 2.2 GPU 侧（两个后端都要改）

- OpenGL：`opengl_text.cpp:91` 顶点属性 `aColored`（5 float 步长，`:266`），`uColor` 为 per-batch uniform，`drawText` 检测颜色变化即 `flushTextBatch()`（`:320-331`）
- Vulkan：`vulkan_text.cpp:65-68` 同样 5 float 步长批渲染
- 批上限 `kTextBatchMaxFloats = 262144`（opengl_text.cpp:15）

### 2.3 DSL / 运行时（`core/dsl.h`、`core/runtime/`）

- `Element`（dsl.h:130-139）：`text/fontFamily/fontSize/fontWeight/textColor/...` 平铺字段，无 runs 概念
- `TextBuilder`（dsl.h:1137-1208）：链式 setter，`.text()` 设整段
- `Runtime::updateText`（runtime_update.h:883-930）：`contentChanged` 判定靠逐字段比较或 `dirtyKey`（keyed 路径比较 `instance.contentDirtyKey != element.dirtyKey`）；文本实例 `TextInstance`（runtime_instances.h:75）持 `unique_ptr<TextPrimitive>` 按 element.id 复用
- 固有尺寸测量：dsl.h:1652 `TextPrimitive::measureTextSize(style)`

### 2.4 input 组件（`components/input.h` + `input_model.h`）

编辑器最难处，现状结构（已审计）：

- **显示文本路径**：多行时按可视行渲染，每行一个 `ui.text`，id 带行号、dirtyKey 带行号（input.h:402-425）；光标/选区/IME 下划线是**独立的 rect 元素**，绘制顺序 = 选区 rect → 文本 → 光标 rect（input.h:385-448），层级天然分离
- **布局缓存**：`InputState.cachedLines`（每段一行级 `TextLine{start,end,hardBreakAfter,TextMetrics}`），`updateChangedParagraphs`（input_model.h:594-643）用前后缀 diff 只重测变化段——**增量重排底座已存在**
- **caret 映射**：`TextMetrics.byteIndices/caretX`（text.cpp:997-1025 `makeTextMetrics`）+ 二分查找（input_model.h:795-804）
- **IME**：preedit 是独立的 `InputState` 快照（`displayState`，input_model.h:659-685），组合文本替换选区范围后走同一条渲染/测量路径
- dirtyKey 已含 `textRevision/compositionRevision`（input_model.h:912-928）

### 2.5 md4c（vendored，`EUI-NEO/3rd/md4c/`）

- 预览用；回调 API 是 Block/Span 树（`enter_block/leave_block/enter_span/leave_span/text`），Block 回调**无源码 offset**（W6 结论）；Span 回调虽传文本指针（可做指针差求 offset），但仅覆盖 inline span，行级标记（`#`、`-`、`>`、`|`）不作为 span 暴露，且 md4c 会做实体解码/规范化，token 与原文非一一对应
- DevDesk 已有自研行扫描器 `md_line_scanner.h`（标题/行号/围栏状态），注释里明确"Phase 2 语法高亮在本文件上扩展"

---

## 3. API 设计

### 3.1 数据结构（`core/render/text_types.h`，EUI-NEO）

```cpp
/// 扁平样式 run（Scintilla 式）。字节偏移是整个文本串的 UTF-8 偏移，
/// 与 TextMetrics::byteIndices、InputState.cursor 同一坐标系。
struct TextRun {
    int byteStart = 0;          // 含
    int byteEnd = 0;            // 不含；run 必须 non-empty、按 start 升序、互不重叠
    Color color{1,1,1,1};       // v1 唯一样式维度
    // Phase D 追加：int fontWeight = 0;（0 = 继承元素级）——先不进结构体
};

/// TextStyle 增加一个字段（向后兼容：空 = 旧单色路径）
struct TextStyle {
    // ... 现有字段不动 ...
    std::vector<TextRun> runs;  // 空 ⇒ 行为与今天完全一致
};
```

约束（构造期归一化，渲染层不做防御）：

- 允许调用方给乱序/重叠/越界 run；EUI-NEO 提供 `normalizeTextRuns(text, runs)` 纯函数：clamp 到 `[0, text.size()]` 的 UTF-8 边界、去重叠（后写覆盖前写）、合并相邻同色、丢弃空 run。单测覆盖
- 相邻同色 run 合并后，typical Markdown 行 run 数 ≤ 6，`vector<TextRun>` 拷贝成本可忽略

### 3.2 DSL 层（`core/dsl.h`，Flutter TextSpan 语义的扁平投影）

Flutter 的树语义 → 扁平 run 的映射：**TextSpan 子树的先序遍历 + 样式覆盖即"后写覆盖前写"**。业务层不需要真的建树——语法高亮的 lexer 天然产出 token 流，直接映射 run；真需要树语义时（嵌套强调），调用方按先序展开即可（DevDesk 高亮器不需要嵌套）。

```cpp
// Element 新增：std::vector<core::TextRun> textRuns;

class TextBuilder : ... {
    /// 追加一个 run（可多次调用，顺序即优先级；内部最后统一 normalize）
    TextBuilder& run(int byteStart, int byteEnd, const Color& color);
    /// 整批设置（高亮器产出的标准形态）
    TextBuilder& runs(std::vector<core::TextRun> value);
    /// 清空 runs，回到单色
    TextBuilder& clearRuns();
};
```

用法示例（DevDesk 高亮器视角）：

```cpp
ui.text("md.editor.line.42")
    .text(lineText)                       // 旧 API 不变
    .color(tokens.text)                   // 元素级 color = 未命中 run 的默认色
    .runs(highlighter.runsForLine(42))    // 新增；不给 = 现状
    ...
```

**向后兼容承诺**：`.text()` 路径零改动；`runs` 为空时 `TextPrimitive` 走与今天逐字节相同的布局和顶点路径（默认色填进 per-vertex color），现有 291 个 DevDesk 测试 + EUI-NEO examples 不应有任何输出差异（截屏对比验收）。

### 3.3 TextPrimitive 内部（`core/render/text.cpp`）

```cpp
public:
    void setRuns(std::vector<TextRun> runs);   // 颜色 run：只 invalidateVertices()，不动 layoutDirty_

private:
    // LaidOutGlyph 增加 run 色或按 run 索引着色；rebuildVertices() 写顶点时
    // 用二分在 run 数组里查 glyph.byteStart 所属 run，命中→run 色，未命中→style_.color
```

- `rebuildLayout()` **不改**（§2.1 关键发现：颜色不影响 shaping/advance/换行）
- `rebuildVertices()`：顶点格式从 5 float 扩到 **9 float**：`x, y, u, v, colored, r, g, b, a`
  - glyph → run 查找：runs 已按 start 排序，`upper_bound` 二分，每 glyph O(log runs)；行级 run 数 ≤ 6，实际就是 1-2 次比较
  - `colored`（彩色 glyph，emoji）分支不变——彩色 glyph 本来就采样 atlas 颜色，run 色仅作 alpha 乘子（与现有 `sampleColor * uColor.a` 语义一致）
- `measureTextSize` / `measureTextMetrics`：v1 不感知 runs（颜色不改尺寸）

### 3.4 分期决策：v1 只做颜色 run，weight run 进 Phase D

| 维度 | 颜色 run（v1） | weight run（Phase D） |
|------|---------------|----------------------|
| layout/shaping | 零改动 | 每 run 独立字体栈 re-shape（`loadSharedFontStack` 按 weight 有独立缓存槽，text.cpp:710-715，天然支持） |
| 换行/caret | 零改动（advance 不变） | TextMetrics 跨 run 缝合，丢 kerning/ligature（ux-design §5.1 已注明该近似） |
| TextSizeCache | 零改动 | key 必须加 run 指纹（否则量错） |
| 收益 | ux-design §6.2 色表的 90%（标记弱化、代码/链接/引用着色） | 编辑器内粗体/标题加粗显示 |

DevDesk 编辑器粗体显示 v1 用颜色区分不降级体验（正文 vs 强调色已有区分）；weight run 作为独立决策点，编辑器高亮落地后按实际需求再启动。

### 3.5 后端与着色器（`core/render/opengl/` + `vulkan/`）

- 顶点步长 5 → 9（`opengl_text.cpp:266`、`vulkan_text.cpp:65-68` 两处 `/ 5` 及 shader `layout(location=2/3)` 属性声明）
- 灰 glyph 片元：`vec4(vColor.rgb, vColor.a * alpha)` 替代 `uColor.rgb`；**`uColor` 降级为整 command 的 tint（element opacity 动画用），默认白色**——批内颜色变化 flush 逻辑（opengl_text.cpp:320-331）可整体删除
- 兼容策略：一次性切换，不留双路径（渲染后端本就同仓同版本发布；`TextDrawCommand.color` 字段保留作 tint，不破坏接口）
- 实施前 grep 确认 `TextDrawCommand` / 顶点数组消费点全集（已知：opengl_text.cpp、vulkan_text.cpp、primitive.cpp 无直接消费）

---

## 4. 渲染 / 测量 / 缓存影响清单

### 4.1 layout 与测量

| 项 | 影响 |
|----|------|
| `rebuildLayout` | 无 |
| `measureTextSize`（dsl.h:1652 固有尺寸） | 无（颜色不改尺寸） |
| TextSizeCache 命中率 | 无变化（key 不含 runs） |
| FontStackCache / glyph atlas | 无 |

### 4.2 顶点与批渲染

- 每 glyph 顶点内存 30 → 54 float（+80%）：万行文档只渲染可视行（input 按行裁剪，input.h:404-405），可视 ~60 行 × ~100 glyph × 54 ≈ 324k float > `kTextBatchMaxFloats`(262144) → 触发帧中批 flush 一次，已有机制，无正确性问题；可将上限提到 512k（一次性内存 2MB，可接受）
- 删除颜色 flush：预览侧多色文本批次数**下降**（每换色省一次 draw call + uniform 切换）

### 4.3 dirty key / 实例复用

- `Runtime::updateText`（runtime_update.h:900-930）：keyed 路径（有 dirtyKey）自动正确——调用方把 runs 变化编进 dirtyKey 即可；非 keyed 路径 `contentChanged` 增加一项 `instance.textRuns != element.textRuns`（vector 比较，runs ≤ 6，成本可忽略）
- input 的行级 `textDirtyKey`（input.h:142-145）：追加 `|` + `styleRevision`（§5.2）

### 4.4 retained layer / 脏区

- 文本元素本来就在 dirty-rect 体系内按元素重绘；runs 变化 = 元素内容变化 = 现有失效路径，无新增机制

---

## 5. input 集成方案（高亮层与光标层关系）

### 5.1 分层原则：高亮是文本层的属性，光标/选区/IME 保持独立 rect 覆盖层

现状绘制顺序（input.h:380-449）：

```
stack.textViewport (clip)
 ├─ 选区 rects（含 IME 下划线 rect）      ← 覆盖层，不动
 ├─ 每行一个 ui.text（单色）              ← 唯一改动点：加 .runs()
 └─ 光标 rect                             ← 覆盖层，不动
```

**改动只有一处**：input.h:412-424 的每行 `ui.text(...)` 追加 `.runs(...)`。选区半透明色叠在彩色文本上、光标在最上——层级语义与现状一致，IME preedit 下划线沿选区 rect 走，全部不变。

### 5.2 框架中立的样式注入：行级回调

EUI-NEO 不懂 Markdown，样式由使用方注入：

```cpp
/// InputBuilder 新增。回调按需调用：仅对【可视行】、且该行文本或 styleRevision
/// 变化时调用一次，结果缓存在 InputState（见下）。返回空 vector = 该行默认色。
InputBuilder& lineStyles(std::function<std::vector<TextRun>(
    const std::string& displayText,   // 该行显示文本（含 preedit 替换后的内容）
    int lineStartByte,                // 行首在全文字节偏移
    int lineNo)> provider);
```

- **InputState 新增**：`unsigned long long styleRevision = 0;`（使用方改高亮规则时自增）+ 行级结果缓存 `std::unordered_map<int, std::vector<TextRun>> cachedLineRuns`（key=行号，内容随 textRevision/styleRevision 失效；只缓存可视行窗口，滚出视口即逐出）
- **caret/选区映射：v1 零改动**——颜色 run 不改 `TextMetrics`，`caretX()/closestCaret()`（input_model.h:795-208）原样工作。这是把 weight run 推到 Phase D 换来的最大去风险
- **IME**：组合期间 `displayState` 产出的显示文本传入回调；组合区落在哪一行就按该行显示文本高亮（组合文本本身通常无 markdown 语义，未命中 run 自然回默认色）。preedit 快照与正文快照共享行缓存时以 textRevision 区分，避免污染
- **回调成本上界**：每可视行每次变化一次 O(行长) 扫描；滚动纯位移不重扫（行文本没变，缓存命中）

### 5.3 与 ux-design §5.1 上游改造清单的对应

| ux-design §5.1 条目 | 本设计落点 |
|---------------------|-----------|
| 1. InputState 增加 lineStyles / 回调 | §5.2 行级回调（选回调方案，避免 state 里存全量样式） |
| 2. 按 run 切多个 ui.text 手工定位 | **升级**：单元素 `.runs()`（免手工定位、免多元素 compose 成本） |
| 3. caretX 跨 run 缝合 | **v1 不需要**（颜色 run 不改 metrics）；Phase D 再做 |
| 4. 通用能力 + example + 测试 | §8 Phase A/B 交付物 |

---

## 6. md4c offset 方案选型

| 方案 | 说明 | 判定 |
|------|------|------|
| **A. 自研行级词法扫描器（选定）** | 扩展 `md_line_scanner.h`：围栏状态机已有，追加行内 token 识别（`#` 前缀、`**`/`*`、`` ` ``、`[]()`、行首 `-`/`*`/`N.`、`>`、`|`），输出 `vector<TextRun>` | **选它**。编辑器高亮是行级词法问题（VS Code/CodeMirror 同路线）；与大纲扫描、Phase 3 分块器共用围栏状态（一鱼三吃）；纯函数可单测；成本 ~200 行 |
| B. 扩展 md4c 加 Block 源码 offset | patch vendored md4c | 弃。内部结构不维护块偏移，改动侵入解析核心，与上游 md4c 永久分叉；收益仅"复用块分类"，而块分类行扫描器已有 |
| C. md4c Span 回调指针差求 offset | `text - doc_begin` 得 inline span 偏移 | 弃。只覆盖 inline span；行级标记不暴露；md4c 解码实体/规范化文本，token↔原文非一一映射；且每击键全文 parse O(doc) 与增量编辑模型冲突 |

**边界诚实声明**：行级扫描器不是 CommonMark 完整实现（跨行强调、懒惰续行等边角会着色不准或漏标）——对**编辑器高亮**可接受（高亮是视觉辅助非语义解析）；预览侧继续走 md4c 保证渲染正确。两套各司其职，不追求统一。

---

## 7. 性能预算（万行文档增量重排）

场景基线：10,000 行中文/英文混排 Markdown，编辑器可视 ~60 行，60fps。

| 环节 | 预算 | 依据 |
|------|------|------|
| 击键 → 段落重测 | O(变化段)（现状已有） | `updateChangedParagraphs` 前后缀 diff（input_model.h:594-643），单段重测 ~百 µs 级 |
| 高亮扫描 | O(变化行)，仅变化行重扫 | 行缓存 + styleRevision；单行扫描（~100 字符，≤10 token）< 10µs |
| 行渲染 | 可视行 × run 查找 O(log runs) | runs ≤ 6，实际 1-2 次比较，纳秒级 |
| 顶点重建 | 仅 dirty 行（dirtyKey 行级失效） | 单行 ~100 glyph × 54 float ≈ 21KB |
| 批渲染 | 60 行一帧，约 1 次 flush | §4.2；建议 `kTextBatchMaxFloats` 262144 → 524288 |
| 全文档打开首帧 | 首帧只扫可视行 | 行缓存逐出滚出视口的行，内存 O(视口) |

**红线**：若 Phase B 实测击键延迟（含 undo 快照、段落重测、高亮、顶点重建全链）> 8ms，优先排查点：① runs 未进 dirtyKey 导致全量行重建 ② 回调没走缓存每帧重扫 ③ textRuns 拷贝进了热路径（应 move）。

---

## 8. 分阶段实施

| Phase | 内容 | 仓 | 规模 | 交付物 / 验收 |
|-------|------|-----|------|--------------|
| **A. 只读样式** | TextRun/TextStyle.runs/normalize、TextBuilder.run/runs、TextPrimitive per-vertex color、OpenGL+Vulkan 顶点格式 5→9 float、删颜色 flush | EUI-NEO | ~600 行 + 2 后端 shader | example（多彩标题/代码行）+ 单测（normalize 边界、空 runs 与旧路径输出逐字节一致）；现有 examples 截屏零差异 |
| **B. input 高亮** | InputBuilder::lineStyles 回调、行级 runs 渲染、styleRevision + 行缓存、dirtyKey 追加 | EUI-NEO | ~300 行 | example（关键词着色的多行编辑器，光标/选区/中文 IME 全操作回归）+ 单测 |
| **C. DevDesk 高亮器** | `md_highlighter.{h,cpp}`（扩展 md_line_scanner 围栏状态）+ theme 明暗色表接线 + 行缓存随 textRevision 失效 | DevDesk | ~200-300 行 | ✅ **已落地**（2026-09-23，`feat/md-highlight`）：`md_highlight.{h,cpp}` + `md_line_scanner.scanFences`（大纲/高亮共用围栏状态机）+ §6.2 色表全映射现有 token + lineStyles 接线/主题切换 bump styleRevision；+7 单测（含万行性能冒烟实测击键轮 max 1.53ms < 8ms） |
| **D. weight run（可选）** | TextRun 加 fontWeight、per-run 字体栈 shaping、TextMetrics 跨 run 缝合、TextSizeCache key 扩展、caret 回归 | EUI-NEO | ~800 行 | 独立 PR，A-C 落地后按需求决策（粗体内容当前只有标记弱化、无强调色——是 Phase C 已知取舍） |

依赖链：A → B → C 严格串行；D 独立。A+B 即满足"上游 PR"定义（PR #2，附 example + 测试 + docs，符合 EUI-NEO 贡献政策）。

**工作量估计（单人）**：A ≈ 1 周（顶点格式迁移 + 双后端回归是大头）；B ≈ 1 周；C ≈ 2-3 天；合计 **2.5-3 周**。D 追加 1-2 周。

---

## 9. 风险与开放问题

### 风险

| # | 风险 | 等级 | 缓解 |
|---|------|------|------|
| 1 | 顶点格式 5→9 float 触碰双后端 shader/批逻辑，回归面大 | 中 | 机械改动但必须双后端 + 全 examples 截屏对比；先 OpenGL 后 Vulkan 分两个 commit，各自可回滚 |
| 2 | 行缓存/回调设计漏失效路径（文本变了缓存没失效 → 高亮错位） | 中 | 缓存 key 强制 (lineNo, textRevision, styleRevision) 三元组；错位属于视觉 bug 可立即察觉；单测覆盖"改行→滚走→滚回"序列 |
| 3 | 行级扫描器边角语法着色不准（跨行强调等） | 低 | 编辑器高亮是辅助信息；预览走 md4c 不受影响；漏标优于错标（默认色兜底） |
| 4 | runs 拷贝进每帧热路径造成抖动 | 低 | Builder API 全 move；行缓存命中路径零分配 |
| 5 | Vulkan 后端当前未在 Windows 日常验证（DevDesk 走 OpenGL） | 低 | Phase A 的 Vulkan 部分标注"编译通过 + 基本渲染"，深度回归延后到 Vulkan 启用时 |

### 开放问题（实现前需拍板）

1. **`TextRun` 是否预留 `fontWeight` 字段（v1 恒 0）**——预留则 Phase D 不改 ABI（vector 重新分配是调用方的事，EUI-NEO 无 ABI 承诺，但 DevDesk 全量重编译成本存在）；建议：不预留，Phase D 直接加字段（ponytail：YAGNI，加字段是一次全量重编译，早晚会发生）
2. **行缓存归属**：放 `InputState`（EUI-NEO 持有，使用方通过回调注入内容）vs 使用方自持——建议 InputState 持有（与 cachedLines 同生命周期，IME preedit 快照自动隔离）
3. **`.run()` 追加语义的颜色合并阈值**——normalize 合并"相邻同色"，浮点色相等比较用逐分量 == 还是 epsilon：建议 ==（token 色来自同一 theme 实例，不存在计算色）
4. **Phase B 是否顺带做 ux-design PR #1 的 Tab 缩进**——两个 PR 都动 input，可合并为一波；建议分开（styled runs 已够一个 PR 的量）

---

## 附：外部参照（API 形态对照，结论引用自本仓 external-research §4.2/§4.5，2026-09-22 调研）

| 框架 | API 形态 | 借鉴点 | 教训 |
|------|----------|--------|------|
| Flutter TextSpan/InlineSpan | 树形 span，样式向下继承，Text.rich() 接收整树 | **业务层树语义**（本设计 §3.2 的扁平投影来源）；composing range 与 selection 并列为独立 range | 树 + 继承对编辑器场景过重；SkParagraph 内部照样摊平成 run——印证"树在 API、扁在内部" |
| Scintilla | 扁平 StyleRange + style index 槽位（SCI_STYLESETFORE 预注册） | **内部存储形态**（本设计 §3.1 直接采用扁平 run）；Document/Editor/ViewStyle 数据-交互-渲染三层分离 | style index 间接层（颜色查表）在我们场景无收益——直接存 Color，少一层映射少一处失效 |
| Qt QTextLayout + QTextCharFormat | FormatRange 数组随 layout.draw() 传入，QTextLine 逐行布局 | FormatRange{start,length,format} 与本设计 TextRun 同构，验证扁平 range + 单 layout 是成熟形态；光标映射与 layout 分离 | Qt 的 format 对象重量级（QTextCharFormat 继承 QVariant 体系）——我们用 POD struct 规避（细节依训练知识，待验证） |

（外部链接未能在线复核——WebFetch 被网络策略拦截；上述三家的 API 形态以 2026-09-22 仓内调研文档为准，Qt 部分标注待验证。）
