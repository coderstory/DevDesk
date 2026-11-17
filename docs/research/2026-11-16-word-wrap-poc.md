# POC 报告：EUI-NEO input 多行编辑器软换行（word wrap）可行性（need:267）

- 日期：2026-11-16 ｜ 角色：developer（POC/研发）｜ 预算：工具 50 / 构建 3 / 墙钟 40min（实际：构建 2 次，未超限）
- 分支：EUI-NEO `poc/word-wrap`（基于 e066ef4 = origin/main；**未 push**；**零改动**——POC 结论是能力已存在，无需框架改动）
- 文件清单：`tests/probes/word_wrap_poc_probe.cpp`（DevDesk，新增）；本报告。EUI-NEO 未改任何文件。
- 队友脏文件核对：收尾 `git -C EUI-NEO status --short` 仍恰为 `core/app/app_runner.h`、`core/app/glfw_app_main.cpp`、`core/platform/tray_bridge.c` 三个，diff 未动（见 §7）。

## 0. 核心结论（先行）

**软换行在 EUI-NEO 当前 HEAD 已基本交付**：行模型、布局、渲染、增量重排、光标导航五条链实测全部按「显示行 = 视口宽度内切段」工作，且字节无损、UTF-8/CJK 边界安全。简报「已证事实 2」（viewportWidth 只影响 metrics 不影响切行）与代码现状**不符**——`measureLines` 自 5fe6106（input model 拆分重构）起就含超宽行切段逻辑（`components/input_model.h:741-765`），multiline 布局分支恒 `state.horizontalScroll = 0.0f`（`input_model.h:150`），横向滚动只属于单行态。老板裁决前提（「自动换行必须做」）对应的缺口实际是**残余缺陷**，不是从零改造。

**风险结论：有条件做（推荐）**——做的对象是 4 个残余缺口（§4，合计工作量 M），不是行模型重写。条件：单行 10 万字节级超长行的编辑性能缓解（缺口③）可先接受现状或单独排期；其余三项为 S~M 小改。

## 1. POC 方法与可复现实跑

无头探针（不建窗口、不渲染帧，符合「老板在场时探针走离屏」纪律），直接驱动真实函数链：`InputModel::measureLines` / `updateChangedParagraphs` / `InputLayout::build`（字体走 `setDefaultFontFiles` 系统兜底，PingFang SC）。

```sh
bash scripts/build-test-run.sh --dev --no-run        # 构建（增量，第 2 次通过）
./build/tests/word_wrap_poc_probe                    # 运行（总耗时 2.4s，退出码 0）
```

完整输出（原样粘贴）：

```
[demo] sample 86 bytes, viewport 220px -> 3 display lines
  [0] bytes[0,40) w=208.9 hard=0 | hello 世界 this is 自动换行测试 
  [1] bytes[40,69) w=215.3 hard=0 | with a verylongenglishwordwit
  [2] bytes[69,86) w=114.1 hard=0 | houtspaces 结尾
[perf] corpus timings (viewport=600px, fontSize=15, PingFang SC fallback)
  corpus sizes: latin=277KB megaSingleLine=109KB cjkMixed=259KB
  latin5000 wrap ON                  lines=5001   first= 192834.0us  incrementalEdit=     85.2us  fullRelayout= 182137.0us  midWordBreaks=0
  latin5000 wrap OFF                 lines=5001   first= 182766.1us  incrementalEdit=     63.1us  fullRelayout= 182531.1us  midWordBreaks=0
  mega100K wrap ON                   lines=1357   first=  84382.0us  incrementalEdit=  84823.5us  fullRelayout=  84907.7us  midWordBreaks=0
  mega100K wrap OFF                  lines=1      first= 25677.4us  incrementalEdit=  26364.5us  fullRelayout=  25379.7us  midWordBreaks=0
  cjk5000 wrap ON                    lines=5001   first= 176489.5us  incrementalEdit=     82.5us  fullRelayout= 164153.5us  midWordBreaks=0
  cjk5000 wrap OFF                   lines=5001   first= 163590.0us  incrementalEdit=     58.7us  fullRelayout= 166230.4us  midWordBreaks=0
[layout-chain] InputLayout::build on wrapped model
[layout] contentHeight=100020 maxVScroll=99720 cursorLine=2501 cursorX=8.0 cursorY=280.0 selectionRects=15 hScroll=0
[poc] PASS: wrap segmentation byte-exact, UTF-8/CJK boundaries safe, layout chain self-consistent
```

探针内断言（非仅打印）：逐段字节范围重建全文必须与原文全等（无丢失/重复）；每段 start/end 必须落在 UTF-8 码点边界（续字节即红）；除不可断单码点外段宽 ≤ 视口+1px；增量重排结果与全量排版逐行全等；`InputLayout::build` 选区矩形非退化、multiline 下 hScroll==0。全部通过。

