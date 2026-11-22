# CJK 行首禁则排版（kinsoku）调研快答 — need:324

日期：2026-11-18 ｜ 档位：researcher 快答 ｜ 结论分级见各节标注

## 结论（5 条）

1. **字符集**：行首禁则最小集 = 全角句读 + 全角结束括号/引号 + 省略号（。，、！？：；）〉》」』】…，共 ~15 码点），**只收全角、半角永不入集**（保 URL/英文混排，见 §六）。依据 UAX #14 类别表（一手）与 JIS X 4051 / Ecma OpenXML 简中表（汇编）两源交叉，一致。【已确认】
2. **断行决策点共 4 处，3 处在 EUI-NEO、1 处在 DevDesk 应用层**：只改 EUI-NEO 排版层覆盖不到 agent 工具卡描述（`wrapToolDescLines` 手工折行、渲染走 `wrap(false)` 逐行文本节点）。【已确认（codegraph 实读）】
3. **推荐实现：拉回法（pull-in）**——断行判定成立时，若即将起行的字符 ∈ 禁则集，则并入本行行尾（允许本行超 `maxWidth` ≤1 个标点宽）；不引入 UAX #14 完整 pair 规则表。【推荐，待实现验证】
4. **测量与渲染必须同步改**：`text_measure.cpp` 与 `text_layout.cpp` 存在「测量宽度与渲染宽度逐位一致」既有契约（text_measure.cpp:206-208 注释），不同步会导致卡高估算与实际渲染行数漂移。【已确认（代码注释）】
5. **工作量 ~1 人日**（含单测与双平台验证）；风险面集中在半角误伤（用全角-only 规避）、连续禁则超宽（加 ≤2 次钳制）、应用层旁路盘点不全（实现前 1 次 codegraph 全扫）。【推测（工作量）】

## 一、问题

池 #324：全站文本换行未处理 CJK 行首禁则——窄盒描述以全角逗号起行（evidence/20261118-ui-fix-260-261/review2.md ①）。改动归属 EUI-NEO 排版层（本报告证实还需 DevDesk 应用层 1 处配合，见 §三 #4）。

## 二、禁则字符集提案（两档）

### 最小集（推荐第一刀：行首禁则、全角 only）

| 类别 | 码点 |
|---|---|
| 句读 | U+3002 。 U+3001 、 U+FF0C ， U+FF01 ！ U+FF1F ？ U+FF1A ： U+FF1B ； |
| 结束括号 | U+FF09 ） U+3009 〉 U+300B 》 U+300D 」 U+300F 』 U+3011 】（可加 U+3015 〗 U+3017 〙） |
| 尾符 | U+2026 …（U+2014 — 可选，破折号成对使用建议连同前字回退） |

- UAX #14 对应类别：U+3001/3002/FF0C=CL，U+FF01/FF1F=EX，U+FF1A/FF1B=NS，U+300D 等 Pe=CL——这些类在 LB25/LB30/LB31 规则下均不可起行。【已确认（UAX #14 类别表本轮实读）】
- 设计约束：**半角标点（,.:;!?)]}）不入集**。简中标准表（Ecma）虽含半角，但 DevDesk 场景 URL（`://` `.`）与英文缩写混排频繁，半角入集会误伤（§六）；池 #324 痛点本身是全角逗号。

### 完整集（第二刀可选：按简中全表扩充）

简中（Ecma/OpenXML）：`!%),.:;?]}¢°·'"†‡›℃∶、。〃〆〕〗〞﹚﹜！＂％＇），．：；？！］｝～`；繁中日文小假名/长音符等全表见 Wikipedia 汇编页。DevDesk 以中文用户为主，简中全表足够，日韩表不建议引入。【已确认（Wikipedia 汇编，标注：二手，底层标准 JIS X 4051 / Ecma OpenXML）】

行尾禁则（`$(£¥·'"〈《「『【〔〖（．［｛` 等不收尾）本轮**不做**——#324 未报此症状，且行尾禁则需「回退 1 字」语义，复杂度高一档。

