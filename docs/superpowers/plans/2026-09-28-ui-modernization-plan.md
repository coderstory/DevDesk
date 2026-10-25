# DevDesk UI 现代化（EUI 组件全量接入）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 全仓 UI 改用 EUI 组件，停止手绘（手写 rect+text 的「卡」、列表、图表一律换成 EUI 组件）；统一三套右键菜单坐标口径（三处错位根因）；全仓补过渡 / 阴影 / 渐变，消除「硬切」与「平面感」；上游 EUI-NEO 补三处能力缺口（input onContextMenu 透出 / contextMenu 宽度自适应 / styledMenu 收起态高度回收）。

**Architecture:**
- **M4 上游贡献（先行）**：`../EUI-NEO/` 三处组件能力补齐，经「隔离产 patch → 非隔离 apply」跨仓库流程落地；DevDesk 侧 bump `最低依赖 rev 软校验（已删，ADR 0006）` 并删对应 hack。
- **M1 右键统一**：新增统一锚点换算函数（core 层纯函数，含 Y、含滚动偏移参数），三处调用点（todo 看板 / md 树 / md 编辑器）只传 PointerEvent 原始坐标，位置/宽度/钳制交给 `contextMenu` 组件自身；claude profile 列表补右键（编辑/复制/删除/设为当前，复用既有动作）。
- **M2 全局过渡**：md 视图三态切换、dialog/toast 开合（常驻构建 + opacity 收起——框架无「已删元素回放动画」机制）、todo 卡 drop 落位；基调：弹窗/视图 0.15s OutCubic、列表/微交互 0.1s OutQuad。
- **M3 手绘换组件**：Home 统计卡 → `card` + Token 速率 `lineChart` sparkline + API 调用量 `barChart`；profile 列表与调用记录 → `virtualList`；Todo 卡片 → `card`（拖拽业务逻辑保留）；shadow/gradient 补齐。
- 业务逻辑一律不动（拖拽、CRUD、代理流转、设置持久化、统计口径）；不铺开 workshop 组件；不改主题 token 体系。

**Tech Stack:** C++20 / CMake + Ninja + MSYS2 MinGW64 + ccache（本仓 `scripts/build-test-run.sh` 自动处理 PATH 与 configure，worktree 首建即用）/ SQLite / 自制迷你 doctest（`REGISTER_TEST` + `DOCTEST_CHECK*`，**不是**真 doctest——grep `TEST_CASE` 找不到用例属正常）。

**Spec:** `docs/superpowers/specs/2026-09-28-ui-modernization-design.md`（**V2**，commit `69a3698`）—— 计划与 spec 逐条一致；冲突**以 spec 为准**并回报。

## Global Constraints

- **执行环境**：涉及编译/测试的子任务必须 worktree 隔离（`isolation: "worktree"`），主工作树禁跑构建；完成后 worktree 内原子 commit（**显式 pathspec，禁 `git add -A`**）并报 sha → 主会话串行合并进 master → 立即 `git worktree remove` + `prune` 清残留。
- **命令**：构建 + 定向测试一律 `bash scripts/build-test-run.sh --no-run --filter <子串>`。**禁全量重建、禁全量套件**（全量只在里程碑收尾由主会话跑一次）。每里程碑过滤串：M1 `uiModern` / `markdown` / `todo` / `claude`；M2 `markdown` / `todo` / `claude` / `uiModern`；M3 `home` / `claude` / `todo` / `uiModern` / `proxy_stats`。
- **测试框架是自制迷你 doctest**：`REGISTER_TEST(name, "描述", {...; return true;})` 三参形态（照 `tests/test_modules/test_menu_beautify.cpp`）或二参；宏实参**不能有裸逗号**（模板实参、`{a, b}` 初始化列表都算）→ 用类型别名或逐项构造。
- **不抛 C++ 异常**：错误一律 `core::Result<T>`；`-Wall -Wextra -Werror` 全开——删函数/删参数时以编译器 unused 报错为准删最小范围。
- **模块隔离**：`core` 不依赖 `platform` / EUI-NEO 运行时——换算函数放 core 即**纯函数**（零 include 风格，照 `src/core/ui_geometry.h` 既有 `windowToCompose`）；`module_*` 之间不可互依赖。
- **跨仓库红线**：EUI-NEO 与 DevDesk 同级非 submodule。**隔离 agent 无法对其它仓库做 git 操作**——正确形态：隔离 agent 产 patch（diff）+ 用文件副本模拟依赖真跑构建验证 → 另起**非隔离** agent 把 patch `git apply` 进 `../EUI-NEO`、跑 EUI-NEO 自身编译、原子 commit（消息写明「DevDesk 需要 X 功能」）、清理副本；非隔离线 prompt 必须写死「只许改 `../EUI-NEO`，禁止碰 DevDesk 文件」。
- 代码注释与 commit message、UI 文案**一律中文**（技术名词除外）；行尾 LF。
- 范围纪律：只做 spec §7 / §10 清单内的改动；**4523e09 已落地两项不在任务清单**（todo 菜单 y 减滚动偏移、md 空路径隐形卡修复），只保留回归断言。
- commit 粒度：每个 Task 完成后一次原子 commit；消息用本计划的给定文案。

## 实施前代码事实（2026-09-28 codegraph + 源码实测，各 Task 前提）

1. **U1 缺口确认**：`InputBuilder`（`EUI-NEO/components/input.h:43-570`）无 `onContextMenu`；元素级回调已存在于 `core/dsl.h:1049`（`BuilderBase::onContextMenu(PointerEvent, Rect)`，无条件置 `interactive=true` + `cursor=Hand`）；`button.h:97` 已有同款透出可照抄。`CursorShape` 枚举仅 `Arrow/Hand`（`core/input/input_types.h:11`）——**无文本光标**，挂接后需显式恢复 Arrow。
2. **U2 缺口确认**：`contextmenu.h` `build()`（:111-154）直接 `std::min(width_, …)`（:118），无文本测量；`measureTextWidth` 在 `core/render/text.cpp:2197`（`TextPrimitive::measureTextWidth`）；`styledmenu.h` `showStyledMenu` 内已有可移植测量口径（最长行文本实宽 + 行首图标位 + 尾部位 + 双侧 inset）。
3. **U3 缺口确认**：`styledmenu.h:163-165` 注释明示「窗口尺寸定死、无运行期 size setter」；`showStyledMenu` 按**全展开**行数预留窗高（宁高勿叠）。
4. **坐标口径**（`runtime_input.h:416-447`）：`onContextMenu` 派发前已 `/dpiScale`，坐标为 **Logical 窗口坐标**；页面区原点 x = `Router::pageOriginX`、**y = 0**（`core/ui_geometry.h:14` 注释）→ 换算 = x 减 pageOriginX、y 按同规则；`scrollOffsetY` 参数只服务「基于非滚动几何」的调用。
5. **card 组件无 hover states**（`CardStyle` `card.h:13-31` 无 states 字段）→ hover 反馈由卡内交互层补（透明 `.press`/`.hit` rect 带 states——旧代码 `rect.onClick` + 上层非交互文本本就可穿透命中，模式同源）。
6. **lineChart 空值陷阱**：`linechart.h:83-85` `values_` 为空时**填充 demo 数据**——sparkline 传值前必须防空（空序列归一为 `{0.0f}`）。
7. **`proxy_stats::Snapshot`**（`proxy_stats.h:15-20`：activeStreams/sessionTokens/rateTps/lastActivityMs）**无历史序列** —— sparkline 数据源需在 writer 侧加最近速率采样环（纯数据扩展，不动既有字段）。
8. **EUI 侧默认过渡**：contextMenu / virtualList / scroll 均默认 `0.12s OutCubic`（`virtuallist.h:186`），lineChart 默认 `0.16s OutCubic`（spec §2.4 引用）：这些组件自身已带过渡的落点**不再重复加**。

## 文件结构（改动落点一览）

**DevDesk 侧**

| 文件 | 职责 | 动作 |
|------|------|------|
| `src/core/ui_geometry.h` | 统一锚点换算（core 纯函数） | 加 `MenuAnchor` + `menuAnchorForPointer`（M1-1） |
| `src/module_todo/todo_ui_dialogs.h` / `.cpp` | todo 右键菜单 | `Ctx` 加锚点坐标字段；锚点改指针坐标；删板几何推算；M4-2 后删 190 写死宽（M1-2） |
| `src/module_todo/todo_ui_board.cpp` | 卡片/列头右键入口 + 卡容器 | onContextMenu 存指针坐标；drop 落位过渡（M2-3）；卡片容器换 card（M3-3） |
| `src/module_todo/todo_module.cpp` | `drawContextMenu` 调用点 | 去 `columnScrolls` 实参（M1-2） |
| `src/module_markdown/markdown_module.cpp` | 树/编辑器菜单 + 三态视图 | 树/编辑器锚点统一换算 + 删写死宽（M1-3/M1-4）；删 hit 挂靠 hack（M4-1 后）；三态切换过渡（M2-1）；弹窗过渡（M2-2） |
| `src/module_claude_proxy/claude_ui_list.cpp` | profile 列表 | 补右键菜单（M1-5）；virtualList + 卡换 card + 行过渡（M3-2） |
| `src/module_claude_proxy/claude_ui_state.h` | 列表状态 | 右键菜单状态字段（M1-5） |
| `src/module_claude_proxy/claude_ui_logs.cpp` | 调用记录 | scrollView 平铺换 virtualList（M3-2） |
| `src/module_home/home_module.cpp` | 统计卡 / charts / recent 区 | `drawStatCard` 换 card + sparkline/barChart 尾参；recent 背景换 card（M3-1） |
| `src/core/proxy_stats.h` / `.cpp` | 速率采样环 | 加最近 N 点采样环 + `rateSeries()`（M3-1 数据扩展） |
| `src/module_home/home_service.h` / `.cpp` | API 当日分布 | 加 `getTodayApiCallSeries`（M3-1 数据扩展） |
| `CMakeLists.txt` + `CLAUDE.md` | 依赖 rev | `最低依赖 rev 软校验（已删，ADR 0006）` bump + 「当前依赖 rev」记录（M4-4） |
| `tests/test_modules/test_ui_modern.cpp`（**新增**）+ `tests/CMakeLists.txt` | 无头用例 | §9 用例（M1-1/4523e09 回归/M3 接线） |
| `src/module_settings/*`、`EUI-NEO/` 之外均不动 | — | 范围纪律 |

