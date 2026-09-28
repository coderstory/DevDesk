# DevDesk UI 现代化设计（2026-09-28）

## 1. 需求与决策

用户原话（2026-09-28 反馈）：

> 「EUI 本身具备了很多能力了，你还是没用好，导致 UI 简陋，很多是自己写代码绘制的」
> 「最好全部场景都别自己写 UI，都用 EUI 的」
> 「右击菜单文本和背景错位分离、位置跑远、不同地方错误不一样」

**R1**：全仓 UI 改用 EUI 组件，停止手绘（手写 rect+text 出的"卡"、列表、图表一律换成 EUI 组件）。

决策（D-xx，全部为本次设计定案，待用户复核 §12）：

- **D-1 右键菜单统一**：三处调用点（todo 看板 / md 树 / md 编辑器）改成「只传锚点坐标——PointerEvent 原始坐标经统一换算函数转页面坐标」，位置与尺寸计算交给 `contextMenu` / `styledMenu` 组件自身（其内部已有 Y/X 钳制）；不再各处用板几何推算、不再写死宽度。
- **D-2 全局接入 `transition + animate`**：弹窗开合、视图切换、列表项 hover/选中态补过渡（消除"硬切"）。基调先统一 OutCubic（EUI 组件默认即 OutCubic：contextMenu/virtualList 0.12s，**lineChart 0.16s**——`EUI-NEO/components/contextmenu.h:328`、`virtuallist.h:186`、`linechart.h:426`（成员默认值 :421），与 §2.4 一致）。
- **D-3 手绘容器换 EUI**：Home 统计卡换 `card` + Token 速率加 `lineChart` sparkline；profile 列表 + 调用记录换 `virtualList`；Todo 卡片换 `card`（拖拽业务逻辑保留不动）。
- **D-4 上游贡献（EUI-NEO）**：`input` 透出 `onContextMenu`（md 编辑器不再每帧 hit 挂靠 hack）；`contextMenu` 补宽度自适应（按最长项 `measureTextWidth`，styledMenu 已有该能力可移植）；`styledMenu` 收起态高度回收（openWindow 加运行期 setter 或重测）。
- **D-5 shadow/gradient 接入**：卡片/弹窗补 EUI 阴影（`shadow` token），关键背景补渐变（消除"平面感"）。
- **D-6 charts 接入**：Home 仪表盘 Token 速率 `lineChart`、API 调用量 `barChart`、Todo 逾期分布 —— 数据现成（`core/proxy_stats.h` 的 `snapshot()` + `HomeService::getStats` + `core::overdue::kWhereSql`）。
- **D-7 不做**：不为美化而美化——loader/tiltCard/heartSwitch 等 workshop 组件按需收敛、不铺开；不改任何业务逻辑（拖拽、CRUD、代理流转、设置持久化均不动）。

## 2. 现状事实

以下全部经 grep / codegraph 实测（2026-09-28）：

### 2.1 全仓能力用量计数（grep `DevDesk/src`）

| 能力 | 计数 | 结论 |
|------|------|------|
| `.transition(` / `.animate(` | **0** | 全仓零过渡，所有 UI 切换均为"硬切" |
| `.shadow(` / `.gradient(` | **0** | 零阴影、零渐变，平面感来源 |
| `lineChart` / `barChart` / `dataTable` / `virtualList` | **0** | 零图表、零虚拟列表 |

EUI-NEO 侧 easing 共 8 种：`Linear/InQuad/OutQuad/InOutQuad/OutCubic/InOutCubic/OutExpo/OutBack`（`EUI-NEO/core/animation.h:12-21`）。

### 2.2 右键菜单三处现状与 bug 机制（三套机制并存，坐标口径各不相同）