## 三、断行决策点落点（file:line，全部本轮 codegraph 实读）

| # | 位置 | 断行判定 | 禁则挂点 |
|---|---|---|---|
| 1 | `EUI-NEO/core/render/text_layout.cpp:61`（`rebuildLayout`） | `style_.wrap && maxWidth>0 && cursorX>0 && cursorX+advance>maxWidth` → 断在 `shaped[index]` 前 | `shaped[index].codepoint` 直接判（`ShapedGlyph::codepoint` 字段，text.h:30）——TextPrimitive 渲染路径，**全站 `wrap(true)` 文本** |
| 2 | `EUI-NEO/core/render/text_measure.cpp:219`（`measureTextSize`） | `maxWidth>0 && lineWidth>0 && lineWidth+advance>maxWidth` | 码点从 `paragraph[metrics.byteIndices[index]]` 读首字节；**必须与 #1 同判定**（逐位一致契约，text_measure.cpp:206-208） |
| 3 | `EUI-NEO/components/markdown.h:1983`（`layoutInlineRunsWithStyle`→`pushFittingSegment`） | `hasLineItem && width>0 && cursorX+gap+segmentWidth>width` → 断在该 segment 前 | CJK 单字各成 segment（`splitMarkdownTextForLayout`，markdown.h:236-244），即将起行的 `segment.text` 首字节即判——**markdown 渲染路径**；`markdown.h:2016` 长词码点断点（超整行宽才走，全角标点单字不会触发）可不动 |
| 4 | `src/module_agent/agent_module_dialogs.cpp:1217`（`wrapToolDescLines`） | `!line.empty() && measureTextWidth(line+cp)>maxWidth` → 断在 `cp` 前 | `cp` 即下一行首字符，`cp` 首字节判——**DevDesk 应用层旁路**，渲染走逐行 `wrap(false)` 文本节点，EUI-NEO 改动不覆盖 |

- agent 消息卡（`renderMessageRow`，agent_module_dialogs.cpp:144-145 注释「text 块 maxWidth 必须显式给」）走 EUI-NEO text 块 wrap → 随 #1/#2 自动覆盖；其高度估算 `estimateMessageHeight` 若内部走 `measureTextSize` 则随 #2 同步——**待实现时核对**。【待验证】
- 其他模块疑似手工折行点（git_views `ellipsisFitMeasured` 为省略号路径非折行）——实现前用 codegraph 全扫 `measureTextWidth` 调用点盘点 1 次即清。【待验证】

## 四、实现路径选项

**选项 A：拉回法（pull-in / oikomi 简化）——推荐**
断行判定成立时，若即将起行字符 ∈ 禁则集 → 不换行，并入本行（行尾允许超 `maxWidth` ≤1 标点宽）。连续禁则逐字判定，最多超 N 格（加「连续拉回 ≤2 次」钳制防极端溢出）。
- 优点：4 处各 3-8 行；无死循环；不打断英文词（只推后断点）；markdown segment 语义天然兼容（禁则 segment 并入本行 cursorX）。
- 缺点：行尾可能超出容器 1 个标点宽（全角标点窄，视觉可接受；浏览器 `line-break: strict` 的悬挂语义同族）。

**选项 A'：回退法（oidashi）**：断点前移把前字推到下一行。缺点：markdown 英文词场景行尾大空隙、需限回退次数、TextPrimitive 路径要跨 glyph 回退状态。不推荐。

**选项 B：UAX #14 简化 pair 类别表**（建 `text_detail::lineBreakClass` 全表 + pair 判定）：完整但工作量 ×3-5，顺带改善英文 word-wrap——超出 #324 范围，第二阶段再说。

共享基础设施：一张 `bool isProhibitedLineStart(unsigned int cp)`（~50 行，放 `EUI-NEO/core/render/text_internal.h` 的 `text_detail` 或新头），四处共用；DevDesk #4 可 include EUI-NEO 头（DevDesk→EUI-NEO 依赖方向合法）。