**EUI-NEO 侧（M4，上游贡献）**

| 文件 | 职责 | 动作 |
|------|------|------|
| `components/input.h` | U1 | `InputBuilder::onContextMenu` 透出 + build() 挂接（恢复 Arrow） |
| `components/contextmenu.h` | U2 | `autoWidth()`：按菜单树最长项 `measureTextWidth` 算宽，再走既有 X 钳 |
| `components/styledmenu.h`（+ `include/eui/detail/dsl_app_impl.h` 视 U3 盘点） | U3 | 收起态高度回收：开窗/组头切换时重测窗尺寸 |

## 执行方式（批处理口径与串行约束）

**交叠文件**（同一文件同一时刻只有一个写者，按序串行）：

- `markdown_module.cpp`：M1-3 → M1-4 → M2-1 → M2-2(md 部分) 一条实施线串行；
- `todo` 系（todo_ui_dialogs/board/module.cpp）：M1-2 → M2-2(todo 部分) → M2-3 → M3-3 一条线串行；
- `claude_ui_list.cpp/.h`：M1-5 → M3-2 一条线串行；
- `home_module.cpp`：M3-1 → M3-4(home 部分) 同线。

**批次**（每批一条 worktree 实施线，每 Task 一次原子 commit，批尾派独立复核）：

- **批 A（上游，M4）**：M4-1 → M4-2 → M4-3（EUI-NEO，非隔离线串行提交）→ M4-4（DevDesk 侧 bump）。批尾复核：对照 spec §6 逐项核 patch 内容已落地、DevDesk 侧无残留 hack。
- **批 B（M1）**：M1-1 → M1-2 → M1-3 → M1-4 → M1-5。批尾复核：对照 spec §3 逐条核对（三处锚点统一、4523e09 修复不在清单、写死宽度清况）。
- **批 C（M2）**：M2-1 → M2-2 → M2-3。
- **批 D（M3）**：M3-1 → M3-2 → M3-3 → M3-4。批尾复核：对照 spec §5 清单 + §2.1 计数复查（`.transition(`/`.shadow(`/`.gradient(`/`lineChart`/`barChart`/`virtualList` 落点全绿计数）。

**依赖**：M1-4 的「去 hack」依赖 M4-1、三处删写死宽度依赖 M4-2 —— M4 未合入前该两项带过渡期实现（保留 hack / 保留写死宽但不参与锚点，spec §11）；M3-2 依赖 M1-5（同文件且右键挂在换装后的容器上）。

---

## Task M4-1: U1 —— input 透出 onContextMenu（EUI-NEO 上游）

**Files:**
- Modify: `../EUI-NEO/components/input.h`
- (贡献附带) `../EUI-NEO/examples/` 或 `../EUI-NEO/tests/unit/` 最小用例（以 EUI-NEO 现有 runner 为准，不新建基建）

**Interfaces:**
- Produces：`components::InputBuilder::onContextMenu(std::function<void(const core::PointerEvent&, const core::Rect&)>)` —— 与 `button.h:97` 同签名
- Consumes（DevDesk M1-4）：`input(...).onContextMenu(cb)` 链式挂接，不再 `ui.find(kEditorInputHitId)` 逐帧挂靠

- [ ] **Step 1: 隔离 agent 在 DevDesk 侧副本操作并产 patch**

隔离 worktree 中先读 `../EUI-NEO/components/input.h`（可用 `cp` 副本模拟依赖），按下方代码块改好副本，`git diff` 产出 patch 文本。

`InputBuilder` 公开方法区（`onCancel` 之后、`onFocus` 之前）加：

```cpp
    /** @brief 右键菜单回调透出（2026-09-28 DevDesk UI 现代化 U1）：
     *  元素级 onContextMenu（core/dsl.h BuilderBase）此前 input 不透出，
     *  消费方只能每帧 ui.find(".hit") 手工挂靠。挂接会把悬停光标置为
     *  Hand（dsl.h onContextMenu 无条件赋值），input 悬停语义是箭头 →
     *  组件内部显式恢复 CursorShape::Arrow（框架无文本光标）。 */
    InputBuilder& onContextMenu(std::function<void(const core::PointerEvent&, const core::Rect&)> callback) {
        onContextMenu_ = std::move(callback);
        return *this;
    }
```

私有成员区（`onKeyEvent_` 附近）加：

```cpp
    std::function<void(const core::PointerEvent&, const core::Rect&)> onContextMenu_;
```

`build()` 内回调拷贝区（`onFocus` 拷贝行附近）加：

```cpp
        const std::function<void(const core::PointerEvent&, const core::Rect&)> onContextMenu = onContextMenu_;
```

`build()` 的 hit 链末尾（`hit.onKeyEvent(...)` 之后、根 content lambda 内）加：

```cpp
                // U1：右键菜单透出——仅设置时挂接；onContextMenu 会把
                // cursor 置 Hand，显式恢复 Arrow（input 悬停指针语义）
                if (onContextMenu) {
                    hit.onContextMenu(onContextMenu).cursor(core::CursorShape::Arrow);
                }
```

- [ ] **Step 2: 非隔离 agent apply + EUI-NEO 自身编译验证**

非隔离 agent（prompt 写死「只许改 `../EUI-NEO`」）：`git -C ../EUI-NEO apply <patch>` → 编译验证（**优先用既有构建目录增量**：`cmake --build ../EUI-NEO/build-check`；不存在则最小 configure 一个临时目录，**禁止清理/重建 EUI-NEO 已有构建目录**；macOS 侧编译不在 Windows 验证范围，列入未验证项）→ 附带最小用例或 example 随贡献（以 EUI-NEO 既有 `tests/unit/` runner 为准，无对应入口则在报告说明）→ 原子 commit：

```bash
git -C ../EUI-NEO add components/input.h <附带用例文件>
git -C ../EUI-NEO commit -m "feat(input): InputBuilder 透出 onContextMenu（DevDesk 需要编辑器右键菜单，U1）"
```

- [ ] **Step 3: 报 sha + 记录**

报告 commit sha（M4-4 bump 用）、编译验证输出、附带用例情况。

## Task M4-2: U2 —— contextMenu 宽度自适应（EUI-NEO 上游）

**Files:**
- Modify: `../EUI-NEO/components/contextmenu.h`

**Interfaces:**
- Produces：`ContextMenuBuilder::autoWidth(bool value = true)`（默认关，**向后兼容**：不调时行为与现状完全一致）；开启时 `build()` 按菜单树最宽项 `core::TextPrimitive::measureTextWidth` 算宽，再走既有 X 钳（:121）
- Consumes（DevDesk M1-2/3/4/5 / M1-1 后）：三处 `.size(190/160/170, 36)` 改 `.autoWidth()` 并删写死常量

- [ ] **Step 1: 隔离产 patch（接口 + 构件）**

`ContextMenuBuilder` 公开方法区（`size` 之后）加：

```cpp
    /** @brief 宽度自适应（2026-09-28 DevDesk UI 现代化 U2）：按菜单树
     *  最长项文本实宽（measureTextWidth，text.cpp:2197）+ 双侧 inset +
     *  级联箭头预留区计算宽度，再走既有 X 钳兜底截断；默认关 = 行为与
     *  未开启时完全一致（向后兼容）。 */
    ContextMenuBuilder& autoWidth(bool value = true) {
        autoWidth_ = value;
        return *this;
    }
```

私有成员区（`zIndex_` 附近）加 `bool autoWidth_ = false;`。

`context_menu_detail` 命名空间（既有 `menuHeight` 旁）加：

```cpp
// U2（2026-09-28 DevDesk UI 现代化）：菜单树最深一层的最宽文本实宽
inline float measureWidestText(const std::vector<ContextMenuItem>& items,
                               const theme::ThemeMetricTokens& metrics) {
    float widest = 0.0f;
    for (const ContextMenuItem& item : items) {
        const float textW = core::TextPrimitive::measureTextWidth(
            item.text, {}, metrics.typography.option);
        widest = std::max(widest, textW);
        widest = std::max(widest, measureWidestText(item.children, metrics));
    }
    return widest;
}
```

`build()` 的宽度计算（现 :117-118）替换为：

```cpp
        const float inset = metrics_.spacing.small;
        const float itemHeight = resolvedItemHeight();
        // U2 宽度自适应：autoWidth 时按最宽文本项实宽 + 双侧 inset +
        // 级联箭头预留区（移植 styledmenu.h showStyledMenu 的 measureTextWidth
        // 口径）；单行超长项交给下方既有 X 钳截断兜底
        float targetWidth = width_;
        if (autoWidth_) {
            targetWidth = inset * 2.0f +
                          context_menu_detail::measureWidestText(items_, metrics_) +
                          metrics_.spacing.section;
        }
        const float width = std::min(targetWidth, std::max(0.0f, screenWidth_ - metrics_.spacing.section));
```

（若 `ContextMenuItem` 有图标位/富文本字段且实测盒宽与此口径不符，以实际渲染文本对拍 `styledmenu.h` 同款口径调整——两处都必须同源。）

- [ ] **Step 2: 非隔离 apply + EUI-NEO 编译验证 + 附带用例**

`git -C ../EUI-NEO apply` → `cmake --build ../EUI-NEO/build-check`（同 M4-1 口径）→ 附带最小测试（U2 纯测量函数可入 `tests/unit/`；无 runner 入口则在报告说明）→ commit：