| 调用点 | 机制 | 现状 | bug 机制 |
|--------|------|------|----------|
| todo 看板 `todo_ui_dialogs.cpp:236-246` | in-page `components::contextMenu`（"todo.ctx"） | 宽度写死 `menuW = 190.0f`；x = `cardLeft(g, colIndex) + g.colW*0.5 - menuW*0.5`；y = `cardTop(g, cardIndex) + kCardHeight + 4`（**⚠️ 已由 4523e09 即时修复**：`drawContextMenu` 新增 `columnScrolls` 形参（`todo_dialogs.h` / `todo_module.cpp:662` 同步传参），`cardIndex >= 0` 时 y 再减 `columnScrollOffset`，滚动后菜单贴住卡片实际位置；列头菜单在滚动区外不受影响——M1 基于新签名实施） | **位置跑远（历史 bug 机制）**：`cardTop` 按**板几何**推算、不含列滚动偏移（columnScrollOffset 不参与），滚动后菜单钉在旧卡片位置——**⚠️ 已由 4523e09 即时修复，M1 不再实现**；宽度写死不自适应（仍未修，随 M1/§3.5） |
| md 树 `markdown_module.cpp:3465-3470, 3472-3498` | in-page `components::contextMenu`（"md.ctx"） | `openFileContextMenu`：x 经 `windowToPageX`（:3467），y **裸用** `static_cast<float>(ev.y)`（:3468，注释称"y 方向原点恒 0 不换算"——页面内含 header 的场景即错位）；菜单宽度写死 160 | **隐形卡（历史 bug 机制）**：`drawNoteContextMenu` 空路径早退 `return` 但**不关 `ctxMenuOpen_`**（open 保持 true）→ 覆盖层处于"已开但无菜单"的隐形模态态，点哪都无响应——**⚠️ 已由 4523e09 即时修复**（空路径早退前直接赋值 `ctxMenuOpen_ = false` 再 return，`markdown_module.cpp:3478-3481`，不走 `onOpenChange` 回调），**M1 不再实现该修复**；坐标裸 y 换算问题仍在，随 M1 |
| md 编辑器 `markdown_module.cpp:1111-1118, 3520-3535` | in-page `components::contextMenu`（"md.editor.ctx"，「插入表格」级联） | 每帧 `ui.find(kEditorInputHitId)` 把 `hit->onContextMenu` **挂靠到 input 元素上**（hack：EUI `input` 不透出 `onContextMenu`）；宽 170 写死 | 挂靠 hack：依赖每帧重建重新挂、`find` 失败则菜单静默消失；坐标 x 经换算、y 裸用（同树行；`:1116` x / `:1117` y） |
| claude profile 列表 `claude_ui_list.cpp` | **无右键** | 全仓 `onContextMenu` **文本命中 11 处（含注释）、真实绑定 5 处**：md ×2（`markdown_module.cpp:119` 树行、`:1113` 编辑器 input 挂靠）+ pet ×1（`pet_window.cpp:422`）+ **todo ×2（`todo_ui_board.cpp:283` 列头菜单 / `:406` 卡片菜单）**——旧计数"markdown ×4、pet ×1"漏了 todo 两个入口；claude 模块 0 处 | 列表无右键入口（编辑/复制/删除/profile 只能走按钮） |

另有独立窗 `styledMenu` 机制：`pet_window.cpp:229`（退出确认）、`pet_window.cpp:316`（桌宠菜单）、`main.cpp:400`（托盘菜单）——`showStyledMenu`（`EUI-NEO/components/styledmenu.h:351`）内部已做 `styledMenuWindowSize`（:259）+ `styledMenuClampPosition`（:277）+ `measureTextWidth`。以及原生降级 `core::platform::showContextMenu`（`context_menu_win.cpp` / `_stub.cpp`，桌宠小窗专用）。

**用户所见三处错误不一样**的根因即在此：三套机制、三套坐标口径、两处裸 y、一处板几何。

### 2.3 手绘清单（本应使用 EUI 组件）

