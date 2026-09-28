# Ui::setScrollOffset 上游补齐 + M6 去 id 轮转 hack

> 状态：v2（2026-09-28，5 视角 skeptic 审计 CONFIRMED 项全合：EUI-NEO pending 3 + onChange 2 + M6 删除 2 + perf 3 + testability 4 BLOCKER + 1 minor）。用户拍板「补上游 + 重构 M6」。跨仓库：EUI-NEO（上游框架）+ DevDesk（下游 M6）。

## 1. 问题

DevDesk M6 滚动跟随用「容器 id 两档轮转」hack 强制 scrollView 实例重建以采纳本帧 offset，代价是拉的那帧全量重绘（换 id → elementStructure 变 → `fullPaintRequested_`）+ id 不稳定。根因：EUI-NEO `scrollView` 的 `.offset()` DSL 只在 `ScrollStateInstance` 首次 init 时采纳（`runtime_state_bindings.h:92-97` 的 `!instance.initialized` 守卫），之后每帧保留实例内部 offset、DSL 同名值不生效。

底层 `Runtime::setScrollOffset(stateId, offset)`（`dsl_runtime.h:264` 声明、`runtime_update.h:470-492` 实现）**已实装**，直写 `instance.offset` + 标脏区 + 触发 `onScrollOffsetChanged`，绕过 init-only 守卫。但 DSL 层 `Ui` 类（`dsl.h:1419`）无 `set*` 方法、不持有 Runtime 指针——DevDesk 代码用 `ui.*` 调不到。需补 `Ui::setScrollOffset`（DSL 层 pending 挂起 → 帧末消费路由到 `Runtime::setScrollOffset`）。

## 2. 方案

### 2.1 EUI-NEO 侧：补 `Ui::setScrollOffset`（pending 挂起，镜像 requestFocus）

照搬 `Ui::requestFocus`（`dsl.h:1534`）的 pending 挂起 + 帧末消费模式，**严格镜像其语义**（审计修正）：

- `Ui` 类新增成员 `std::vector<std::pair<std::string, float>> pendingScrollOffsets_;`（`dsl.h` `Ui` 私有区，紧挨 `pendingFocusId_` :1854）。可选 `reserve(1)`（perf minor：单消费方单 scrollView，每帧 ≤1 条）。
- 公开方法 `void setScrollOffset(const std::string& id, float offset)`（public 区，紧挨 `requestFocus`）：**存 RAW id**（审计 minor：requestFocus 存 raw、消费时经 `ui_.find` 解析；spec v1 写「请求时 resolveId」错——scope 上下文错配时静默 no-op）。`pendingScrollOffsets_.emplace_back(id, offset);`（不立即 resolveId）。
- **enqueue 时 per-id 去重**（审计 major + perf minor：同帧同 id 多次 set → 只留最后值，避免多次 onChange/脏标/击穿一次性标志）：emplace 前扫一遍，同 id 覆盖 offset，不新增条目。
- Runtime 消费点：`Runtime::compose` 帧末，在 `syncScrollStateBindings()`（`runtime_lifecycle.h:34`）**之后**、与 `consumePendingFocusId()`（:45）同级，新增 `consumePendingScrollOffsets()`：
  - **take-and-clear**（审计 major：镜像 consumePendingFocusId 的「先 swap 取走清空再处理」，防 onChange 回调重入 setScrollOffset 时迭代器失效 UB / 无限增长）：`auto snapshot = std::move(pendingScrollOffsets_); pendingScrollOffsets_.clear();` 后遍历 snapshot。
  - 对每条：`ui_.find(stateId)` 解析 id（与 requestFocus 同路径，帧末 scope 上下文）；**找不到该元素 → 丢弃该请求**（审计 minor：本帧元素树无该元素 = 请求丢弃，不排队跨帧——镜像 requestFocus 的 dsl.h:1531 注释语义）；找到 → 调既有 `Runtime::setScrollOffset(stateId, offset)`。
- 消费时机在 syncScrollStateBindings 之后，保证 scroll state instance 已建好（`instances_.scrollStates` 是 try_emplace get-or-create，当帧树存在即命中）。