```bash
git -C ../EUI-NEO commit -m "feat(contextmenu): autoWidth 宽度自适应（DevDesk 需要右侧菜单自适应，U2）"
```

- [ ] **Step 3: 报 sha**

报告 commit sha、build 输出。

## Task M4-3: U3 —— styledMenu 收起态高度回收（EUI-NEO 上游）

**Files:**
- Modify: `../EUI-NEO/components/styledmenu.h`
- Modify: `../EUI-NEO/include/eui/detail/dsl_app_impl.h`（+ 后端窗口持久化文件，视 Step 1 盘点结果）

**Interfaces:**
- Produces：运行期窗尺寸 setter（候选：`core::window::requestWindowResize(Handle, w, h)` 或 `app` 层 `requestWindowResize(pageId, w, h)` 队列，模式同既有 `dslWindowRequests` 消费队列）；`showStyledMenu` 开窗尺寸改按**当前折叠态**行数（展开态行数经运行时 setter 动态补高，替换「全展开预留」）
- Consumes：组头 toggle 处（`expandedSet` 切换重算 `rows` 的现有回调）接入重测

- [ ] **Step 1: 盘点 `../EUI-NEO/core/window/` 与后端窗口持久化**

确认三件事：① `core::window` 有无运行期 resize 通道（`requestWindowResize` / `setWindowSize` 类）；② 后端（glfw/sdl app 实现层）窗口句柄注册表能否按 `Handle`/`pageId` 路由 resize；③ 现状 `showStyledMenu` 开窗尺寸口径（物理像素，`styledMenuWindowSize` :259 已取整）。结果记录在报告：A=已有通道直接接 / B=需新增最小 resize 队列（>2 文件或 >60 行的规模变化**停下上报**，不擅自扩范围）。

- [ ] **Step 2: 实施（按盘点结果走 A 或 B）**

A：`showStyledMenu` 开窗尺寸由 `fullRows` 改 `currentRows` 计算（其余宽度自适应逻辑不动），组头 toggle 后按最新 `rows` 重算 `styledMenuPanelHeight` → 调既有 resize 通道。

B：`dsl_app_impl.h` 加 `DslWindowResizeRequest{ pageId, width, height }`（物理像素、已取整，与 `styledMenuWindowSize` 同口径）+ 队列 + `requestWindowResize(pageId, w, h)` + `consumeWindowResizeRequests()`；后端消费（每帧冲刷，模式同 `consumeWindowRequests`），窗口按 pageId 路由后调平台 resize；`showStyledMenu` 侧：`StyledMenuRuntime` 增加 `int curWidth/curHeight` 记录当前窗尺寸，组头 toggle 回调重算 `rows` 后请求新尺寸。

折叠/展开**双向**都重测（收起回收高度是 U3 主目标、展开补高是防溢出）。

- [ ] **Step 3: 非隔离 apply + 编译 + 附带用例 + commit**

`git -C ../EUI-NEO apply` → `cmake --build ../EUI-NEO/build-check`（同前口径）→ commit（多条文件分开 commit 或一条，消息标明 U3）：

```bash
git -C ../EUI-NEO commit -m "feat(styledmenu): 收起态高度回收（运行时尺寸 setter，DevDesk 需要，U3）"
```

- [ ] **Step 4: 报 sha + 盘点结论**

报告：盘点结论（A/B）、commit sha、编译输出。

## Task M4-4: DevDesk bump `最低依赖 rev 软校验（已删，ADR 0006）`

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `CLAUDE.md`（「当前依赖 rev」记录）
- Test: 无（编译面验证并入批 B 首个任务）

**Interfaces:**
- Consumes：M4-1/2/3 的 EUI-NEO commit sha

- [ ] **Step 1: 取 M4-1..3 的合入后 EUI-NEO HEAD sha**

`git -C ../EUI-NEO rev-parse --short HEAD`。

- [ ] **Step 2: 两处同步**

`CMakeLists.txt:53`：`set(最低依赖 rev 软校验（已删，ADR 0006） "269a8c4")` → 新 sha（`src/module_settings/settings_config.cpp:103` 的 `#ifndef` 兜底**不动**——注入点是 CMake）；`CLAUDE.md` 「**当前依赖 rev**」行同步（`269a8c4（2026-09-27，U1 尺度契约合入点…）` → 新 sha +「（2026-09-28，UI 现代化 M4 U1-U3 合入点）」）。spec §7 指明的两处同步点即此。

- [ ] **Step 3: Commit**

```bash
git add CMakeLists.txt CLAUDE.md
git commit -m "chore(eui): 最低依赖 rev 软校验（已删，ADR 0006） bump 至 M4 合入点（UI 现代化 U1-U3）"
```

---

## Task M1-1: 统一锚点换算函数（core 纯函数）

**Files:**
- Modify: `src/core/ui_geometry.h`
- Test: `tests/test_modules/test_ui_modern.cpp`（**新增**，见 Step 2）+ `tests/CMakeLists.txt` 注册

**Interfaces:**
- Produces：`core::MenuAnchor { float x; float y; }` + `core::MenuAnchor menuAnchorForPointer(float windowX, float windowY, float pageOriginX, float scrollOffsetY, float dpiScale = 1.0f)`
- Consumes：M1-2/3/4/5 全部调用点

- [ ] **Step 1: 先写测试并跑预期失败**

`tests/test_modules/test_ui_modern.cpp` 新建（文件头注释照 `test_menu_beautify.cpp` 风格），首条用例：

```cpp
// tests/test_modules/test_ui_modern.cpp —— UI 现代化无头用例（2026-09-28）：
//   1. 统一锚点换算纯测（含 Y、含滚动偏移、dpi 退化）
//   2. 4523e09 隐形卡修复回归（断言状态、不依赖回调）
//   3. virtualList / card 换装后的布局不变式
//   4. charts 数据接线与空序列守卫
REGISTER_TEST(uiModernAnchorPure,
              "ui: 统一锚点换算（含 Y、含滚动偏移、dpi 退化）", {
    // 基准：窗口 (100, 200)，页面原点 x=20，dpi=1.0（onContextMenu 通道口径）
    const core::MenuAnchor a0 =
        core::menuAnchorForPointer(100.0f, 200.0f, 20.0f, 0.0f, 1.0f);
    DOCTEST_CHECK_EQ(a0.x, 80.0f);
    DOCTEST_CHECK_EQ(a0.y, 200.0f);
    // 滚动偏移（基于非滚动几何的锚点）：
    const core::MenuAnchor a1 =
        core::menuAnchorForPointer(100.0f, 200.0f, 20.0f, 36.0f, 1.0f);
    DOCTEST_CHECK_EQ(a1.y, 164.0f);
    // dpi=2.0（onPress/onDrag 通道口径：原始值 / 框架值）：
    const core::MenuAnchor a2 =
        core::menuAnchorForPointer(100.0f, 200.0f, 20.0f, 0.0f, 2.0f);
    DOCTEST_CHECK_EQ(a2.x, 30.0f);   // 100/2 - 20
    DOCTEST_CHECK_EQ(a2.y, 100.0f);
    // 退化 dpi（0/负/NaN）→ 按 1.0 口径（windowToCompose 同款）：
    DOCTEST_CHECK_EQ(
        core::menuAnchorForPointer(100.0f, 200.0f, 20.0f, 0.0f, 0.0f).x, 80.0f);
    return true;
});
```

`tests/CMakeLists.txt` 文件列表（:23 起，按字母序）加一行 `test_modules/test_ui_modern.cpp`。跑 `bash scripts/build-test-run.sh --no-run --filter uiModern` 预期失败（找不到 `menuAnchorForPointer` 编译错——符合 TDD 预期失败）。

- [ ] **Step 2: 实现**

`src/core/ui_geometry.h`（`windowToCompose` 之后，保持文件零 include 纯函数风格）：

```cpp
// UI 现代化 §3.1（2026-09-28）：右键菜单统一锚点换算。
// 三处调用点（todo 看板 / md 树 / md 编辑器 / claude 列表）不再各自推算板
// 几何 / 裸 y，只喂 PointerEvent 原始坐标；位置、宽度、Y/X 钳制交给
// contextMenu / styledMenu 组件自身（组件内已有 clampMenuY / X 钳兜底）。
// 口径（runtime_input.h:416-447 实测）：onContextMenu 派发前已 /dpiScale、
// 坐标为 Logical 窗口坐标；页面区原点 x = Router::pageOriginX、y = 0
// （本文件头注释）→ x 减页面原点、y 按同规则即可。
// scrollOffsetY：仅当锚点基于「非滚动几何」的调用需要（如 4523e09 的
// cardTop - scrollY 口径）；基于指针原坐标的调用一律传 0。
struct MenuAnchor {
    float x = 0.0f;
    float y = 0.0f;
};

inline MenuAnchor menuAnchorForPointer(float windowX, float windowY,
                                       float pageOriginX, float scrollOffsetY,
                                       float dpiScale = 1.0f) {
    return {windowToCompose(windowX, pageOriginX, dpiScale),
            windowToCompose(windowY, 0.0f, dpiScale) - scrollOffsetY};
}
```

- [ ] **Step 3: 跑预期通过 + commit**

```bash
bash scripts/build-test-run.sh --no-run --filter uiModern
# 期望：uiModernAnchorPure 通过
git add src/core/ui_geometry.h tests/test_modules/test_ui_modern.cpp tests/CMakeLists.txt
git commit -m "feat(core): 统一右键锚点换算函数（UI 现代化 M1-1）"
```

## Task M1-2: todo 看板右键锚点改指针坐标

**Files:**
- Modify: `src/module_todo/todo_ui_dialogs.h` / `todo_ui_dialogs.cpp`
- Modify: `src/module_todo/todo_ui_board.cpp`
- Modify: `src/module_todo/todo_module.cpp`
- Test: `tests/test_modules/test_ui_modern.cpp`

**Interfaces:**
- Consumes：`core::menuAnchorForPointer`（M1-1）
- Produces：`drawContextMenu` 新签名（去 `columnScrolls` 形参）；`Ctx` 增 `anchorX/anchorY`（float，右键时 PointerEvent 窗口坐标）