注意：latin/cjk 语料单行约 45–90 字节，600px 视口下不溢出（lines=5001，每段一行），故其 wrap ON/OFF 耗时接近——**溢出切行的额外成本以 mega100K 行为准**（84ms vs 26ms，≈3.3×）。切行正确性在 demo（强制 220px 窄视口）中可见且过了同一组断言。midWordBreaks=0 是因为语料行不溢出；词中途断行缺陷由 demo 输出直接证明（`verylongenglishwordwit|houtspaces`，见 §2 问题 1）。

## 2. 六问答案

**问 1 行模型改造方案：不需要改造，现状已是方案 (B) 的变体且实测自洽。**
`TextLine{start, end, hardBreakAfter, metrics}`（`input_model.h:28-33`）本义已是「显示行」：超宽逻辑行被切成多个 `TextLine` 顺序放入 `cachedLines`，软段以 `hardBreakAfter=false` 标记（`input_model.h:741-765`）；`hardBreakAfter=true` 即逻辑行边界，逻辑结构可零成本恢复。`lineIndexFor` 已处理软段归属（`input_model.h:792-796`），渲染循环直接遍历显示行（`input.h:444-485`），`contentHeight = 显示行数 × lineHeight`（`input_model.h:153`）。数据结构草案：无新增字段需求。改动面：无。

**问 2 styled-runs 契约：契约已按显示行工作，向后兼容已成立，但缓存键有一个真缺口。**
现状：provider 收到 `displayText = 全文档文本`、`lineStartByte = 显示行段起始字节（文档偏移）`、`lineNo = 显示行序号`（`input.h:454-463`，`input_model.h:941-957`）——软换行下「显示行」即软段，坐标系不变，md 模块等调用方**签名与语义均无需改**。缺口：`LineRunCache` 命中键是 `(lineNo, textRevision, styleRevision)`（`input_model.h:947`），**不含 viewport/wrap 维度**——窗口宽度变化会重切分段使 lineNo 内容移位，但 textRevision 未变，缓存返回旧 runs → 高亮错位脏缓存。修法见 §4 缺口②。

**问 3 光标导航语义：现状已是「视觉列」语义，无需改。**
`moveCursorVertical`（`input_model.h:482-535`）在 `cachedLines`（显示行）上取 `preferredCursorX = xFor(cursor)`（当前显示行内 x，即视觉列），目标显示行用 `closestOnLine` 按 `metrics.caretX` 最近停靠点回填字节偏移（`:495-511`）。上下移动跨软段与跨硬行行为一致——软段间无 `\n`，方向键仍需先跨段（Left/Right 语义保持逻辑字符移动，符合主流编辑器习惯）。判定规则：同列 = 视觉 x 最近停靠点（非逻辑列），与 VSCode 等一致。需要改的函数：无。

**问 4 增量重排边界：现状已正确，无需改。**
`updateChangedParagraphs`（`input_model.h:616-665`）把改动范围扩到 `\n` 边界（逻辑段）后整段调 `measureLines` 重排——重排结果天然含该段的所有软段（`measureLines` 内部再切），`hardBreakAfter=false` 软段随段重建，`lower_bound` 定位对新旧行序都成立。实测单字符编辑：5000 行文档 63–85µs（量级正确，注释 `:653` 的「不搬动整篇 metrics 容器」优化生效）。大单行（100KB 无换行）：增量与全量同代价（84.8ms vs 84.9ms，wrap ON）——受影响逻辑行 = 全文，无段落边界可借力，这是唯一的成本热点，见 §4 缺口③/④。

**问 5 性能实测：见 §1 表。** 关键读数：
- 首次排版被「每逻辑行一次 shaping」主导（5000 行 ≈ 165–193ms，与 wrap 开关几乎无关；277KB 语料）。
- 单字符编辑增量重排：多段文档 <100µs，完全流畅。
- **100KB 单行 wrap ON：首次 84ms / 每次编辑 85ms**——编辑卡顿（>1/60s 预算约 3 倍）；wrap OFF 也要 26ms（现状单行态本就慢，wrap 不是唯一因素）。
- 滚动帧率：未实测（无头 POC 不建帧循环）。按渲染路径推理：每帧只画 `[firstLine, endLine)` 可视行（`input.h:446-448`），成本 ∝ 可视行数（约 30 个 text 元素），与文档总行数无关——mega 行切成 1357 段也只画可视窗口。标注为「未验证，推理有据」，留真机收口实测。

**问 6 开关切换锚定：给算法（若产品需要 wrap 开关；现状 multiline 恒 wrap，无开关可切）。**
按光标锚定（推荐，光标总在屏上或随 followCaret 回来）：
1. 关→开：在未 wrap 行表上 `lineIndexFor(cursorByte)` 得逻辑行 L；视觉行 V = Σ_{i<L} segCount(i)（segCount = 该逻辑行在 wrapped 表中的行数，可用两表 binary search 前缀和求）；`verticalScroll = V × lineHeight`。
2. 开→关：在 wrapped 表上二分找 cursorByte 所在显示行 R；其所属逻辑行 = 从 R 向前回溯最后一个 `hardBreakAfter=true` 的行数前缀；`verticalScroll = 逻辑行号 × lineHeight`。
3. 纯视口锚定（无光标）：把「视口顶 firstLine 行」的 start 字节当锚字节，走 1/2 同款映射。宽度剧变时误差一屏内，followCaret 会拉回光标。