**不破坏 init-only 语义**：`Ui::setScrollOffset` 走 `Runtime::setScrollOffset` 直写 `instance.offset`（:482），完全不经过 `element.scrollOffset` 字段；下一帧 `syncScrollStateBindings` 跑 else 分支时 `instance.offset` 已是新值、被 clamp 保留、不被 element 覆盖。

**不循环**（审计 perf/testability 修正风险 3 论证）：`scrollView` 不设 `composeOnScrollOffsetChange`（`dsl.h:212` 默认 false，全仓仅 `virtualmasonry.h:124` 设），故 `setScrollOffset` 不会因 onChange 置 `composeRequested_`；且消费即清空队列（跨帧无残留）；`dsl_window_runtime.h:100-110` 的二次 compose 时队列已空。真正防循环的是「消费即清空 + composeOnScrollOffsetChange=false」，**不是** closeEnough 早返回（closeEnough 只挡同值脏标累积）。

### 2.2 DevDesk M6 侧：删 hack、固定 id、调用点后移、改调 ui.setScrollOffset

**可整个删**（id 轮转 hack 全部，审计补全 :1751 + :1941）：
- 常量 `kLiveScrollIdGen0` / `kLiveScrollIdGen1`（`markdown_module.cpp:214-215`）+ 注释块 203-213
- 字段 `liveScrollGen_`（:3832）—— **含 :1751 日志行** `" gen=" + std::to_string(liveScrollGen_)`（审计 major：v1 漏列，字段删后悬空引用必编译失败）
- 函数 `liveScrollId()`（:1689-1691）
- 翻转点 `liveScrollGen_ ^= 1`（:1744）
- `enterLiveState` 里 `liveScrollGen_ = 0`（:2784）+ 关联注释 2779-2782
- 注释中所有「换容器 id」「两档轮转」「代」字样（1688、1718-1720、1898-1899、2781）

**改两处引用点**（v1 只写「改一行 :1900」，实际两处）：
- scrollView 构建点（:1900）`liveScrollId()` → 固定字面量 `"md.live.scroll"`
- **每帧回读钳位点**（:1941）`ui.find(liveScrollId())` → `ui.find("md.live.scroll")`（审计 major：v1 漏列）

**改一处调用 + 调用点后移**（审计 major：核心结构性修正）：
- `liveScrollFollowMaybe`（:1722-1752）里 `liveScrollGen_ ^= 1`（:1744）改成 `ui.setScrollOffset("md.live.scroll", d.offset)`
- **调用点从 :1856（drawLiveColumn 内，scrollView build 之前）后移到 scrollView build + 回读钳位块（:1900-1944）之后**：v1 调用点用**上帧** `liveMaxOffset_`/几何判定，但消费用**本帧** `instance.maxOffset`；同帧删块内容变矮时 clamp(d.offset,0,Y_new) ≈ instance.offset → closeEnough 早返回 → onChange 不触发 → `liveProgrammaticScroll_` 标志残留 → 用户滚轮被误判程序来源。后移后判定 max 与送达 max 同帧 → 只能 no-op（不置标志）或必 apply（echo 同帧同步消费标志），残留前置条件被结构性消除。
- `liveScrollFollowMaybe` 当前签名无 `Ui&` 参数（由 drawLiveColumn 内调用，调用点有 `ui`），需把 `ui` 透传进函数，或把「落地」上移到调用点。

**不变**：`mdLiveFollowOffset` 纯判定（`md_live_edit.cpp:195-210`）、`liveFollow_` 状态机（true/false 转移）、`liveScrollY_` 记账、onChange 回写、no-op 抑制（`d.offset == liveScrollY_`）、每帧回读钳位（1941-1943）——与 id 轮转正交。

### 2.3 onChange 副作用处理（标志位方案，钉死为唯一方案）

`Runtime::setScrollOffset` 会触发 `owner->onScrollOffsetChanged`（`runtime_update.h:485-490`）。M6 的 onChange（`markdown_module.cpp:1916-1923`）写 `liveScrollY_ = v; liveFollow_ = false;`。程序化拉一次会触发 onChange → 把刚设的 `liveFollow_` 关掉 → 跟随失效。