- [ ] **Step 1: 写测试跑预期失败**

`test_ui_modern.cpp` 加（照 `tests/test_modules/test_todo_ui.cpp` 既有 compose harness 驱动，断言形态照 `test_menu_beautify.cpp` headless 风格）：

```cpp
REGISTER_TEST(uiModernTodoMenuAnchor, "todo: 右键菜单锚点 = 指针换算坐标", {
    // 走既有 todo compose harness：构造一列含一张卡的看板 → 模拟卡片
    // onContextMenu（ev 窗口坐标 (400, 300)）→ 断言
    //   d.ctx.anchorX/anchorY 已写入；打开菜单后 ui.find("todo.ctx")
    //   的 position 与 core::menuAnchorForPointer(400, 300, pageOriginX,
    //   0, 1.0) 一致（板几何不再参与）；
    // 列头菜单同口径（cardIndex < 0 分支）。
    return true;
});
```

预期失败：编译错（`menuAnchorForPointer`/Ctx 新字段不存在）或运行失败。

- [ ] **Step 2: 实现（锚点 + 去几何），`columnScrolls` 形参按编译器 unused 报错删最小范围**

`todo_ui_dialogs.h` 的 `Ctx` 结构加：

```cpp
    float anchorX = 0.0f;  // 右键时 PointerEvent 窗口坐标（UI 现代化 M1-2）
    float anchorY = 0.0f;
```

`todo_ui_board.cpp` 两处 `onContextMenu` 处理器（列头 :283 区、卡片 :406 区）在既有 `d.ctx.*` 赋值处追加：

```cpp
        d.ctx.anchorX = static_cast<float>(ev.x);
        d.ctx.anchorY = static_cast<float>(ev.y);
```

`todo_ui_dialogs.cpp` `drawContextMenu`（:202-263）：删除 `menuW = 190.0f`、`cardLeft/cardTop` 板几何推算与 `columnScrolls` 形参，锚点换换算（**滚动语义随指针坐标自然正确**——事件坐标已是视觉窗口坐标，4523e09 的 `columnScrollOffset` 减法由此不再需要；`columnScrolls` 形参与 `todo_module.cpp:662` 对应实参一并删除，若模块侧取其值的函数此后无其它调用者则一并删（以 `-Werror` unused 为准））：

```cpp
    // 锚点 = PointerEvent 原始坐标经统一换算（本批 M4-2 已合入 →
    // 宽度交 autoWidth；若走过渡期，保留 .size(190.0f, 36.0f) 但不参与锚点）
    ::core::Router* router = ::core::appRouter();
    const core::MenuAnchor anchor = core::menuAnchorForPointer(
        d.ctx.anchorX, d.ctx.anchorY,
        router ? router->pageOriginX() : 0.0f, 0.0f, 1.0f);

    components::contextMenu(ui, "todo.ctx")
        .open(d.popups.isOpen(Popup::Ctx))
        .screen(screen.width, screen.height)
        .position(anchor.x, anchor.y)
        .autoWidth()
        .items(std::move(items))
        .theme(themeTokens)
        .zIndex(d.popups.zOf(Popup::Ctx))
        .onSelectPath([&host](const std::vector<int>& path) {
            host.contextSelectPath(path);
        })
        .onOpenChange([&d](bool v) { d.popups.setOpen(Popup::Ctx, v); })
        .build();
```

（`core::appRouter()` 为 Router 单例访问，与 `markdown_module.cpp` 的 `windowToPageX` 同源；若本文件另有更贴的取得方式沿用，但路径必须唯一。`menuAnchorForPointer` / `autoWidth` 的可用性前提：本计划批序 M4 在 M1 之前，批 A 合入后 M1-2 即可直接用；若批 A 未合入而实施 M1-2，则按 spec §11 走过渡期——锚点照样换换算，宽度保留 `.size(190.0f, 36.0f)` 并在 commit 注明。）

- [ ] **Step 3: 跑预期通过 + commit**

```bash
bash scripts/build-test-run.sh --no-run --filter uiModern
bash scripts/build-test-run.sh --no-run --filter todo    # 既有 todo 用例回归
git add src/module_todo/todo_ui_dialogs.h src/module_todo/todo_ui_dialogs.cpp \
        src/module_todo/todo_ui_board.cpp src/module_todo/todo_module.cpp \
        tests/test_modules/test_ui_modern.cpp
git commit -m "fix(todo): 右键菜单锚点改指针坐标 + 换算函数（UI 现代化 M1-2）"
```

（过渡期与「删写死宽度收尾」的口径统一见 M1-4 Step 4 说明。）

## Task M1-3: md 树菜单统一换算

**Files:**
- Modify: `src/module_markdown/markdown_module.cpp`
- Test: `tests/test_modules/test_ui_modern.cpp`（4523e09 回归断言）

**Interfaces:**
- Consumes：`core::menuAnchorForPointer`（M1-1）
- 回归断言对象：`openFileContextMenu` 空 relPath 分支早退前**直接赋值** `ctxMenuOpen_ = false`（4523e09 已落地，**本 Task 不实现该修复**，只锁回归）

- [ ] **Step 1: 写测试跑预期失败**

`test_ui_modern.cpp` 加（照 `tests/test_modules/test_markdown.cpp` 既有 compose harness 驱动模块；spec §9 断言形态——**断言状态字段本身，不依赖 onOpenChange 回调**）：

```cpp
REGISTER_TEST(uiModernMdTreeNoGhost, "md: 空路径右键早退不留隐形菜单态（4523e09 回归）", {
    // 走既有 markdown compose harness：把模块置于「当前选中路径为空」态 →
    // 触发 openFileContextMenu("", ev) 空路径分支 → 断言
    // ctxMenuOpen_ == false（直接赋值语义；onOpenChange 回调不会触发）。
    return true;
});
```

预期失败：目前测试能过（4523e09 已修）——这条是**回归锁**，随本次 commit 一起落地（先红后绿不适用，属「既有修复回归断言」，在 commit 说明里注明）。

- [ ] **Step 2: 实现（树锚点统一换算 + 删 160 写死宽依赖 M4-2）**

`markdown_module.cpp` 匿名命名空间（`windowToPageX` :248 旁）加模块侧适配：

```cpp
// UI 现代化 §3.1：右键锚点统一换算（与 windowToPageX 同基准：onContextMenu
// 通道 runtime 已 /dpiScale → dpiScale 传 1.0；y 页面原点恒 0 不另减；
// scrollOffsetY 供「基于非滚动几何」的调用，本轮全部传 0）
core::MenuAnchor menuAnchorForPointerInPage(const eui::PointerEvent& ev,
                                            float scrollOffsetY = 0.0f) {
    const ::core::Router* router = ::core::appRouter();
    return ::core::menuAnchorForPointer(
        static_cast<float>(ev.x), static_cast<float>(ev.y),
        router ? router->pageOriginX() : 0.0f, scrollOffsetY, 1.0f);
}
```

`openFileContextMenu`（:3465-3470）改为：

```cpp
    void openFileContextMenu(const std::string& relPath, const eui::PointerEvent& ev) {
        ctxMenuPath_ = relPath;
        if (relPath.empty()) {  // 4523e09：空路径早退前直接清状态（不回回调）
            ctxMenuOpen_ = false;
            return;
        }
        const core::MenuAnchor anchor = menuAnchorForPointerInPage(ev);
        ctxMenuX_ = anchor.x;
        ctxMenuY_ = anchor.y;
        ctxMenuOpen_ = true;
    }
```

（若 4523e09 的空路径早退系该函数内其它形态，保持其现有实现不动，只换坐标两行。）`drawNoteContextMenu` 的 `.size(160.0f, 36.0f)`：M4-2 合入后改 `.autoWidth()` 并删 `160.0f`；未合入则保留写死宽（过渡期，spec §11——写死宽不再参与锚点即达本 Task 目标）。

- [ ] **Step 3: 跑预期通过 + commit**

```bash
bash scripts/build-test-run.sh --no-run --filter uiModern
bash scripts/build-test-run.sh --no-run --filter markdown
git add src/module_markdown/markdown_module.cpp tests/test_modules/test_ui_modern.cpp
git commit -m "fix(markdown): 树右键锚点统一换算 + 隐形卡回归断言（UI 现代化 M1-3）"
```

## Task M1-4: md 编辑器去 hit 挂靠 hack（依赖 M4-1）

**Files:**
- Modify: `src/module_markdown/markdown_module.cpp`
- Test: `tests/test_modules/test_ui_modern.cpp`

**Interfaces:**
- Consumes：`InputBuilder::onContextMenu`（M4-1）；`menuAnchorForPointerInPage`（M1-3）
- Produces：`drawEditorColumn` 内删除每帧挂靠块（`markdown_module.cpp:1111-1120` 区，`ui.find(kEditorInputHitId)` 挂靠）；`drawEditorContextMenu` 删 170 写死宽（M4-2 后 `.autoWidth()`）

- [ ] **Step 1: 写测试跑预期失败（M4-1 未合入则本步后移，先走过渡期 Step 0）**

`test_ui_modern.cpp` 加：

```cpp
REGISTER_TEST(uiModernEditorContextMenuAttached, "md: 编辑器 input 直接挂 onContextMenu（无逐帧 hack）", {
    // markdown compose 进入编辑态 → ui.find(kEditorInputHitId)->onContextMenu
    // 非空（input builder 链直挂；hack 删除后此断言锁「回调仍在」）。
    return true;
});
```

**Step 0（M4-1 未合入时的过渡期，spec §11）**：本 Task 只做坐标统一——把 hack 回调体内坐标改用 `menuAnchorForPointerInPage(ev)`（替换 :1116-1117 的 `windowToPageX` x / 裸 `ev.y` y），`.size(170.0f, 36.0f)` 保留，并在 commit message 注明「M4-1 未合入，hack 保留」。

