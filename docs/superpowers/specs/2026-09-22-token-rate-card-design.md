# 首页 Token 速率实时卡片 — 调研与 UI 设计

日期：2026-09-22 ｜ 状态：设计定稿（待评审） ｜ 关联 spec：§8.4（Home 仪表盘）、§10.4（SSE 流式代理）

## 1. 需求

在 DevDesk 首页（Home 仪表盘）增加一张卡片，实时展示 Claude 代理当前的**每秒输出 token 数**（多路流式并发时取总和）与**累计 token**。核心诉求是"实时速率 + 总量"；趋势线等增强项按成本收益决定。本文档是调研 + UI 设计，不含实现。

## 2. 调研发现

### 2.1 首页现状（src/module_home/）

- **布局**：`home.stats` 是一个 `ui.row`，内含 4 张统计卡。每卡 `statW = (contentW - 20*3) / 4`、`statH = 120`，由 `drawStatCard()`（home_module.cpp:29）统一绘制：`ui.stack` 内绝对定位——label 在 y=20（13px-1 字号，textSecondary）、number 在 y=42（`fontSizeDisplay` 48px、700 字重、56px 行高）、badge 在 y=98（11px，textTertiary）。卡片底色 `t.colors.surface`、圆角 `radiusLarge`(14px)、1px `borderDefault` 边框，整卡 `onClick` 跳转对应页。accent 卡（逾期任务）用 `t.colors.error` 染数字和 badge。
- **数据刷新**：仅 `onStart()` 时 `HomeService::getStats()` 查一次 DB + 右上角手动"↻ 刷新"按钮。**没有任何定时/事件驱动刷新机制**——这是本需求要新引入的。
- **垂直空间预算（关键约束）**：Home 是 `ui.column`（无滚动）。最小窗口 1200×720 下：padding 80 + header 72 + stats 120 + recent（60+5×44+12=292）+ actions 44 + 3×gap 60 = **668px，仅剩 52px 余量**。任何"新增一行卡片"的方案在最小窗口会溢出。这直接决定了布局选型（见 §4）。

### 2.2 流式链路：chunk 从哪过、SSE 在哪可见

数据路径（streaming_proxy.h 头注释 + 源码核实）：

```
Thread B (libcurl write cb) → StreamingResponse 内部队列(≤100 chunk, 背压)
    → Thread A (cpp-httplib provider lambda) readChunk() 拉一个 chunk
    → writeChunk() 写给客户端
```

- **SSE 事件唯一自然可见点**：`forwardStream()` 的 provider lambda（streaming_proxy.cpp:230-262）。每个 `chunk` 是 libcurl 给的原始字节，**可能把一条 SSE 事件从中间切断**（TCP 分段边界 ≠ 事件边界），所以不能直接按 chunk 解析，需要一个小的**事件重组器**：累积缓冲区，按 `\n\n`（`sse::kEventSeparator` 常量已有）切事件，剥 `data: ` 前缀后交给 JSON 解析。重组器状态随流生命周期（provider 捕获的 shared_ptr 存活期）。
- **取消/断开时钩子仍可运行**：三条结束路径——客户端断开（`isWritable()==false` → `cancel()` + return false）、上游错误（readChunk 返回 Error）、自然结束（End）；另外 `releaser` 在响应彻底结束时（含断开）必定触发。**流结束时的统计收尾（activeStreams-1、以最后已知累计值定格）放在 provider 返回 false 前和 releaser 里都安全**。
- **多路并发**：每个 `handleStreamingRequest()`（proxy_router.cpp:281）独立创建 `StreamingResponse` + provider，cpp-httplib 线程池天然支持并发。用一个自增 uint64 流 id 区分；聚合统计 = 各流之和。
- **重要发现（范围外）**：流式路径的 `claude_call_logs` 落库发生在 `forwardStream()` 返回后（proxy_router.cpp:379-393），`latencyMs` 实际是"到响应头"的时间，流式正文时长和字节数**没有**被记录（responseBytes 为空）。本任务不修，记入开放问题。

### 2.3 SSE usage 语义（核实结果）

官方 SSE 事件序列（skill 内置 curl 示例，与实现一致）：