## 五、工作量与落地条件

- 落地条件清单：① 上表 4 处全改（漏 #4 = agent 工具卡描述仍禁则起行；漏 #2 = 卡高与渲染行数漂移）；② #1/#2 判定逐位一致（沿用既有契约测试口径，参照 `EUI-NEO/tests/unit/text_ellipsis.cpp`）；③ 若做成开关，`TextSizeCacheKey`（text_measure.cpp:171-180）必须纳入开关位——默认全局启用则不动 cacheKey（推荐，无开关）；④ 纯算法层无平台分叉，不触 `*_win/*_macos`，但验证按纪律双平台跑。
- 代码量：禁则表 ~50 行 + 挂点 4×(3-8) 行 + 单测（EUI-NEO unit 新文件 + DevDesk `tests/test_modules/test_agent_ui.cpp` 补 `wrapToolDescLines` 用例）。
- 估计：**~1 人日**（含双平台验证）。【推测】
- 覆盖对照：TextPrimitive wrap 全站文本 ✔（#1#2）｜markdown 渲染 ✔（#3）｜agent 工具卡描述 ✔（#4，需 DevDesk 侧同批改）｜终端（third_party/Corta 自带 reflow，非本仓排版层）✖ 不在范围｜行尾禁则 ✖ 第二刀。缺口补法：行尾禁则 = 后续需求单独开条。

## 六、风险面

1. **英文混排**：最小集全角 only，半角路径零变化；全角标点夹英文中被拉回仅行尾超宽一格。无新回归面。
2. **URL 不误伤**：markdown 侧 URL 按空白分词整词成 segment，不进词内断点（2016 路径仅整段超行宽才触发）；TextPrimitive 侧 URL 逐字形断是现状行为，最小集不含 `.` `:` `/` → 不加重。**硬约束：半角字符永不入集**（若未来扩全表含半角，须同步加 URL 词法豁免）。
3. **测量/渲染漂移**：#1/#2 不同步改 → `estimateMessageHeight` 类卡高与渲染行数不一致，文本溢出卡外。验收用例：同一 CJK 字符串 `measureTextSize` 行数 == `rebuildLayout` 后 `lines_.size()`。
4. **连续禁则**（「。。。。」）：拉回法行尾超 N 格——钳制连续拉回 ≤2 次，超出放行（接受禁则起行的极端 case，罕见）。
5. **应用层旁路漏网**：本轮只确认 `wrapToolDescLines` 一处；实现阶段 1 次 codegraph 扫 `measureTextWidth` 手工折行调用点全量盘点，漏改即覆盖不完整。

## 来源

精读（本轮实取）：
- UAX #14 Unicode Line Breaking Algorithm，https://www.unicode.org/reports/tr14/ —— 类别表：U+3001..3002 / U+FF0C = CL；U+FF01/U+FF1F = EX；U+FF1A..FF1B = NS；U+300D 等 = CL（Pe）；U+300C = OP（Ps）；LB25/LB30/LB31 相关。【已确认】
- Wikipedia "Line breaking rules in East Asian languages"，https://en.wikipedia.org/wiki/Line_breaking_rules_in_East_Asian_languages —— JIS X 4051 / Ecma OpenXML（简中/繁中/韩）行首/行尾禁则字符全表 + burasage/oidashi/oikomi 三种处理法。【已确认（二手汇编）】

检索失败（如实标注，未采信记忆）：
- W3C《Requirements for Chinese Text Layout》(clreq) https://www.w3.org/TR/clreq/ —— 两次 fetch 被 WAF 拒（"sensitive information" 错误）。字符集以 UAX #14 + Wikipedia 两源交叉替代。
- MDN CSS `line-break` —— 超时未取。
- Pango/Chromium/Blink 引擎一手实现代码本轮未取（预算约束）；「业界引擎按 UAX #14 类别表实现」为【推测】不作依据。

代码侧（本轮 codegraph 实读，已确认）：§三 表格 4 处 file:line。