**方案：`liveProgrammaticScroll_` 标志位**（v2 钉死，删「期望值比对」替代——审计 major 证伪更差：onChange 收到的是 clamp 后 instance.offset，与「刚设的」d.offset 在内容变矮帧不等 → 假阴性杀自己跟随；0/max 边界假阳性）：
- M6 新增成员 `bool liveProgrammaticScroll_ = false;`（紧挨 `liveFollow_`）
- `liveScrollFollowMaybe` 落地前置 `liveProgrammaticScroll_ = true;` → 调 `ui.setScrollOffset(...)`（配合 §2.2 调用点后移，此时 d.offset 用本帧 max → apply 必触发 onChange 同帧消费标志）
- onChange 回调：`if (liveProgrammaticScroll_) { liveScrollY_ = v; liveProgrammaticScroll_ = false; return; /* 不置 liveFollow_=false */ }` 原有 `liveScrollY_ = v; liveFollow_ = false;` 走 else（用户滚轮来源）
- **enterLiveState 复位**（审计 major）：补 `liveProgrammaticScroll_ = false;`（:2775-2787 现只重置 liveScrollGen_/liveScrollY_）
- **纵深防御**：下帧 drawLiveColumn 开头清零 `liveProgrammaticScroll_`（防任何漏清路径，即便 §2.2 后移已结构性消除残留）

**为何不改 EUI-NEO 的 `Runtime::setScrollOffset` 不触发 onChange**：onChange 对滚动条拖动、惯性运动（`updateScrollMotion` :527、`updateRuntimeScrollDrag` :557 都调 setScrollOffset 并依赖 onChange）是必要的；EUI-NEO 侧不应为 M6 一个消费方改全局行为。来源区分在 M6 侧做。

## 3. 改动清单

| 仓库 | 文件 | 改动 |
|------|------|------|
| EUI-NEO | `core/dsl.h` | `Ui` 类加 `pendingScrollOffsets_` 成员 + `setScrollOffset(id, offset)` 公开方法（存 raw id + per-id 去重） |
| EUI-NEO | `core/runtime/runtime_lifecycle.h` | `compose` 帧末 `syncScrollStateBindings` 后加 `consumePendingScrollOffsets()` 调用 |
| EUI-NEO | `core/runtime/runtime_update.h` | `consumePendingScrollOffsets()` 实现：take-and-clear（move+clear）→ 遍历 snapshot → `ui_.find` 解析 + 找不到丢弃 → 调既有 `Runtime::setScrollOffset` |
| EUI-NEO | `tests/unit/ui_set_scroll_offset.cpp`（**新建**，审计 testability minor） | Runtime-driven 单测：`core::dsl::Runtime rt; rt.compose(...)` 模式；断言 onChange 收到 v==目标 offset、同帧同 id 连续 set 两次只一次 onChange、pending 消费后清空、本帧无该元素→丢弃 |
| DevDesk | `src/module_markdown/markdown_module.cpp` | 删 id 轮转 hack（§2.2 全列表含 :1751/:1941）+ scrollView 固定 id + `liveScrollFollowMaybe` 调用点后移到 :1944 后 + 改调 `ui.setScrollOffset` + 加 `liveProgrammaticScroll_` 标志 + onChange 区分来源 + enterLiveState 复位 + drawLiveColumn 开头清零 + 跟随状态转移 LOG_DEBUG 行 |
| DevDesk | `tests/test_modules/test_markdown.cpp` | **改写 3 个 M6 用例**（审计 major：v1 漏列，固定 id 后原 id 轮转断言必中断）：`md_module_live_scroll_follow_pull`（:4916-4917）、`_wheel_disarms`（:5006-5007/:5014/:5023-5024/:5030）、`_wrapped_caret`（:5128）—— id 断言全改固定 `"md.live.scroll"`；**新增**「md.live.scroll 跨帧 id 恒定 + 实时态元素存在性」断言（bare-Ui 可达，照 :469-485 模式） |
| DevDesk | `tests/test_modules/test_md_live_edit.cpp` | **不动**（审计 testability minor：文件 charter 自约纯函数、不碰 Ui/Runtime；本改动行为用例不落这里） |

> 行为用例（消费/onChange/follow 保持/滚轮关跟随）的可测性见 §4。

## 4. 验收（v2 重写：审计 testability 4 BLOCKER 收口）

### 4.1 EUI-NEO 侧（Runtime-driven 单测，`tests/unit/ui_set_scroll_offset.cpp`）