```
event: message_start
data: {"type":"message_start","message":{...,"usage":{"input_tokens":N,"output_tokens":M}}}

event: content_block_delta      ← 不带 usage
data: {"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"Hello"}}

event: message_delta
data: {"type":"message_delta","delta":{...},"usage":{"output_tokens":12}}
```

- `message_delta.usage.output_tokens` 是**累计值**（本条 message 到目前为止的输出总数），不是增量。速率 = 相邻两次事件的累计差 ÷ 时间差。`message_start` 的 usage 在 `message.usage.output_tokens`（嵌套一层），初值很小。
- 官方文档站点（docs.anthropic.com / platform.claude.com）当前区域拦截，联网核实"message_delta 是否在整个流中途周期性到达"未完成。**设计前提**：现代 API 行为是 message_delta 随生成周期性下发累计 usage（Claude Code 的实时 tps 显示即基于此）；若实测只在结尾出现一次，回退方案是流中途用 content_block_delta 的文本量做粗估（约 4 字节/token，UI 标注"估算"）。**开发第一步先打一条真实流式请求验证事件时序**，再定实现细节。
- 解析工具：`streaming_proxy.cpp` 已有极简顶层 JSON 扫描器（`findTopLevelField`），但 usage 是嵌套字段，不适用。module 层可依赖 platform，直接用 `platform::json::parse()`（yyjson 薄封装，src/platform/json.h）最省事。

### 2.4 数据模型：DB 里没有 token

`claude_call_logs`（migrations.h:104）只有 `request_bytes/response_bytes/latency_ms` 等，**没有任何 token 列**。因此"累计 token"只能是**进程内会话累计**（从启动开始），不做历史持久化——加列需要 migration v3 + 流结束回写（且流式日志时点本身不准，见 2.2），收益低，明确排除出本期范围。

### 2.5 跨模块数据通道：core 层共享统计，可行且有先例

分层规则禁止 module_home 依赖 module_claude_proxy。现有两个先例：

| 先例 | 位置 | 模式 |
|------|------|------|
| `registerAppDatabase()/appDatabase()` | core/database.cpp:79 | `std::atomic<T*>` 全局指针，main 注册，module 读 |
| `registerAppRouter()/appRouter()` | core/router.cpp:53 | 同上 |
| `ClaudeService::global()` | module 层单例 | main.cpp 设置；settings 模块为此破例依赖 claude_proxy（有注释豁免） |

新通道 **`core/proxy_stats`** 照抄前两例的精神但更简单——统计对象无外部依赖，直接在 core 内静态持有（类似 `core::theme::current()` 的进程级静态），不需要 register/appDatabase 式的指针注册：

```cpp
// src/core/proxy_stats.h（示意）
namespace core::proxy_stats {
struct Snapshot {          // reader 一次拷贝拿走，无锁读取
    int      activeStreams;     // 活动流数
    int64_t  sessionTokens;     // 进程启动以来累计 output tokens
    double   rateTps;           // 最近 ~2s 窗口的 tok/s（多流总和）
    int64_t  lastActivityMs;    // 距最近一次 token 事件的毫秒数
};
// writer（module_claude_proxy 流式钩子调用）：
uint64_t beginStream();                        // 返回流 id，activeStreams+1
void updateStream(uint64_t id, int64_t cumulativeTokens);  // 每个 message_delta 事件
void endStream(uint64_t id);                   // provider 返回 false / releaser
// reader（module_home compose 调用）：
Snapshot snapshot();
}
```

实现要点：内部 `std::mutex` + 每流一条 `{lastCum, lastUpdateSteady}` + 一个 16 槽 `(steady_clock, 总累计)` 环形采样缓冲（writer 在每次 updateStream 时 push 采样点），`rateTps` 由 reader 用"最新采样 − 最老采样 ≥2s 前最近的采样"现算，窗口内无新 token 则随时间衰减到 0。writer 在网络线程、reader 在 UI 线程，全部经 mutex，临界区是几十字节的拷贝，无性能问题。core 不依赖 platform/EUI-NEO，只用 `<atomic>/<mutex>/<chrono>`，分层规则零违反。

### 2.6 EUI-NEO 能力核实（grep ../EUI-NEO/ 源码，非假设）