| 位置 | 现状 | 应替换 |
|------|------|--------|
| Home 统计卡 `home_module.cpp:33-45 drawStatCard`（label/number/badge/accent/timerSeconds 手绘卡），调用点 :331-361；最近编辑区 `home.recent.bg` :385 等手绘 rect | 手绘 rect+text | `card` + 每卡内 text；Token 速率处加 `lineChart` sparkline |
| Home Token 数据：`core/proxy_stats.h/.cpp`（`snapshot()`，`home_module.cpp:300-301` 已取用；`streaming_proxy.cpp:226` 注入） | 数据现成 | charts 直接消费 |
| profile 列表 `claude_ui_list.cpp:20-45 drawProfileCard`（`.bg`/`.avatar`/`.active.bg` 手绘）+ `:248-357 drawProfileListPanel`（for 循环平铺 + scrollView） | 手绘卡 + 手写平铺 | `virtualList`（builder 提供 itemCount/rowHeight/滚动回收，`virtuallist.h:205`） |
| Todo 卡片 `todo_ui_board.cpp:161 drawBoard` / `:250 drawColumnContent`（文件头注释自述"卡片绘制只在文件内部"） | 手绘列/卡 | `card`（拖拽 onDrag 保留） |

### 2.4 EUI 组件能力（codegraph 核实声明位置）

| 组件 | 声明 | 关键能力 |
|------|------|----------|
| `contextMenu` | `EUI-NEO/components/contextmenu.h`（Builder :73，open/position/size/items/onSelect/onSelectPath/onOpenChange/transition） | 自动高度（`menuHeight` :55）、Y 钳（`clampMenuY` :59，min 8px 边距）、X 钳（build() 内 :121）、级联子菜单（`childMenuX` :63 + cascade 状态）、开合过渡（默认 0.12s OutCubic :328）、常驻构建关闭态由 opacity/disabled 收起 |
| `styledMenu` | `EUI-NEO/components/styledmenu.h:351` | 独立窗；`styledMenuWindowSize` :259（按行数算窗高）、`styledMenuClampPosition` :277（显示器工作区钳制）、`measureTextWidth`（`EUI-NEO/core/render/text.cpp:2197`）宽度自适应已内置 |
| `card` | `EUI-NEO/components/card.h`（CardBuilder :32） | bg = color/gradient/radius/border/shadow/opacity 一体；content 内嵌 inset 布局；`wrapContentHeight` 自适应 |
| `lineChart` | `EUI-NEO/components/linechart.h:426` | size/title/values/labels/LineStyle/transition；默认 0.16s OutCubic |
| `barChart` | `EUI-NEO/components/barchart.h` | 存在（size/title/value 类接口），供 D-6 使用 |
| `virtualList` | `EUI-NEO/components/virtuallist.h:205` | itemCount/rowHeight/滚动回收/scrollbarGap；默认 0.12s OutCubic 过渡 |
| `transition+animate` | `EUI-NEO/core/animation.h:12-21`（Ease 8 值）+ DSL `.transition()/.animate()` | 位掩码属性动画（Opacity/Transform 等） |

## 3. 右键菜单统一（D-1）

### §3.1 统一坐标换算函数（新增）

**目标**：三处调用点不再各自算坐标，只传「PointerEvent 原始坐标」，由统一函数完成"窗口坐标 → 页面坐标 →（如需）组件坐标"全链换算，**含 Y、含 scrollView 偏移**。

- 新增函数（落点 `core/` 或 `components/` 层，实施时定；命名示例 `menuAnchorForPointer(const eui::PointerEvent&, float pageOriginX, float scrollOffsetY) → core::Point`）：
  1. 全链基准复用现有 `core::windowToCompose`（`markdown_module.cpp:248-252` 的 `windowToPageX` 即其薄包装，传 `Router::pageOriginX()` + dpiScale）——**推广到 Y**：`windowToCompose(ev.y, pageOriginY, dpiScale)`；
  2. todo 场景另外叠加列滚动偏移（columnScrollOffset）——锚点落在卡片视觉位置而非几何位置；
  3. 返回后只喂 `contextMenu.position(x, y)`（或 styledMenu 锚点），其余（宽度、Y/X 钳制、级联）交给组件。
- 三处调用点删除各自的裸 y / 板几何口径，统一调该函数。EUI 侧 `contextMenu` 已有 `clampMenuY`/X 钳（`contextmenu.h:59`/:121）作兜底，换算函数只需保证"算对"，不用重复做屏幕钳制。

### §3.2 Todo 看板（`todo_ui_dialogs.cpp:236-246`）