**不经私有 `instance.offset`**（审计 BLOCKER 1：Runtime::instances_ 私有无 accessor，用 onChange 回调值等价断言）：
- `ui.setScrollOffset(id, offset)` 在 compose 帧末被消费 → scrollView.onChange 回调收到 v == 目标 offset（等价断言 instance.offset 的钳位后值）
- **同帧同 id 连续 set 两次**（不同 offset）→ 只触发一次 onChange（perf minor + eui-neo-pending major：per-id dedup 验证），onChange v == 第二次值（后值覆盖）
- pending 队列消费后清空（跨帧无残留）
- 本帧元素树无该 id → 请求丢弃（不排队跨帧）
- init-only 语义不被破坏：未调 setScrollOffset 的 scrollView，DSL `.offset()` 仍只首帧生效（回归）

### 4.2 DevDesk M6 侧（拆自动化可达 + 手工点验）

**可达自动化**（bare-Ui harness，test_markdown.cpp）：
- `md.live.scroll` 跨帧 id 恒定（不再轮转）—— `ui.find("md.live.scroll")` 跨连续 compose 帧命中同一实例
- 实时态元素存在性回归（既有用例零改动，除 3 个 id 轮转用例改写）

**手工点验清单**（审计 BLOCKER 2/3/4：liveFollow_ 私有 + 滚轮无头不可驱动 + fullPaintRequested_ 私有无 getter + RenderFrameStats 需真 GPU —— 降级为手工 + 日志）：
- ① 跟随保持：连续打字 caret 越出安全带 → 视口拉一次 → caret 始终可见（操作者肉眼）
- ② 用户滚轮关跟随：用户滚轮后 `liveFollow_=false`（肉眼：滚轮后打字不再自动拉视口）
- ③ 无全量重绘观感：滚动跟随时无明显整屏闪烁（对比改造前 id 轮转帧）
- ④ 跟随状态转移日志：新增 `LOG_DEBUG("[live] follow: prog=1 applied offset=X")` / `LOG_DEBUG("[live] follow: wheel disarmed")` 行，操作者跑 dev 构建核对状态机（替代直接读私有 `liveFollow_`）

> phase-3 spec §7 M6 原始验收本就是「手工清单③④ + 日志无高度失配」，本 spec v1 误标为「回归用例」已删（审计 BLOCKER 2）。

## 5. 风险（v2 修正）

| # | 风险 | 处置 |
|---|------|------|
| 1 | 同帧同 id 多次 setScrollOffset | **钉死：enqueue 时 per-id 去重，同 id 只留最后值**（审计 eui-neo-pending major + perf minor）；消费至多一次 onChange；§4.1 单测断言 |
| 2 | resolveId 时机错配 → 静默 no-op | 存 raw id，消费时 `ui_.find` 解析（镜像 requestFocus）；找不到丢弃（§2.1） |
| 3 | ~~closeEnough 早返回防循环~~ | **论证修正**（审计 perf/testability）：防循环 = composeOnScrollOffsetChange=false（scrollView 不设）+ 消费即清空队列；closeEnough 只挡同值脏标累积。结论（不循环不掉帧）不变 |
| 4 | `liveProgrammaticScroll_` 漏清 | **结构性消除**（审计 onchange major）：调用点后移到 build+回读后 → 判定 max 与消费 max 同帧 → 只能 no-op（不置标志）或必 apply（echo 同帧消费标志）；加 enterLiveState 复位 + drawLiveColumn 开头清零作纵深防御。**删「期望值比对」替代**（证伪更差） |
| 5 | EUI-NEO 改动需先合 master 才能让 DevDesk 用 | EUI-NEO 是 sibling dir 同步编译；commit 顺序：先 EUI-NEO commit → 再 DevDesk commit |
| 6 | 测试文件 charter | 行为用例进新 Runtime-driven 文件（EUI-NEO tests/unit）；test_md_live_edit.cpp 保持纯函数；test_markdown.cpp 只加 id 恒定断言（审计 testability minor） |

## 7. 流程

本 spec v2（5 视角审计全合）→ 派开发（EUI-NEO 一条 worktree + DevDesk M6 一条 worktree，**串行**：EUI-NEO 先合 master → DevDesk M6 再开工用新 API）→ 各自真编译（`merge-tree` 无冲突 ≠ 能编译）→ M6 用例改写 + 手工点验清单交付。