- **compose 不是每帧重跑**：DSL.md:693——纯 hover/transition 不重新 compose；`composeRequested` 被置位（如 click 回调）才重跑 compose 并 full paint。所以"每帧读统计"不成立，**必须主动制造重绘**。
- **实时刷新的正解是 `onTimer`**：`core/dsl.h:762` 定义 `onTimer(seconds, cb)`；`runtime/runtime_update.h:379` 确认其语义：周期触发（触发后自动重新武装），回调执行后置 `composeRequested_ + paintRequested_`，计数期间置 `animating_` 维持渲染循环。用法：卡片里放一个 0 尺寸 `ui.stack(...).onTimer(0.5f, []{})`，每 0.5s 触发整页 compose 重跑，compose 里读 snapshot 画新数字。linechart 组件内部就是用这个机制驱动动画（linechart.h:218）。
- **帧率/CPU 代价**：DevDesk 未设 `.fps(...)`，动画期间主循环按显示器刷新率跑（渲染后端架构.md:362-363）。onTimer 常驻意味着 Home 页可见期间渲染循环持续运转（静止页本可 wait-events 休眠）。代价主要是每 0.5s 一次的 compose + dirty paint，compose 本身很轻（Home 树几十个节点）；非计时帧只有 update 遍历。可接受，但如果实测 CPU 占用可观，门控方案是"仅 activeStreams>0 或 3s 内有活动时挂 0.5s timer，否则挂 2s 慢轮询"——一期先不门控（见开放问题）。
- **文本每帧更新开销**：text 换字符串 → 一次 text prepare + dirty rect，2-3 个 text run 完全无感。
- **linechart 不适合做迷你趋势线**：组件 API（linechart.h）是整卡尺寸设计——固定内边距（plotY=70、高度-112）、0-1 归一化 values、百分比 tooltip、自带标题/网格/数据点。塞进 120px 高的统计卡会把绘图区压成 `max(1, h-112)`≈1px。迷你 sparkline 得用 `ui.polygon` 自绘（linechart 内部的胶囊线段画法可抄），属于中等工作量 → 归入可选增强，不进一期。

### 2.7 速率算法

- **口径**：每流维护 `lastCum`，事件到达时 `delta = cum - lastCum`；窗口速率 = 全部流总累计在最近 2s 内的增量 ÷ 实际经过时长（用环形采样缓冲取"≥2s 前最近的采样点"）。2s 窗口在"够新"与"不抖"之间平衡；Claude Code 状态栏 tps 也是秒级窗口。
- **显示平滑**：不做 EMA 数学平滑，靠**显示层量化**消抖——UI 每 0.5s 刷新一次 + 整数显示（<100 tok/s 时保留 1 位小数），观感已足够稳；再叠 EMA 会让"开始/停止"变得迟钝。若实测仍抖，加一层 `显示值 += 0.3×(真值-显示值)` 的追逐（写 home 模块本地状态即可，不动 core）。
- **采样存储**：16 槽环形缓冲绰绰有余（0.5s UI 刷新率下 2s 窗口只需 4-5 个有效点；burst 事件多算几个点也无妨）。
- **静止衰减**：读 snapshot 时若 `lastActivityMs > 2000`，速率直接按窗口内实际增量计算（自然为 0），无需额外逻辑。

## 3. 数据流设计（端到端）

```
libcurl write cb ─→ [StreamingResponse 队列] ─→ forwardStream provider (Thread A)
                                                    │ 每拿到一个 chunk：
                                                    │  1. 喂 SSE 重组器（累积 + 按 \n\n 切事件）
                                                    │  2. "data:" 行 → platform::json::parse
                                                    │  3. type==message_start → updateStream(id, message.usage.output_tokens)
                                                    │     type==message_delta → updateStream(id, usage.output_tokens)
                                                    │  4. writeChunk 转发客户端（原有逻辑零改动）
                                                    │ 流结束（End/Error/断开）+ releaser → endStream(id)
                                                    ▼
                              core::proxy_stats（mutex + 每流累计 + 16 槽采样环）
                                                    ▲
Home compose（每 0.5s 由卡片内 onTimer 触发重跑）── snapshot() ──┘
```