- 删除 `menuW = 190.0f` 写死与 `cardLeft/cardTop` 板几何推算；
- 改用 PointerEvent 原始坐标（或换算函数叠加 `columnScrollOffset`）；
- 宽度交组件（§3.5 上游补自适应后再删写死值；上游未到位前过渡期保留 190 常量但**不再参与锚点计算**——锚点用指针坐标，宽度只影响菜单自身绘制）。

### §3.3 md 树 + md 编辑器（`markdown_module.cpp`）

- 树（:3465-3470）：`ctxMenuY_` 改走统一换算（含 Y，不再裸 `ev.y`）；
- **隐形卡修复 ✅ 已由 4523e09 落地**（`markdown_module.cpp:3478-3481`）：空路径早退前**直接赋值** `ctxMenuOpen_ = false` 再 return（不触发 `onOpenChange` 回调）——M1 不再实现该修复，回归断言见 §9；
- 编辑器（:1111-1118）：上游 `input` 透出 `onContextMenu` 后（M4），删除每帧 `ui.find(kEditorInputHitId)` 挂靠 hack，改在 input builder 链上直接 `.onContextMenu(...)`；坐标同样走统一换算。

### §3.4 claude profile 列表补右键（`claude_ui_list.cpp`）

- 在 `drawProfileCard`/`drawProfileListPanel` 的卡片元素上挂 `onContextMenu`，菜单项：编辑 / 复制 / 删除 / 设为当前（对应现有按钮逻辑，`claude_ui_logic` 既有动作直接复用，不改业务逻辑）；
- 锚点走 §3.1 统一换算；列表滚动场景下换算函数已含滚动偏移 → 右键不会跑位。

### §3.5 宽度自适应（上游项，M4）

- `contextMenu`（`contextmenu.h`）补 `measureTextWidth` 宽度自适应：`build()` 内按 `items_` 最长项 `measureTextWidth`（`EUI-NEO/core/render/text.cpp:2197`）+ 内边距 + （有级联时）箭头区宽度，算出 `width` 再走既有 X 钳（:121）；
- 移植自 `styledMenu` 的既有实现（`styledmenu.h` `showStyledMenu` 已用 `measureTextWidth` 做自适应，逻辑可复用）；
- 落地后 DevDesk 三处（190/160/170）全部删除写死宽度。

## 4. 全局过渡接入（D-2）

**原则**：EUI 组件自身已带默认过渡的（contextMenu 0.12s、virtualList 0.12s、lineChart 0.16s，均 OutCubic）不改；DevDesk 侧补的是**组件不带、当前硬切**的落点。**落点粒度至少到模块 compose 的 builder 链级别**（下表"落点"列 = 文件 · 元素 · 硬切处，实施时以 compose 内具体 builder 链为准）：

| 落点（文件 · 元素 · 硬切处） | 现状 | 方案 |
|------|------|------|
| `markdown_module.cpp` 编辑器三态（编辑/双栏/预览）切换——三态分支切换瞬间整视图硬切 | 硬切 | 视图根 stack 补 `transition + animate`（Opacity\|Transform，OutCubic ~0.15s），由 segmented 回调驱动切换 |
| `todo_ui_board.cpp` `drawCard` / `drawColumnContent` 卡片 drop 落位 | 硬落 | 卡片层容器 `transition + animate`（限定落位过渡，**onDrag 跟手期间不动画**——实施时验证 onDrag 通道不打架） |
| dialog/toast 开合（`todo_ui_dialogs.cpp` / `markdown_module.cpp` / `claude_ui_*` 各弹窗容器；EUI 未内建过渡处） | 硬开硬关 | **关闭过渡必须走"常驻构建 + opacity 收起"模式**：框架**没有"已删元素回放动画"机制**——元素一旦从 compose 树移除即瞬时消失，收起动画无从播放。照 contextMenu / md 菜单既有常驻模式（`markdown_module.cpp:3477` 注释：关闭态由组件内部 opacity/disabled 收起；实现 `contextmenu.h` `renderHiddenLevel:302`：`opacity(0)` + `translateY(-4px)/scale(0.94)` 微位移 + `transition + animate`）：`open` 标志驱动，关闭态**保留元素构建**，`onOpenChange(false)` 只收标志不删元素；EUI dialog/toast 若已内建则跳过（实施时先盘点，不重复加） |
| `claude_ui_list.cpp` virtualList 行 hover/选中 | 无过渡 | row compose 内 `instantStates` → 过渡 states（0.1s OutQuad 区分层级） |
| Home 统计卡 hover/点击反馈 | 无过渡 | 随 M3 换 `card` 后一并补（card 自带 hover 过渡） |