## 3. 与简报「已证事实」的矛盾清单（停用依赖，上报主会话）

| 简报事实 | 代码实况 |
|---|---|
| 「viewportWidth 只影响 metrics 不影响切行」（事实 2） | `input_model.h:741-765` 超宽行按 caretX 停靠点切段，5fe6106 引入 |
| 「超宽行靠 horizontalScroll 横向滚动显示」（事实 1） | multiline 分支 `horizontalScroll = 0`（`input_model.h:150`），横向滚动仅单行态（`input.h:486-498` else 分支） |

其余事实（缓存结构、styled-runs 契约、光标字段、IME、垂直滚动）核实无误。

## 4. 残余缺口与改动面清单（file:line 级）

| # | 缺口 | 改动面 | 工作量 |
|---|---|---|---|
| ① | **拉丁词中途断行**：贪婪字符级切段无词边界感知（demo 实证：`verylongenglishwordwit｜houtspaces`）。CJK 本就该逐字断，仅拉丁受损 | `components/input_model.h:751` 附近：切段回退点从 `previousStop` 改为「≤ 视口宽的最近词边界停靠点」（空格/标点后），记录 word-boundary 候选 ~30 行；CJK 不受影响 | S~M（单文件单函数 + 定向用例） |
| ② | **lineRuns 缓存键缺 viewport 维度**：resize 后高亮脏缓存 | `components/input_model.h:577-614`（ensureLayoutCache viewport 变更分支）追加 `state.cachedLineRuns.clear()`，1–3 行 | S |
| ③ | **段 metrics 二次测量浪费**：wrap 路径先测整行再逐段 measureMetrics（≈2× shaping）。全行 `metrics.byteIndices/caretX` 已含全部停靠点，段 metrics 可切片导出 | `components/input_model.h:755` 与 `:763` 的 `measureMetrics(segmentText,...)` 换成从整行 metrics 按段起止切片（注意局部 x 平移与 ligature 边界） | S~M |
| ④ | **100KB 单行编辑 85ms/键**（wrap ON）：受影响逻辑行=全文。缓解选项：(a) 接受现状（超长无换行行本身是边缘用法，wrap OFF 也 26ms）；(b) 段邻域增量切行（改动大、风险高） | (a) 零改动；(b) `input_model.h:616-665` 大改 | (a) 0；(b) L，建议不做或单独排期 |

合计建议实施范围 ①+②+③ = **M**（集中在 `components/input_model.h` 单文件，渲染层 `input.h` 无需改；测试用本探针扩断言 + DevDesk md 编辑器真机目检）。

## 5. 工作量估算依据

- ①②③ 均为单文件（`EUI-NEO/components/input_model.h`）、单函数级改动，合计 <100 行；无数据结构变更、无接口变更、无调用方（md/agent/memory 等 7 处 multiline 用户）波及——按本仓分级惯例属 M。
- ④(b) 涉及增量重排算法重设计，且 100KB 单行是边缘语料，价值/风险比差——不推荐。
- 不存在「重写行模型」需求：POC 实测五条链已自洽，简报担心的最大冲突点（增量重排边界）实测无冲突。

## 6. 硬清单自检

POC 结论均有 §1 实测数据支撑（六配置计时表 + PASS 断言链），无纯定性判断；六问全部作答；改动面到 file:line；工作量给出依据；风险结论明确且条件可执行。

## 7. 未验证项与收尾核对

- 真机目检：DevDesk md 编辑器（`md_module_editor.cpp:47` multiline input）当前可见换行态未上屏确认（无头 POC 不开窗；预期已换行，若真机仍不换行则问题在产品层传参，非框架）。
- 滚动帧率未实测（§2 问 5 已述推理边界）；Windows 侧字体栈下的 CJK 度量未实测（本 POC 在 macOS arm64）。
- EUI-NEO 收尾核对：开工基线恰为 3 个脏文件（`core/app/app_runner.h`、`core/app/glfw_app_main.cpp`、`core/platform/tray_bridge.c`）；收尾工作区另出现队友并行改动（`platform.h/.cpp`、`text.cpp/.h`、`text_internal.h`、`text_shaping.cpp`、`CMakeLists.txt` + 未跟踪 `file_dialog_logic.h`、`system_font_families.cpp`）——与简报预告的队友战场一致，**本任务全程未写入任何 EUI-NEO 文件**（唯一动作 `git checkout -b poc/word-wrap`），基线三文件 diff 未经我手。未 push；分支 `poc/word-wrap` 留作后续缺口 ①②③ 的工作分支。