- [ ] **Step 2: 实现（M4-1 合入后）**

删除 `drawEditorColumn` 内的挂靠块（「审计 [12]：编辑器右键 → 插入表格菜单…」注释到 `}` 的整个 `if (auto* hit = ui.find(kEditorInputHitId)) {...}`），在 input builder 链上直接挂（`.lineStyles(...)` 之后、`.build()` 之前）：

```cpp
                    // UI 现代化 §3.3 / M4-U1：右键直挂 input（上游透出），
                    // 删每帧 find 挂靠 hack；坐标走统一换算
                    .onContextMenu([this](const eui::PointerEvent& ev,
                                          const eui::Rect&) {
                        const core::MenuAnchor a =
                            menuAnchorForPointerInPage(ev);
                        editorCtxX_ = a.x;
                        editorCtxY_ = a.y;
                        editorCtxOpen_ = true;
                    })
```

`drawEditorContextMenu`（:3536）：`.size(170.0f, 36.0f)` → M4-2 后删（`.autoWidth()` 由组件默认触发于本菜单）；未合入则保留（过渡期）。`windowToPageX` 若 `openFileContextMenu` 与 hack 都改用新函数后无其它调用者，一并删除（编译器 unused 会确认）。

- [ ] **Step 3: 跑预期通过 + commit**

```bash
bash scripts/build-test-run.sh --no-run --filter uiModern
bash scripts/build-test-run.sh --no-run --filter markdown
git add src/module_markdown/markdown_module.cpp tests/test_modules/test_ui_modern.cpp
git commit -m "fix(markdown): 编辑器右键去 hit 挂靠 hack，改 input.onContextMenu 直挂（UI 现代化 M1-4）"
```

- [ ] **Step 4: M4-2 合入后的宽度收尾**

M1-2/3/4/5 的写死宽（190/160/170 及各调用点 `autoWidth` 过渡态）统一在 M4-2 合入后按各 Task 的过渡期说明逐个删（每删一处跑对应模块过滤串），可合并为一次小 commit（消息 `refactor(ui): 右键菜单宽度自适应收尾（M4-2 后删写死宽）`），显式 pathspec。

## Task M1-5: claude profile 列表补右键（编辑/复制/删除/设为当前）

**Files:**
- Modify: `src/module_claude_proxy/claude_ui_list.cpp`
- Modify: `src/module_claude_proxy/claude_ui_state.h`
- Modify: `src/module_claude_proxy/claude_module.cpp`
- Test: `tests/test_modules/test_ui_modern.cpp`

**Interfaces:**
- Consumes：`core::menuAnchorForPointer`（M1-1）；`ProfileListActions` 既有动作（编辑/复制/删除/设为当前——对应按钮回调同一函数，**不复写业务逻辑**）
- Produces：`drawProfileContextMenu(ui, screen, t, state, actions)` 新函数；状态 `int64_t ctxProfileId_` + `bool ctxMenuOpen_` + `float ctxMenuX_/ctxMenuY_`

- [ ] **Step 1: 写测试跑预期失败**

`test_ui_modern.cpp` 加（照 `test_claude_ui.cpp` 既有 compose harness + `click(ui, id)` helper 模式；驱动方式：列表中卡片元素上触发 onContextMenu）：

```cpp
REGISTER_TEST(uiModernClaudeProfileCtxMenu, "claude: 列表右键菜单四项（编辑/复制/删除/设为当前）", {
    // 既有 claude compose harness：两个 profile → 模拟卡片 onContextMenu →
    // 断言 "claude.list.ctx" 菜单存在、items 数 = 4、选择「设为当前」后
    // 调用的动作与既有按钮同源（activatedId 被写入，profile 激活位变化）。
    return true;
});
```

预期失败：菜单 id 不存在（未实现）。

- [ ] **Step 2: 实现**

`claude_ui_state.h` 状态区加：

```cpp
    int64_t ctxProfileId_ = 0;    // 右键目标 profile（0 = 无）
    bool ctxMenuOpen_ = false;    // 列表右键菜单开合
    float ctxMenuX_ = 0.0f;       // 锚点（页面坐标，经统一换算）
    float ctxMenuY_ = 0.0f;
```

`drawProfileCard` 或行容器（视既有交互元素挂点）追加 onContextMenu（在 `claude_ui_list.cpp` 的行回调里）：

```cpp
        inner.stack(cardId + ".wrap")
            .size(contentWidth, 96.0f)
            .content([&] {
                drawProfileCard(inner, cardId, 0.0f, contentWidth, t,
                                profiles[i], st);
            })
            .onContextMenu([this, pid = profiles[i].id, &ui](
                               const eui::PointerEvent& ev, const eui::Rect&) {
                st.ctxProfileId_ = pid;
                const core::MenuAnchor a = core::menuAnchorForPointer(
                    static_cast<float>(ev.x), static_cast<float>(ev.y),
                    ::core::appRouter() ? ::core::appRouter()->pageOriginX() : 0.0f,
                    0.0f, 1.0f);
                st.ctxMenuX_ = a.x;
                st.ctxMenuY_ = a.y;
                st.ctxMenuOpen_ = true;
            })
            .build();
```

（`st` 为 `ProfileListState&`；若该处 lambda 无法直接捕获，按既有 `[sp = &st]` 捕获形态改写。）新增 `drawProfileContextMenu`（放 `drawProfileListPanel` 之后，形态照 todo `drawContextMenu`）：

```cpp
// profile 列表右键菜单（UI 现代化 §3.4 / M1-5）：
// 编辑 / 复制 / 删除 / 设为当前 —— 动作复用 claude_module 既有按钮
// 回调同源逻辑（打开编辑表单 / duplicateProfile / 删除确认 / 激活+代理重载）
void drawProfileContextMenu(eui::Ui& ui, const eui::Screen& screen,
                            const core::theme::Tokens& t,
                            const components::theme::ThemeColorTokens& themeTokens,
                            ProfileListState& st,
                            const std::vector<Profile>& profiles,
                            const ProfileListActions& actions) {
    if (st.ctxProfileId_ == 0) return;
    const Profile* self = nullptr;
    for (const Profile& p : profiles) {
        if (p.id == st.ctxProfileId_) { self = &p; break; }
    }
    if (self == nullptr) { st.ctxMenuOpen_ = false; return; }
    const std::vector<components::ContextMenuItem> items = {
        {"编辑"}, {"复制"}, {"删除"}, {"设为当前"}};
    components::contextMenu(ui, "claude.list.ctx")
        .open(st.ctxMenuOpen_)
        .screen(screen.width, screen.height)
        .position(st.ctxMenuX_, st.ctxMenuY_)
        .autoWidth()                 // M4-2 后；过渡期保留下限常量
        .items(items)
        .theme(themeTokens)
        .onSelect([&](int index) {
            st.ctxMenuOpen_ = false;
            // 与既有按钮同源的动作分发（具体函数名以 claude_module.cpp
            // 既有按钮回调为准，不得新写逻辑）
            switch (index) {
                case 0: actions.requestEdit(*self); break;
                case 1: actions.requestDuplicate(*self); break;
                case 2: actions.requestDelete(*self); break;
                case 3: actions.requestSetActive(*self); break;
                default: break;
            }
        })
        .onOpenChange([&st](bool v) { st.ctxMenuOpen_ = v; })
        .build();
}
```

`claude_module.cpp`：compose 末尾调用 `drawProfileContextMenu(...)`（常驻构建——关闭态由组件内 opacity 收起，spec §4 同款模式）；`ProfileListActions` 若现无这四个动词封装，**照既有按钮回调拆最小委托**（不新写业务逻辑、不改原回调）。

- [ ] **Step 3: 跑预期通过 + commit**

```bash
bash scripts/build-test-run.sh --no-run --filter uiModern
bash scripts/build-test-run.sh --no-run --filter claude
git add src/module_claude_proxy/claude_ui_list.cpp src/module_claude_proxy/claude_ui_state.h \
        src/module_claude_proxy/claude_module.cpp tests/test_modules/test_ui_modern.cpp
git commit -m "feat(claude): profile 列表右键菜单（编辑/复制/删除/设为当前，UI 现代化 M1-5）"
```

---

## Task M2-1: md 视图三态切换过渡

**Files:**
- Modify: `src/module_markdown/markdown_module.cpp`
- Test: 视觉类，无头断言过渡属性存在

**Interfaces:**
- Consumes：既有 `mode_` / `ViewMode` / segmented「md.mode」回调（:453 区）
- Produces：状态 `bool viewFadeIn_` + `beginViewTransition()`

- [ ] **Step 1: 实现（先落代码，视觉验证进人工点验）**

模块状态区加：

```cpp
    bool viewFadeIn_ = true;   // 三态切换过渡（M2-1）：false 触发 root 淡入
    void beginViewTransition() { viewFadeIn_ = false; }
```

`ui.column("md.root")`（:348-356）链上补过渡（compose 每帧目标一致时不动画，仅模式切换触发）：

```cpp
        ui.column("md.root")
            .size(screen.width, screen.height)
            .padding(pad)
            .gap(t.sizes.spacingLarge)
            .opacity(viewFadeIn_ ? 1.0f : 0.0f)
            .translateY(viewFadeIn_ ? 0.0f : 6.0f)
            .transition(core::Transition::make(0.15f, core::Ease::OutCubic))
            .animate(core::AnimProperty::Opacity | core::AnimProperty::Transform)
            .content([&] {
                drawHeader(ui, contentW, contentH, bodyH, t);
                drawBody(ui, contentW, bodyH, sideW, midW, gap, t, componentTheme);
            })
            .build();
```

root content 内追加复位计时器（0.15s 后把目标值拨回 1 触发淡入动画）：

```cpp
        if (!viewFadeIn_) {
            ui.stack("md.root.fadein")
                .size(0.0f, 0.0f)
                .onTimer(0.15f, [this] { viewFadeIn_ = true; })
                .build();
        }
```