基调：**弹窗/视图 = 0.15s OutCubic；列表项/微交互 = 0.1s OutQuad**（拉开层级，视觉不糊）。

## 5. 手绘换 EUI 组件（D-3/D-5/D-6）

### §5.1 Home 统计卡 → `card` + `lineChart` sparkline

- `home_module.cpp` `drawStatCard`（:33）整体替换为 `components::card(ui, "home.stat.*")`：
  - bg 由 card 组件绘制（color/gradient/radius/border/shadow 一体）；
  - 卡内 label/number/badge 用 text 组件；onClick 保留；
  - 模块注册显隐逻辑（`claudeOn/mdOn/todoOn` 决定卡数量）不动；
  - **必须保留的业务面**：`drawStatCard` 的配色参数 `accent` / `numberColor` / `badgeColor`（非空覆盖默认「accent ? error : 常规」配色——待办/逾期等卡的语义色依赖它，`home_module.cpp:31-32` 注释）与 **Token 速率卡的 0.5s `timerSeconds` 重绘驱动**（流式速率实时刷新：compose 非每帧重跑，靠卡内 0 尺寸 `onTimer(timerSeconds)` 周期触发整卡重绘，`home_module.cpp:95-102`）——换 `card` + `lineChart` 后这两个机制原样保留；
- Token 速率卡：卡内嵌 `lineChart` sparkline（values 来自 `core::proxy_stats::snapshot()`，`home_module.cpp:300-301` 已取数）；API 调用量卡可加 `barChart`（数据 `claude_call_logs` 当日分布，`HomeService::getStats` 扩展或服务侧现取）；
- `home.recent.bg`（:385）等手绘 rect 背景换 `card` 背景层。

### §5.2 profile 列表 + 调用记录 → `virtualList`

- `claude_ui_list.cpp` `drawProfileListPanel`（:248）的 for 循环平铺 + scrollView 换 `components::virtualList`（`virtuallist.h:205`）：`itemCount` = profiles 数、`rowHeight` = 卡高、contentKey 保滚动位置语义（沿用现有 `profilesScrollOffset_` 状态）；
- `drawProfileCard`（:20）保持在 row compose 内的绘制，但外层容器换 `card`（手绘 `.bg/.avatar/.active.bg` 保留为卡的装饰层而非骨架）；
- 调用记录列表（`claude_ui_logs.cpp`）同样换 `virtualList`。

### §5.3 Todo 卡片 → `card`

- `todo_ui_board.cpp` `drawColumnContent`（:250）内卡片绘制换 `card` 组件（拖拽业务逻辑——onDrag/onPress 通道、落位判定——**原样保留**，只换容器绘制）；
- 卡片 hover、逾期红边等状态走 `card` 的 states/border。

### §5.4 shadow/gradient 补齐（D-5）

- 所有换成的 `card` 默认带 `theme::panelShadow`（`card.h:19` CardStyle 自带 shadow token）→ 阴影随换装自动到位；
- 关键背景（Home 欢迎区、设置页主体区、md 预览区）补 `gradient`（2 色水平/垂直，EUI 支持范围），颜色取自现有方案 B token，不新增色板。

## 6. 上游贡献项（EUI-NEO，跨仓库 patch 流程）