要点：
1. **转发路径零语义改动**——统计是 provider 里"顺路看一眼"，不碰队列、不碰背压、不加拷贝（重组器只做 substr/查找）。
2. **非流式（缓冲）路径不计入**：`handleRequest` 的响应里也有 usage，但本期只做流式（速率概念只对流式有意义）；累计口径 = "流式输出 token 会话累计"。记开放问题。
3. **故障安全**：JSON 解析失败/字段缺失 → 跳过该事件（统计是尽力而为，绝不能让解析异常影响转发）；`endStream` 保证在所有结束路径幂等调用（内部按 id 查存在性）。

## 4. UI 方案对比与推荐

### 方案 A：第 5 张统计卡（同一行，推荐）

`home.stats` 行内 4 卡变 5 卡，`statW = (contentW - 20*4) / 5`（1600 窗口 288px → 1200 最小窗口 208px，48px 大数字 3-4 字符放得下）。完全复用 `drawStatCard` 视觉语言，唯一改动是行内多一个调用 + 数字带单位。

```
┌──────────────┬──────────────┬──────────────┬──────────────┬──────────────┐
│ Token 速率    │ 今日笔记      │ 待办任务      │ 逾期任务      │ API 调用      │
│ (tok/s)      │              │              │              │              │
│              │              │              │              │              │
│  ▂ 47        │  3           │  5           │  1           │  12          │ ← 48px，活动时 primary 色
│              │              │              │              │              │
│ ●2 路并发     │ 今日修改过的   │ 未完成总数    │ 已过截止时间   │ 今日代理调用   │ ← badge 行
│ 会话累计 12.4k│ 笔记         │              │              │              │
└──────────────┴──────────────┴──────────────┴──────────────┴──────────────┘
   208~288px        （四等分 → 五等分，间距 20px 不变，行高 120px 不变）
```

- **状态设计**（三态，无过渡动画，最简）：
  - **空闲**：数字 `0`、灰色（textTertiary）；badge "空闲 · 会话累计 0"
  - **流式中**：数字 primary 色（Claude Clay `#D97757`，与"逾期"的 error 红区分开——速率高不是坏事，不该用警示红）；badge 前缀 `●`（success 绿）+ "N 路并发 · 会话累计 12.4k"
  - **刚结束**：速率自然衰减到 0（2s 窗口），数字回落灰色，累计定格保留。不做回落动画——速率归零本身就是最好的"结束信号"，加动画纯属复杂度。
- **数据项取舍**：大数字 = 当前速率；badge = 并发路数 + 会话累计。已用时、趋势线均不做。
- **点击**：整卡跳转 Claude 代理页（与其他卡行为一致）。

### 方案 B：全宽实时横幅卡（不推荐做一期）

在 stats 行与"最近编辑"之间插一张 `contentW × 88` 全宽卡：左 1/3 大数字速率 + live 圆点，中 1/3 会话累计，右 1/3 活动流明细，未来可嵌 sparkline。

```
┌────────────────────────────────────────────────────────────────┐
│ ● 实时输出        47 tok/s        会话累计        活动流         │
 │                  ▁▂▄▆█▇▄▂           12.4k          2 路         │ ← 88px 高
└────────────────────────────────────────────────────────────────┘
```

信息容量和"实时感"最强，但 **§2.1 的垂直预算否决它**：现有 668px + 88 + 20 gap = 776 > 720 最小窗口高度，Home 无滚动必然溢出；要么砍"最近编辑"列表条数（伤害现有功能），要么等 Home 改造为 scrollView（超范围）。留作二期 Home 布局升级时的候选。

### 方案 C：A + 迷你趋势线（增强项，推迟）

在方案 A 卡片的 number 右侧（约 60×28px）用 `ui.polygon` 自绘最近 2 分钟速率 sparkline（linechart 的胶囊线段画法移植，8-12 个采样点）。

```
│ Token 速率 (tok/s)                                              │
│  ▂ 47   ▁▂▄▆█▇                                                  │
│ ●2 路并发 · 会话累计 12.4k                                       │
```

卡片内 208px 宽度下趋势线只有 60px，可读性存疑；且 polygon 逐段自绘 + transition 是 80-100 行新代码。**结论：一期不做**。若二期做，放方案 B 的全宽卡里价值更高。

### 推荐：方案 A