segmented「md.mode」的 onChange 回调（现设置 `mode_` + 持久化的位置）追加：

```cpp
        beginViewTransition();
```

- [ ] **Step 2: 无头断言 + 定向测试 + commit**

`test_ui_modern.cpp` 加 `uiModernMdViewTransition`：编辑态 compose → 断言 `ui.find("md.root")` 的 transition/opacity 属性存在且 epsilon 断言 `beginViewTransition()` 后首帧 opacity < 0.1f（照既有 headless 断言形态）。跑 `--filter uiModern` + `--filter markdown` 后：

```bash
git add src/module_markdown/markdown_module.cpp tests/test_modules/test_ui_modern.cpp
git commit -m "feat(markdown): 视图三态切换过渡（0.15s OutCubic，UI 现代化 M2-1）"
```

## Task M2-2: dialog/toast 常驻构建 + opacity 收起

**Files:**
- Modify: `src/module_todo/todo_ui_dialogs.cpp`（各 `draw*Dialog`）
- Modify: `src/module_markdown/markdown_module.cpp`（`drawNewNoteDialog`/`drawNewFolderDialog`/`drawDeleteDialog`/`drawRenameDialog`/`drawToast`；`drawEditorContextMenu` 懒守卫 :3531 去懒构建对齐树菜单常驻）
- Modify: `src/module_claude_proxy/claude_ui_*.cpp`（弹窗容器）
- Test: 无头断言关闭态元素仍构建 + opacity 0

**Interfaces:**
- Produces：全弹窗统一「常驻构建 + `open` 标志 opacity 收起」形态；关闭态 `.disabled(!open)` 防误触

- [ ] **Step 1: 盘点（先做，不重复加）**

grep 两件事：① 各模块 `draw*Dialog`/`drawToast` 的懒构建守卫（`if (!xxx) return;` 形态）清单；② `../EUI-NEO/components/dialog.h` / `toast.h` 是否已内建过渡（`transition`/`animate` 出现次数）。已内建的容器跳过并记录；未内建的列入 Step 2。

- [ ] **Step 2: 逐容器改造（照时间轴同款，`timepicker.h:83-118` 与 `renderHiddenLevel` 为参照）**

每个未内建过渡的容器：删懒守卫、改常驻构建，根 stack 链加（`open` = 该容器既有的开合标志，如 `d.popups.isOpen(Popup::X)` / `ctxMenuOpen_` / toast 可见标志）：

```cpp
        // M2（2026-09-28）：弹窗常驻构建 + opacity 收起——框架无「已删元素
        // 回放动画」机制，收起动画必须保留元素构建，由 open 标志驱动
        // （照 contextMenu renderHiddenLevel / timepicker 同款；关闭态
        // disabled 防误触，视觉归 0 不拦事件）
        const float visible = open ? 1.0f : 0.0f;
        ui.stack(id)
            .size(w, h)
            .zIndex(zi)                      // 各容器既有 zIndex 值保留
            .opacity(visible)
            .translateY(open ? 0.0f : 8.0f)
            .scale(open ? 1.0f : 0.98f)
            .transformOrigin(0.5f, 0.5f)
            .transition(core::Transition::make(0.15f, core::Ease::OutCubic))
            .animate(core::AnimProperty::Opacity | core::AnimProperty::Transform)
            .disabled(!open)
            .content([&] { /* 既有内容原样 */ })
            .build();
```

（`w/h` 用各容器现值；`translateY/scale` 数值与既有菜单收起位形一致即可，观感人工点验。）`drawEditorContextMenu`（:3531 `if (!editorCtxOpen_) return;`）删懒守卫改常驻——对齐树菜单（:3477 注释的常驻模式）。

- [ ] **Step 3: 无头断言 + 定向测试 + commit**

`test_ui_modern.cpp` 加 `uiModernDialogClosedKept`：任一改造容器（如 todo 新建任务对话）关闭态 compose → `ui.find(<容器 id>)` **非空** 且 opacity == 0（元素仍构建）。跑 `--filter uiModern` + 各模块过滤串：

```bash
bash scripts/build-test-run.sh --no-run --filter todo
bash scripts/build-test-run.sh --no-run --filter markdown
bash scripts/build-test-run.sh --no-run --filter claude
git add src/module_todo/todo_ui_dialogs.cpp src/module_markdown/markdown_module.cpp \
        src/module_claude_proxy/claude_ui_list.cpp src/module_claude_proxy/claude_ui_dialogs.cpp 2>/dev/null || true
git add <按盘点结果的实际文件清单> tests/test_modules/test_ui_modern.cpp
git commit -m "feat(ui): 弹窗/toast 常驻构建 + opacity 收起过渡（0.15s OutCubic，UI 现代化 M2-2）"
```

## Task M2-3: todo 卡 drop 落位过渡

**Files:**
- Modify: `src/module_todo/todo_ui_board.cpp`
- Test: `tests/test_modules/test_ui_modern.cpp`（拖拽期间不动画断言）

**Interfaces:**
- Consumes：既有拖拽 onDrag/onDragEnd 通道（`drawBoard`/`drawColumnContent` 卡根元素事件挂点，**不动业务判定**）
- Produces：状态 `bool columnFadeIn_`（列内容容器淡入标志，onDragEnd 置位 + 0.12s onTimer 复位）

- [ ] **Step 1: 实现**

状态区加 `bool columnFadeIn_ = true;`。`onDragEnd` 回调（落位完成处，既有返回值/判定逻辑**原样保留**）追加：

```cpp
        columnFadeIn_ = false;   // M2-3：落位淡入（跟手期间不触发）
```

列内容容器链（`drawColumnContent` :250 区每列内容容器）加：

```cpp
            .opacity(columnFadeIn_ ? 1.0f : 0.6f)
            .transition(core::Transition::make(0.12f, core::Ease::OutQuad))
            .animate(core::AnimProperty::Opacity)
```

容器 content 内追加复位计时器 `if (!columnFadeIn_) { ui.stack(...).size(0,0).onTimer(0.12f, [this]{ columnFadeIn_ = true; }).build(); }`。**约束**：onDrag 跟手回调内不得触碰 `columnFadeIn_`（跟手期间 transform 直跟随，动画不打架——实施时验证 onDrag 通道不冲突，冲突则该容器包判定跳过拖拽帧）。

- [ ] **Step 2: 无头断言 + 定向测试 + commit**

`uiModernTodoDropFadeGuard`：compose 看板 → 模拟拖拽进行中（拖拽状态位真）→ 断言列容器 transition 不生效（opacity 恒 1）；onDragEnd 后下一帧断言 opacity < 1（照既有 todo harness 驱动形态，断言以元素属性为准）。跑 `--filter uiModern` + `--filter todo` 后：

```bash
git add src/module_todo/todo_ui_board.cpp tests/test_modules/test_ui_modern.cpp
git commit -m "feat(todo): 卡片落位淡入过渡（拖拽跟手期间不动画，UI 现代化 M2-3）"
```

---

## Task M3-1: Home 统计卡 → card + lineChart sparkline + barChart

**Files:**
- Modify: `src/module_home/home_module.cpp`
- Modify: `src/core/proxy_stats.h` / `.cpp`（速率采样环，纯数据扩展）
- Modify: `src/module_home/home_service.h` / `.cpp`（`getTodayApiCallSeries`，纯数据扩展）
- Test: `tests/test_modules/test_ui_modern.cpp` + `tests/test_core/test_proxy_stats.cpp`（若存在，否则并入新文件）

**Interfaces:**
- Consumes：`core::proxy_stats::snapshot()`（既有）；`components::card` / `components::lineChart` / `components::barChart`
- Produces：`void proxy_stats::noteRateSample(double rateTps)`（writer 侧 2s 节奏采样）与 `std::vector<float> proxy_stats::rateSeries(int maxPoints)`（空 → `{0.0f}`）；`HomeService::getTodayApiCallSeries(int hours)` → `std::vector<int64_t>`（24 点当日逐时分布，纯新增查询不动既有字段）；`drawStatCard` 尾参扩展

- [ ] **Step 1: 数据扩展（先测后写）**

`proxy_stats.h/.cpp`：writer 路径（`updateStream` 事件点）按约 2s 节奏向环形缓存写入 `rateTps`，新增：

```cpp
// UI 现代化 D-6（2026-09-28）：最近速率采样序列（sparkline 数据源）。
// 纯数据扩展——既有字段/口径不动；空序列返回 {0.0f}（防 lineChart
// demo 数据泄漏，linechart.h:83-85 对空 values 会填演示数据）。
std::vector<float> rateSeries(int maxPoints = 32);
```

单测：写 N 点 → 序列裁剪到 maxPoints、末点 == 最近采样；空 → `{0.0f}`；并发安全沿用既有互斥口径。

`home_service.h/.cpp`：`getTodayApiCallSeries`（`claude_call_logs` 当日按小时 GROUP BY，24 点、无值补 0）——照既有 `countBySql` 风格；单测补：数据落库后 series 点与行数对齐（照 `tests/test_home.cpp` 既有 harness）。

`tests/test_ui_modern.cpp` 加 `uiModernSparkSeriesGuard`（空 → `{0.0f}`）与 `uiModernProxyRateRing`（裁剪与末点语义）。先跑预期失败 → 实现 → 跑通过（`--filter uiModern` + `--filter home`）。

- [ ] **Step 2: `drawStatCard` 换 card 组件（保留业务面）**

`home_module.cpp` `drawStatCard`（:33-105）整体重写为 card（**必须保留**：参数面与调用点不动、`accent`/`numberColor`/`badgeColor` 语义色、`timerSeconds > 0` 的 0.5s 重绘驱动；`onClick` 语义保留）：