| 项 | 位置 | 内容 |
|----|------|------|
| U1 `input` 透出 `onContextMenu` | `EUI-NEO/components/input.h` | Builder 加 `onContextMenu` 透传（dsl element 层已有回调位 `EUI-NEO/core/dsl.h:194,716,1049`，input 组件外露即可）；DevDesk 删 hit 挂靠 hack |
| U2 `contextMenu` 宽度自适应 | `EUI-NEO/components/contextmenu.h:118` | `build()` 内按最长项 `measureTextWidth` 算宽，再走既有 X 钳；移植 styledMenu 实现 |
| U3 `styledMenu` 收起态高度回收 | `EUI-NEO/components/styledmenu.h:259` | `openWindow` 支持运行期尺寸 setter 或收起时重测宽度/高度，避免收起态残留高度 |

**跨仓库流程（按仓库纪律，两个仓库同级、非 submodule）**：
1. 隔离 agent（`isolation: worktree`）在 DevDesk 侧副本 + patch 产上游改动的 patch + 验证（文件副本模拟依赖真跑 DevDesk 编译）；
2. patch 经**非隔离** agent `git apply` 进 `../EUI-NEO`，跑 EUI-NEO 自身编译（+其测试，如 `tests/unit/` 相关用例），原子 commit（消息写明"DevDesk 需要 X 功能"）；
3. DevDesk 顶层 bump `DEVDESK_EUI_NEO_MIN_REV`（`src/module_settings/settings_config.cpp:103` 默认值 + 顶层 CMake 注入点 + 本文件「当前依赖 rev」记录两处同步）；DevDesk 消费新 API 后删对应 hack；
4. EUI-NEO 侧不留尾巴：example/demo 或最小测试用例随贡献带上。

## 7. 文件改动清单

**DevDesk 侧**
| 文件 | 改动 |
|------|------|
| `src/core/window.cc`（或选定落点，§3.1） | 新增统一锚点换算函数（含 Y、含滚动偏移） |
| `src/module_todo/todo_ui_dialogs.cpp` | 菜单锚点改换算函数；删写死宽/板几何；Menu 过渡 |
| `src/module_markdown/markdown_module.cpp` | 树/编辑器菜单统一换算；**隐形卡修复 ✅ 已由 4523e09 落地（任务清单不再含此项）**；删 hit 挂靠 hack；视图三态过渡 |
| `src/module_claude_proxy/claude_ui_list.cpp` + `.h` | 列表换 `virtualList`；卡换 `card`；补右键菜单 |
| `src/module_claude_proxy/claude_ui_logs.cpp` | 调用记录换 `virtualList` |
| `src/module_home/home_module.cpp` | 统计卡换 `card`；Token sparkline `lineChart`；API `barChart`；recent 区背景换 card；hover 过渡 |
| `src/module_home/home_service.h/.cpp` | （如 D-6 需）补 API 调用当日分布查询——纯数据扩展，不动既有字段 |
| `src/module_todo/todo_ui_board.cpp` | 卡片换 `card`（拖拽保留） |
| `src/module_settings/settings_config.cpp` + 顶层 `CMakeLists.txt` | DEVDESK_EUI_NEO_MIN_REV bump（M4 后） |
| `tests/`（`test_menu_beautify.cpp`、新增 `test_ui_modern.cpp` 等） | §9 用例 |

**EUI-NEO 侧（M4，上游贡献）**
| 文件 | 改动 |
|------|------|
| `components/input.h` | U1 onContextMenu 透出 |
| `components/contextmenu.h` | U2 宽度自适应 |
| `components/styledmenu.h` | U3 收起态高度回收 |

## 8. 不做（YAGNI，D-7）

- **不铺开 workshop 组件**：loader / tiltCard / heartSwitch 等按需收敛，不为"丰富"而全量接入；
- **不改业务逻辑**：Todo 拖拽、CRUD、代理流转、设置持久化、统计口径全部不动；
- **不做主题重构**：颜色仍用方案 B token，只补 shadow/gradient 应用，不改 token 体系；
- **不动 styledMenu 独立窗机制**（pet/托盘）：机制本身正常，只补 U3 高度回收；
- **不新建组件**：一律复用既有 EUI 组件，缺的走上游贡献（§6），不在 DevDesk 内 self-draw。

## 9. 测试策略