理由：① 零垂直空间成本，绕开 720px 溢出硬约束；② 完全复用现有卡片视觉语言（绝对定位、字号层级、token 配色），新代码集中在"数据来源"而非"新布局"；③ 核心诉求（实时速率 + 总量 + 并发数）全覆盖；④ 每张卡从 4 等分变 5 等分，横向信息密度提升是全局一致的，不产生视觉孤儿。YAGNI：趋势线、已用时、每流明细都等真实使用反馈再说。

## 5. 变更清单预估

| # | 文件 | 改动 | 估量 |
|---|------|------|------|
| 1 | `src/core/proxy_stats.h/.cpp`（新增） | 共享统计：begin/update/endStream + snapshot，mutex + 采样环 | ~120 行 |
| 2 | `src/core/CMakeLists.txt` | 加 1 个 .cpp | 1 行 |
| 3 | `src/module_claude_proxy/streaming_proxy.cpp`（或新文件 `sse_usage.h/.cpp`） | SSE 事件重组器 + message_start/delta usage 解析 + 三处流生命周期钩子（provider 建立前 begin、事件循环内 update、结束/releaser 时 end） | ~130 行 |
| 4 | `src/module_claude_proxy/CMakeLists.txt` | 可能加新文件 | 1-2 行 |
| 5 | `src/module_home/home_module.cpp` | 5 卡布局（4→5 等分 + 新 drawStatCard 调用）+ 卡内 onTimer(0.5s) 驱动刷新 + snapshot 读取/格式化（k/万 缩写、状态三态） | ~50 行 |
| 6 | `tests/test_core/` 新增 proxy_stats 用例 + `tests/test_modules/` 新增 SSE 重组用例 | 见 §6 | ~150 行 |

注意：**W5 工作流正在并行分支改 home_module.cpp 等文件**，落地前先 rebase/协调，尤其 #3 #5 两个文件。

## 6. 测试计划

单测（延续 tests/ 迷你 doctest 风格）：
1. **SSE 重组器**：事件跨 chunk 切断重组、一 chunk 多事件、`\r\n` 分隔、非 `data:` 行忽略、`[DONE]` 容忍、畸形 JSON 跳过不抛。
2. **usage 解析**：message_start（嵌套 message.usage）/ message_delta（顶层 usage）两个形状；缺 usage 字段的事件安全跳过。
3. **proxy_stats**：单流速率窗口计算（注入带假时钟的采样）；多流总和；endStream 后 activeStreams 归零且累计保留；静止 >2s 速率衰减为 0；begin/end 幂等；并发 begin/update/end（多线程小压测）。
4. **home 格式化纯函数**：12.4k / 1.2M 缩写、三态文案（可抽成纯函数单测）。

集成/人工验证：真实 API key 跑一次流式请求（同时验证 §2.3 的 message_delta 周期性前提），观察卡片 idle → 流式 → 结束三态；双客户端并发流验证总和；断开客户端验证 activeStreams 回落。按 CLAUDE.md 验证原则补 idle/流式两张截屏。

## 7. 开放问题

1. **message_delta 周期性**（§2.3）：官方文档被区域拦截未能在线核实，开发第一步用真实请求打日志验证；若仅结尾一次，回退 content_block_delta 文本量粗估（标"估算"）。
2. **onTimer 常驻 CPU**：一期接受 0.5s 常驻轮询；实测若 Home 页 CPU 占用可观，加"活动门控"（空闲时降 2s）。
3. **非流式请求是否计入会话累计**：缓冲路径响应里也有 usage.output_tokens，本期不计；要计的话在 handleRequest 落库点顺路 updateStream 即可（但速率口径会混入"瞬时完成"的大跳变）。
4. **claude_call_logs 补 token 列 + 流式时长修正**（§2.2 发现的既有缺口）：migration v3 + 流结束回写，独立小任务，不混入本期。
5. **input_tokens 是否也统计**：本期只展示 output；core::proxy_stats 的结构预留字段成本为零，UI 展示等需求出现再加。
6. **会话累计的口径**：进程启动以来（重启清零）——是否需要"今日累计"（跨重启、需落库）待用户反馈。
7. **W5 分支协调**：home_module.cpp / streaming_proxy.cpp 的并行改动冲突风险，落地前对齐。