```cpp
// M3（2026-09-28）：手绘卡换 card 组件。保留：accent/numberColor/badgeColor
// 语义色（非空覆盖「accent ? error : 常规」配色）与 timerSeconds 重绘驱动。
// card 组件无 hover states（CardStyle 无 states 字段）→ hover 反馈由卡内
// 交互层 .press 补（rect states 三态 + 0.1s OutQuad 过渡）；
// .press 位 content 首层——card 的 bg 不可交互，非交互文本点击会穿透命中
// .press（与旧 .bg onClick 同命中语义）。
void drawStatCard(eui::Ui& ui, const std::string& id, float width, float height,
                  const core::theme::Tokens& t, const std::string& label,
                  const std::string& number, const std::string& badge,
                  bool accent, std::function<void()> onClick,
                  const eui::Color* numberColor = nullptr,
                  const eui::Color* badgeColor = nullptr,
                  float timerSeconds = 0.0f,
                  std::function<void(eui::Ui&, float, float)> chart = nullptr) {
    const auto ct = core::theme::componentTokens(t);
    const float s = core::theme::fontScale();
    const float inset = ct.metrics.spacing.large;   // card 默认 content inset
    const float cw = width - inset * 2.0f;
    const float ch = height - inset * 2.0f;
    components::card(ui, id)
        .size(width, height)
        .radius(t.sizes.radiusLarge)
        .theme(ct)
        .content([&] {
            ui.stack(id + ".inner")
                .size(cw, ch)
                .content([&] {
                    // 交互层（点击 + hover 过渡；非交互文本穿透命中）
                    ui.rect(id + ".press")
                        .size(cw, ch)
                        .states(theme::color(0.0f, 0.0f, 0.0f, 0.0f),
                                t.colors.hover, t.colors.active)
                        .transition(core::Transition::make(0.1f,
                                                           core::Ease::OutQuad))
                        .animate(core::AnimProperty::Color)
                        .onClick(std::move(onClick))
                        .build();
                    // 卡内文字（坐标与旧实现一致：旧根 stack 无 inset，
                    // .inner 已对齐原点）
                    ui.text(id + ".label")
                        .position(28.0f * s, 20.0f * s)
                        .size(cw - 56.0f * s, 16.0f * s)
                        .text(label).fontSize(t.sizes.fontSizeSmall - 1.0f)
                        .lineHeight(16.0f * s).color(t.colors.textSecondary)
                        .build();
                    ui.text(id + ".number")
                        .position(28.0f * s, 42.0f * s)
                        .size(cw - 56.0f * s, 56.0f * s)
                        .text(number).fontSize(t.sizes.fontSizeDisplay)
                        .lineHeight(56.0f * s).fontWeight(700)
                        .color(numberColor ? *numberColor
                                           : (accent ? t.colors.error
                                                     : t.colors.textPrimary))
                        .build();
                    ui.text(id + ".badge")
                        .position(28.0f * s, 98.0f * s)
                        .size(cw - 56.0f * s, 16.0f)
                        .text(badge).fontSize(t.sizes.fontSizeSmall - 2.0f)
                        .lineHeight(16.0f)
                        .color(badgeColor ? *badgeColor
                                          : (accent ? t.colors.error
                                                    : t.colors.textTertiary))
                        .build();
                    if (timerSeconds > 0.0f) {
                        // 保留：0.5s 周期整卡重绘（流式速率实时刷新）
                        ui.stack(id + ".timer")
                            .size(0.0f, 0.0f)
                            .onTimer(timerSeconds, [] {})
                            .build();
                    }
                    if (chart) {
                        chart(ui, cw - 56.0f * s, 32.0f);
                    }
                })
                .build();
        })
        .build();
}
```

（`theme::color(...)` 透明色构造以 `components::theme` 域实际 API 为准；`.press` 的 hover 色 `t.colors.hover` 与旧 `.hoverColor` 同 token。卡内 y 布局若与旧观感有 ±inset 级位移，以真机点验为准微调——坐标语义进人工点验清单。）

- [ ] **Step 3: 调用点接线（Token sparkline + API barChart）**

Token 速率卡调用点（:331-361 区）追加尾参：

```cpp
                        drawStatCard(ui, "home.stat.tokens", statW, statH, t,
                                     "Token 速率 (tok/s)",
                                     formatRateTps(snap.rateTps), tokenBadge,
                                     false,
                                     [this] { navigateTo(::core::PageId::ClaudeProxy); },
                                     streaming ? &t.colors.primaryText
                                               : &t.colors.textTertiary,
                                     streaming ? &t.colors.success : nullptr,
                                     0.5f,
                                     [&](eui::Ui& inner, float chartW,
                                         float chartH) {
                                         // sparkline：rateSeries 兜底 {0.0f}
                                         // （防 lineChart demo 数据泄漏）
                                         const std::vector<float> series =
                                             core::proxy_stats::rateSeries(24);
                                         components::lineChart(
                                             inner, "home.stat.tokens.spark")
                                             .size(chartW, chartH)
                                             .values(series)
                                             .style(core::LineStyle::Curve)
                                             .theme(core::theme::componentTokens(t))
                                             .build();
                                     });
```

API 调用量卡（:331-361 区同）追加 barChart 尾参（labels 省略用组件默认；`getTodayApiCallSeries(24)` → 每点 `static_cast<float>`——barChart 实际接口以 `barchart.h` 为准，适配失败则以该文件现有接口最小包装，**不新建组件**）：

```cpp
                        drawStatCard(ui, "home.stat.api", statW, statH, t,
                                     "API 调用", num(stats_.todayApiCalls),
                                     ok ? "今日代理调用" : "加载失败",
                                     false,
                                     [this] { navigateTo(::core::PageId::ClaudeProxy); },
                                     nullptr, nullptr, 0.0f,
                                     [&](eui::Ui& inner, float chartW,
                                         float chartH) {
                                         barChartRender(inner, "home.stat.api.chart",
                                                        chartW, chartH, t);  // 见下
                                     });
```

`barChartRender` 为模块内静态 helper：取 `service_->getStats` 的当日序列（或 `stats_` 扩展字段），逐点喂 barChart；数据扩展见 Step 1。

`home.recent.bg`（:385 区）：手绘 rect 换 `components::card(ui, "home.recent")`（padding 与坐标对齐做法同 `drawStatCard` 的 `.inner` 技巧，标题/条目绘制原样进 content）。

- [ ] **Step 4: 无头断言 + 定向测试 + commit**

`uiModernHomeCardSpark`：home compose → `ui.find("home.stat.tokens")`（card）非空 → `ui.find("home.stat.tokens.spark")` 非空且其 values 与 `core::proxy_stats::rateSeries(24)` 非空对齐（接线断言）；`uiModernHomeRecentCard`：`ui.find("home.recent")` 为 card 组件节点（既有 harness 断言形态）。跑：

```bash
bash scripts/build-test-run.sh --no-run --filter uiModern
bash scripts/build-test-run.sh --no-run --filter home
bash scripts/build-test-run.sh --no-run --filter proxy_stats
git add src/module_home/home_module.cpp src/core/proxy_stats.h src/core/proxy_stats.cpp \
        src/module_home/home_service.h src/module_home/home_service.cpp \
        tests/test_modules/test_ui_modern.cpp <新增 proxy_stats 测试文件>
git commit -m "feat(home): 统计卡换 card + Token sparkline + API barChart（UI 现代化 M3-1）"
```

## Task M3-2: profile 列表 + 调用记录 → virtualList

**Files:**
- Modify: `src/module_claude_proxy/claude_ui_list.cpp`
- Modify: `src/module_claude_proxy/claude_ui_logs.cpp`
- Test: `tests/test_modules/test_ui_modern.cpp`

**Interfaces:**
- Consumes：`components::virtualList`（`virtuallist.h:205`；`itemCount/rowHeight/offset/onChange/row(RowCompose)`）；`drawProfileCard`（M1-5 后已带右键，行内保留）
- Produces：profile 列表行数不变式 + 滚动位置回退语义（`profilesScrollOffset_` 沿用）

- [ ] **Step 1: 写测试跑预期失败**

`test_ui_modern.cpp`：

```cpp
REGISTER_TEST(uiModernVirtualProfileRows,
              "claude: virtualList 行数 = profile 数 + 滚动回退", {
    // 既有 claude compose harness：N 个 profile → 断言列表根为 virtualList
    // 节点（含 .slot. 行重建语义）、可见行 <= N；滚动后（模拟 onScrollOffset
    // 写 profilesScrollOffset_）重 compose 断言 offset 回退到同一值。
    return true;
});

REGISTER_TEST(uiModernVirtualLogRows, "claude: 调用记录 virtualList 行数", {
    // 同 harness：allCalls_ 条数 -> 可见行数对应（slot 窗口语义）。
    return true;
});
```

预期失败：列表根非 virtualList 节点。

- [ ] **Step 2: 实现（`claude_ui_list.cpp`）**

`drawProfileListPanel`（:248）的 `components::scrollView(...)` 块（:306-357 区）整体换 `virtualList`（行回调 = 既有 for 循环体搬入，`drawProfileCard` 的 y 参数传 0；滚动语义 `profilesScrollOffset_` 沿用）：

```cpp
                components::virtualList(ui, "claude.profiles.list")
                    .position(0.0f, cardY0)
                    .size(leftW - 24.0f, scrollH)
                    .itemCount(static_cast<int64_t>(profiles.size()))
                    .rowHeight(96.0f)
                    .step(96.0f + cardGap)
                    .offset(st.profilesScrollOffset_)
                    .onChange([sp = &st](float v) {
                        sp->profilesScrollOffset_ = v;
                    })
                    .theme(themeTokens)
                    .row([&](eui::Ui& inner, const std::string& rowId,
                             int64_t index, float contentWidth,
                             float rowHeight) {
                        (void)rowHeight;
                        const auto idx = static_cast<size_t>(index);
                        if (idx >= profiles.size()) return;
                        // 行容器换 card（UI 现代化 §5.2）：
                        // 装饰层（avatar/active 徽标等）进 content；
                        // 交互层 .bg.hit 保留 states（hover/选中过渡 0.1s
                        // OutQuad）与事件挂点（onClick/onContextMenu）
                        components::card(inner, rowId + ".card")
                            .size(contentWidth, 96.0f)
                            .padding(0.0f)
                            .theme(themeTokens)
                            .content([&] {
                                drawProfileCard(inner, rowId, 0.0f,
                                                contentWidth, t,
                                                profiles[idx], st);
                            })
                            .build();
                    })
                    .build();
```