- **无头 UI 用例**（自制 doctest 框架 `REGISTER_TEST`，`devdesk_tests.exe <子串>` 过滤；理念同 `tests/test_modules/test_menu_beautify.cpp` 对 `styledMenuClampPosition` 的测法）：
  - 换算函数纯测：窗口坐标 ×(-页面原点、-滚动偏移、+dpiScale) 表驱动断言（含 Y、含 todo 列滚动场景）；
  - 隐形卡回归：md 空路径 + **断言 `ctxMenuOpen_ == false`**（4523e09 的修复是空路径早退前**直接赋值** `ctxMenuOpen_ = false` 再 return，`onOpenChange` 回调**并不会**被触发——断言状态本身，不是断言回调触发）；
  - `virtualList`/`card` 换装后的布局不变式：行数与回退滚动位置保持（contentKey 语义）；
  - charts 数据接线：sparkline values 与 `proxy_stats::snapshot()` 对齐。
- **定向测试**：每里程碑只跑受影响子串（`menuBeautify` / `markdown` / `todo` / `claude` / `home`）；全量只在里程碑收尾由主会话跑一次。
- **人工点验（视觉类 headless 测不了，必须真机）**：
  - 三处右键：todo 滚动后右键、md 树深目录右键、编辑器空白/选中文字右键——菜单位置正确、菜单宽度自适应、无隐形卡；
  - Home 卡/图表、profile 列表/右键、Todo 卡 hover 过渡、视图三态切换动画、阴影/渐变观感；
  - macOS 27 侧（历史平台）：横跨两平台截图对比（change 全面后至少一次 mac 构建点验）。

## 10. 拆任务

| 里程碑 | 内容 | 依赖 |
|--------|------|------|
| **M4 上游贡献（先行）** | U1 input onContextMenu / U2 contextMenu 宽度自适应 / U3 styledMenu 高度回收（跨仓库 patch 流程 §6） | 独立，无 |
| **M1 右键菜单统一** | §3.1 换算函数 + §3.2 todo + §3.3 md（树换算、编辑器去 hack；**隐形卡修复 4523e09 已落地、不再实现**）+ §3.4 claude 右键 + §3.5 删写死宽度 | M4 的 U1/U2（编辑器去 hack 与宽度自适应依赖上游；上游未到位则该两项后移） |
| **M2 全局过渡** | §4 全部落点 | 无 |
| **M3 手绘换组件 + charts** | §5.1 Home / §5.2 profile+logs / §5.3 todo 卡 / §5.4 shadow+gradient | M2（过渡基调先行或合并均可） |

依赖关系：M4 → M1（部分）、M2/M3 可与 M4 并行。顺序建议 **M4 → M1 → M2+M3**（M4 先行让 M1 的上游依赖尽早着陆）。

## 11. 已接受边界

- 上游贡献未合入前，M1 的「编辑器去 hack」「宽度自适应」两项**带过渡期实现**（保留 hack 但坐标已统一 / 保留写死宽但不参与锚点），不阻塞其余落地；
- `contextMenu` 宽度自适应以**最长项**为准，单行超长 item 允许被 X 钳截断（既有 `std::min(width_, screenWidth_...)` 逻辑兜底）；
- 调用记录 virtualList 化后，行内富文本/高亮的视觉需在 row compose 内自绘（business 展示层，非骨架手绘）；
- macOS 27 平台本次只保证构建与功能一致，动画/阴影观感差异列入 mac 点验清单（§9）。

## 12. 待用户复核

1. **坐标统一函数落点**：`core/`（可被测、被多模块共用）——倾向 core，需确认不限模块隔离；
2. **M4 先行**（上游贡献先落地）是否接受；还是按 M1→M2→M3 纯 DevDesk 侧先走、上游项后置；
3. **charts 范围**：D-6 的 Home Token `lineChart` + API `barChart` 是否就是首期全部（Todo 逾期分布是否本期做）；
4. **过渡时长基调**：弹窗 0.15s OutCubic / 列表 0.1s OutQuad 是否符合预期手感；
5. **§7 文件清单**是否与范围一致、有遗漏或越界项。