`drawProfileCard`（:20-117）：`ui.rect(id + ".bg")` 一段（:35-42）删除（骨架由 card 画），其 states/hover 语义由行内透明交互 rect 承接（`.bg.hit`：states + `transition(0.1s OutQuad)` + `animate(Color)` + onClick/onContextMenu 原挂点）；选中/激活的 border 条件换算到 card 的 `.border(...)`，其余装饰层原样。

- [ ] **Step 3: 实现（`claude_ui_logs.cpp`）**

调用记录列表（:266 scrollView + :280 for 平铺）同样换 `virtualList`（`itemCount = allCalls_.size()`、`rowHeight` = 该文件既有行高常量、行回调搬原绘制函数；滚动状态沿用现状态字段）。

- [ ] **Step 4: 定向测试 + commit**

```bash
bash scripts/build-test-run.sh --no-run --filter uiModern
bash scripts/build-test-run.sh --no-run --filter claude
git add src/module_claude_proxy/claude_ui_list.cpp src/module_claude_proxy/claude_ui_logs.cpp \
        tests/test_modules/test_ui_modern.cpp
git commit -m "feat(claude): profile 列表与调用记录换 virtualList + 行卡换 card（UI 现代化 M3-2）"
```

## Task M3-3: Todo 卡片 → card（拖拽保留）

**Files:**
- Modify: `src/module_todo/todo_ui_board.cpp`
- Test: `tests/test_modules/test_ui_modern.cpp`

**Interfaces:**
- Consumes：`components::card`（`card.h:130`）
- 红线：**拖拽业务逻辑原样保留**——onDrag/onPress 通道、落位判定、跨列移动、逾期/完成标记语义一字不动，只换容器绘制

- [ ] **Step 1: 写测试跑预期失败**

`uiModernTodoCardContainer`：todo compose 看板（含卡）→ 断言卡根节点为 card 组件结构（含 `.card` 节点）；拖拽判定函数（若有既有纯函数 harness）行为断言原封复跑（`--filter todo` 既有用例兜底即可）。

- [ ] **Step 2: 实现（容器绘制换 card）**

`drawColumnContent`（:250）内卡片容器绘制（bg rect + 边框 + hover 状态那一层）换 `components::card(ui, cardId + ".card")`：尺寸沿用卡几何（`kCardWidth/kCardHeight` 语义），`.theme(componentTokens)`、逾期红边/选中边框用 `.border(...)` 条件；卡内容（标题/截止/标签 chips 等）放进 `content`；**事件挂点（onDrag/onPress 等）移到卡内交互层**（透明 rect，模式同 M3-1 的 `.press`——命中语义与旧卡根相同）；拖拽跟手期间不动画（与 M2-3 的 `columnFadeIn_` 判定一致）。

- [ ] **Step 3: 定向测试 + commit**

```bash
bash scripts/build-test-run.sh --no-run --filter uiModern
bash scripts/build-test-run.sh --no-run --filter todo    # 拖拽/CRUD 既有用例全绿 = 业务未动
git add src/module_todo/todo_ui_board.cpp tests/test_modules/test_ui_modern.cpp
git commit -m "feat(todo): 卡片容器换 card 组件（拖拽逻辑保留，UI 现代化 M3-3）"
```

## Task M3-4: shadow/gradient 补齐

**Files:**
- Modify: `src/module_home/home_module.cpp`（Home 欢迎区）
- Modify: `src/module_settings/settings_module.cpp`（设置页主体区）
- Modify: `src/module_markdown/markdown_module.cpp`（md 预览区）
- Test: 无头断言 gradient/shadow 属性存在；观感人工点验

**Interfaces:**
- Consumes：`dsl` 的 `.gradient(Color, Color, GradientDirection)` / `.shadow(...)`（`core/dsl.h:867-912`）；方案 B token（**不新增色板**）
- Produces：三处关键背景渐变 + 既有换装卡片默认阴影随 card `CardStyle`（`card.h:19` panelShadow）自动到位

- [ ] **Step 1: 实现**

- Home 欢迎区：`home.header` 行内标题列垫底色 rect（“home.header.bg”），`gradient(from to)` 取方案 B 同色系低对比双色（候选：`t.colors.surface` / `t.colors.surfaceElevated` 或 `primarySoftBg` 系，方向 Vertical，观感取舍进人工点验）。
- 设置页主体区：主体容器 rect 加 `.gradient(...)`（同口径）。
- md 预览区：预览列背景 rect 加 `.gradient(...)`。
- 各 `card` 落点阴影随 `CardStyle` 默认 `theme::panelShadow`（M3-1/2/3 已cover，本 Task 复查 `grep -c "panelShadow"` 命中）。

- [ ] **Step 2: 无头断言 + commit**

`uiModernGradientSpots`：三模块 compose → 断言三处背景节点 gradient 非空（builder 属性断言形态照既有 harness）。跑 `--filter uiModern` + `home`/`settings`/`markdown` 过滤串后：

```bash
git add src/module_home/home_module.cpp src/module_settings/settings_module.cpp \
        src/module_markdown/markdown_module.cpp tests/test_modules/test_ui_modern.cpp
git commit -m "feat(ui): 关键背景补渐变 + card 阴影复查（UI 现代化 M3-4）"
```

---

## 验收清单（实施线与复核线都跑）

1. 定向用例全绿（各里程碑）：`--filter uiModern` / `markdown` / `todo` / `claude` / `home` / `proxy_stats` / `settings`，每跑一条记结果；**全量只在收尾由主会话跑一次** `bash scripts/build-test-run.sh`（含 check-module-isolation）。
2. 逐条核对 spec §10 里程碑清单：M4 三项 + bump；M1 五项（**4523e09 已落地的两项不在实现清单，回归断言在**）；M2 三项；M3 四项。
3. spec §2.1 计数复查（M3 收尾）：`grep -c "\.transition(\|\.animate(" src/` > 0、`grep -c "lineChart\|barChart\|virtualList" src/` > 0、`grep -c "\.shadow(\|\.gradient(" src/` > 0（三处计数与人工点验一致）。
4. 跨仓库：EUI-NEO 三个 commit 的 sha 记录在案；`最低依赖 rev 软校验（已删，ADR 0006）` 与 `CLAUDE.md`「当前依赖 rev」两处一致；非隔离线未碰 DevDesk 文件（`git -C ../EUI-NEO show --stat` 核对）。
5. 无残留：全仓右键调用点全部走 `menuAnchorForPointer`/`menuAnchorForPointerInPage`（`grep -n "ctxMenuY_\|editorCtxY_"` 无裸 `ev.y` 形态）；写死宽 190/160/170 删除（M4-2 后）。
6. `git status` 干净、每 commit 显式 pathspec、worktree 用完即清。

## 人工点验清单（视觉类 headless 测不了，必须真机）

按 `docs/superpowers/plans/2026-09-28-manual-verification-checklist.md` 的既有流程，各批交付物真机点验；重点项（spec §9）：

1. 三处右键：todo 滚动后右键（菜单贴光标、不跑位）、md 树深目录右键、编辑器空白/选中文字右键（无隐形卡、菜单不消失）；claude 列表右键四项动作各验证一次。
2. Home：统计卡新观感（语义色 accent 卡、badge）、Token sparkline 实时刷新（0.5s）、API barChart、recent 卡；hover 反馈。
3. Todo：卡 hover/落位过渡、拖拽跟手手感不变；对话框开合动画（收起动画可见 = 常驻构建生效）。
4. md 三态切换动画、编辑器右键不 Hand 指针回归（悬停 input 应为 Arrow）。
5. 阴影/渐变观感（各 card 阴影、三处渐变背景）。
6. macOS 27 侧：横跨两平台截图对比至少一次（EUI-NEO 上游改动在 mac 侧编译未在 Windows 验证）。

## 未验证项（实施后如实更新）

1. **onContextMenu 在 scrollView 内容内的坐标口径**：本计划依据 `runtime_input.h:416-447` 源码推断为 Logical 窗口坐标（视觉位）→ 换算函数 y 不用滚动偏移；若真机实测滚动内容内事件坐标为**内容坐标**（todo 滚动后右键跑位），则 todo/md 树按 `menuAnchorForPointer` 的 `scrollOffsetY` 参数补齐偏移（todo 传 `columnScrolls` 原值、md 树取 `md.tree.scroll` 偏移）并记入 commit。
2. **U3 运行期 resize 基建盘点结论**（core::window 有无现成通道；B 方案队列的后端窗口注册表路由）——M4-3 Step 1 的产出决定实现 A/B。
3. **EUI-NEO macOS/ObjC 编译**：非隔离线只在 Windows 侧验证；mac 侧列入人工点验。
4. **card 组件无 hover states**（`CardStyle` 无 states）——hover 由卡内交互层补，观感（透明色叠层色差）人工点验。
5. **lineChart / barChart 外观**（sparkline 高度 32px、无 labels、barChart 接口适配）——观感进人工点验；barChart 实际接口以 `barchart.h` 为准适配。
6. **charts 首期范围**（spec §12.3 待用户复核）：本计划默认首期 = Token lineChart + API barChart；**Todo 逾期分布不做**；用户拍板扩大/缩小则相应增删 M3-1 步骤。
7. **过渡时长手感**（0.15s OutCubic / 0.1s OutQuad，spec §12.4）：真机手感验收，不适配按用户反馈统一调参数（两处常量单点改）。
8. **M1-5 的 `ProfileListActions` 动作委托**：若现有动作封装与四个菜单项不完全对应，按既有按钮回调拆最小委托（不写新逻辑）——拆分点以实际 grep 到的按钮回调为准。