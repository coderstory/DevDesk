# 上线模块原型 → EUI-NEO 控件映射表

> **用途**：上线模块（`module_release`）的 HTML 原型元素，逐一对应到 EUI-NEO 的现有能力；实现前先把「能不能直接用 / 要组合什么 / 必须自绘什么」定死，避免开发阶段才发现缺口。
>
> **路径约定**：`EUI-NEO` 在本仓库同级目录，表中写作 `../EUI-NEO/...`；`src/...` 为 DevDesk 仓库内路径。
>
> **模块隔离**：`module_*` 之间不可互相依赖（`DevDesk/CLAUDE.md`）。`module_todo` / `module_settings` 里的绘制 helper **不能**被 `module_release` 直接 include；下表的「自绘参考」只借用形态，代码需在 `module_release` 内重写，或先提升到 `src/ui_kit/`（跨模块共享层）。
>
> **判定口径**：
> - **可直接用**：组件 API 完整覆盖该元素，只填数据/回调。
> - **需组合**：没有单一组件，但用 2-3 个现有组件可以完整拼出，无需改 EUI-NEO。
> - **需自绘**：EUI-NEO 无对应能力或形态不符，用 DSL 图元（`rect`/`text`/`stack`/`row`/`column`/`flow`）画；必要时在 `src/ui_kit/` 增加共享 helper。
> - **形态不符**：有同名/近名组件，但交互或视觉与需求不一致，不能直接套。

---

## 1. 必选元素映射

| 原型元素 | EUI-NEO 组件 | 头文件 | 是否可直接用 | 备注 |
|---|---|---|---|---|
| 侧边栏一级项 | 无（`sidebar` 是右侧滑出抽屉，不是常驻导航） | `../EUI-NEO/components/sidebar.h:14,239`；自绘参考 `src/app_sidebar.cpp:334-536` | 需自绘 | 常驻侧栏按 `app_sidebar.cpp` 的既有形态自绘：透明 hit `rect`（hover/pressed 三态）+ `text.icon`（FontAwesome）+ `text.label`；选中态叠 `ui_kit::selectionBar`（白卡 + 1px 边框 + 左 3px 主色竖线，`src/ui_kit/selection_bar.h:19,31`）。导航行 40、间距 4、水平内距 16（`src/ui_kit/metrics.h:15`、`src/app_sidebar.cpp:354-355`）。不要用抽屉组件替代。 |
| 侧边栏二级项（折叠树） | 无（`components/` 下无 tree / accordion / collapse） | 图元：`../EUI-NEO/core/dsl.h:1446,1450,1454`；面板：`../EUI-NEO/components/panel.h:28` | 需自绘 | 自绘方案：`row`/`column` + `rect` 行底 + `text` 标签 + 本地 `expanded` 状态；缩进 12/24；折叠箭头用 FontAwesome `text.icon` 并 `rotate`。本期不要求上游补；若后续 ≥2 个模块都要折叠树，再向 EUI-NEO 提 Accordion/Tree 组件。 |
| 任务卡片 | `components::card`（外框） | `../EUI-NEO/components/card.h:33,56,130` | 可直接用（外框），内容自绘 | `card(...).size(w,84).radius(8).shadow(...).border(...).content(...)`；卡内标题/状态 chip/进度自绘。白卡的轻微半透质感用 `ui_kit::cardSurface`（`src/ui_kit/card.h:20`，α=0.92，`src/ui_kit/metrics.h:47`）。若要按优先级给卡面浅染，在 surface 上 `mixColor`（todo 先例 `src/module_todo/todo_ui_board.cpp:66-73`，α=0.12）。 |
| 状态 chip | 无（无 chip / badge 组件） | 自绘参考 `src/module_todo/todo_ui_common.h:16-45` | 需自绘 | `rect`（`radiusPill`）+ `text`（居中、垂直居中、字重 600）。代码权威尺寸：高 22、字号 11（`fontSizeSmall 12 − 1`）、圆角 pill（`src/module_todo/todo_ui_logic.h:602-603`、`todo_ui_common.h:31`）。底 = 功能色 10-12% 混合或 `primarySoftBg`，字 = `success/warning/error/info/primaryText` 对应档。不要跨模块 include `todo_ui_common.h`；release 内复制形态，若 ≥2 模块复用再上提 `ui_kit::chip`。 |
| 进度条 | `components::progress` | `../EUI-NEO/components/progress.h:74` | 可直接用 | `progress(ui,id).size(w,h).value(0..1).theme(componentTokensWeChat(t))`；轨道/填充默认走 `surfaceHover`/`primary`，两端自动 pill（`progress.h` 内 `radius(height*0.5)`）。高度必须显式给：现有代码档 4px（`src/module_settings/github_sync_card.cpp:84`）或 EUI 默认 14（`../EUI-NEO/components/theme.h:85`）；上线模块按 token 文档建议用 4px，原型 6px 需同步改。 |
| 瀑布流分列容器 | `components::virtualMasonry` | `../EUI-NEO/components/virtualmasonry.h:258` | 可直接用 | `virtualMasonry(...).columns(n).gap(16).itemHeight(cb).item(compose)`；可变高 + 虚拟化。若只是等高固定 4 列（八步清单），不要用瀑布流：用 `ui.flow`（`../EUI-NEO/core/dsl.h:1454`）按定宽换行，或手工列切分（`src/ui_kit/columns.h:12` 的 `columnX`）。 |
| 前置项卡片 | `components::card` | `../EUI-NEO/components/card.h:56,130` | 可直接用（外框），内容自绘 | 用 `wrapContentHeight()` 让内容自撑高；卡内「图标 + 标题 + 状态 + 单号 + 到期」全部自绘。圆形状态图标 = `rect(radius = h/2)` + `text.icon`（`../EUI-NEO/core/dsl.h:1246`）；不引入新组件。 |
| 服务卡片 | `components::card`（外框）；折叠无对应组件 | `../EUI-NEO/components/card.h:33` | 需组合（外框可用，展开/收起自绘） | `components/` 下确无 Accordion/Collapse。实现：`module_release` 持有 `expanded` 状态，卡头行（约 44-56 高）常显，卡体按状态条件绘制；卡头 chevron 用 `text.icon` + `rotate`。点击整行切换。不要用 `sidebar` 抽屉，也不要等上游组件。 |
| 八步清单行 | 无专用 step 组件；`stepper` 是数值步进器，不是步骤向导 | `../EUI-NEO/components/stepper.h:343`；图元 `../EUI-NEO/core/dsl.h:1446,1450,1454`；单格底板 `../EUI-NEO/components/card.h:33` | 需自绘 | 每步 = 固定宽高的格（原型 min-height 86），内部「序号 + 圆点勾选 + 步骤名 + 工单号 + 状态副文 + 查看按钮」。列数按可用宽算（4/3/2 档），用 `ui.flow` 或手工列切分；**不要误用 `stepper`**（它是数字 +/- 控件，见 `../EUI-NEO/components/stepper.h:1-80`）。 |
| 勾选态图标 | 无独立 icon/check 组件；`components::checkbox` 是带文字的方形勾选框 | `../EUI-NEO/components/checkbox.h:218`；文本图标 `../EUI-NEO/core/dsl.h:1246` | 需自绘（圆形勾选态） | 原型是 20px 圆形 + ✓。自绘：`rect(radius=h/2, 填 success/primary)` + `text.icon(0xF00C)` 白字；透明态 = 1.5px `borderDefault` 描边圆。点击热区单独一个透明 `rect` 挂 `onClick`。方形表单勾选用 `checkbox`（见下）。 |
| 跳过态 | 无（TextBuilder 无删除线/`text-decoration` 档） | `../EUI-NEO/core/dsl.h:1166-1230`（TextBuilder 属性集） | 需自绘 | 原型语义 = 灰底 + 灰字 + 删除线。实现：灰底 `rect`（`hover`/`borderMuted`）+ 灰字 + 「已跳过」副文；删除线若必须保留，叠一条 1px `rect`，宽度用 `TextPrimitive::measureTextWidth`（`../EUI-NEO/core/render/text.h:80`）量文字宽后算。颜色之外必须同时给文字「已跳过」，避免只靠颜色表达状态。 |
| 全屏文本编辑层 | `DialogBuilder.screen(w,h)` + `InputBuilder.multiline().scrollbar()` | `../EUI-NEO/components/dialog.h:60,242`；`../EUI-NEO/components/input.h:59,61,588` | 可直接用（组合） | "全屏" = 应用内整窗模态：`dialog(...).screen(screenW,screenH).size(screenW-48, screenH-48)`，内容为 `input.multiline().scrollbar().inset(...)` + 底部按钮行。编辑器字体用 `fontFamily("monospace")`（映射见 `../EUI-NEO/core/render/text_shaping.cpp:539-542`）。这是遮罩层，不是独立 OS 窗口；若产品要求独立窗口，改用 `app::openWindow`（见提醒窗行）。 |
| 日期选择 | `components::datePicker` | `../EUI-NEO/components/datepicker.h:451` | 形态不符 | 该组件是 **月/日/年三列滚轮**，不是月历网格（`datepicker.h:190` 注释明确「星期/今日不存在」）。上线需求要「周二/周四高亮 + 非常规日二次确认」，滚轮无法表达。**决定：月历格自绘**（7 列 `ui.row`/`flow` + `rect` 日期格 + 底部图例），自绘量小且完全贴合业务；若产品接受滚轮，才可退回 `datePicker`。 |
| 复选框 | `components::checkbox` | `../EUI-NEO/components/checkbox.h:218` | 可直接用 | `.checked(bool).text("...").onChange(cb).theme(componentTokensWeChat(t))`；默认方框 22px（`../EUI-NEO/components/theme.h:86`），原型 15-16px 时显式 `.boxSize(16)`、`.fontSize(13)`。注意它是方形勾选框，不能替代圆形的步骤勾选态。 |
| 按钮 | `ui_kit::button`（四级）或 `components::button` | `src/ui_kit/button.h:33-35,51`；`../EUI-NEO/components/button.h:342` | 可直接用 | 业务按钮优先 `ui_kit::button`（Primary/Secondary/Text/Danger，40 高、16 内距、8 圆角、14 字，`src/ui_kit/button.h:33-35`）。EUI-NEO 的 `components::button` 用于通用/带图标场景。原型 `btn-sm`（34 高/12 内距）在代码无对应 token，必须显式字面值并在 `module_release` 登记，不能悄悄新增全局档。 |
| 下拉 | `components::dropdown` | `../EUI-NEO/components/dropdown.h:256` | 可直接用 | `.items().selected().onChange().openUp()`；字段靠近窗口底部时必须用 `openUp` 防裁切（`dropdown.h:66-68`）。弹层 z 档 900（`dropdown.h:246-252`），不会被 dialog(1000)/toast(1100) 穿。设置页表单行高 36（`src/module_settings/settings_ui_logic.h:140`）。 |
| 滚动容器 | `components::scrollView`；长列表 `virtualList`；瀑布流 `virtualMasonry` | `../EUI-NEO/components/scrollview.h:217`；`virtuallist.h:205`；`virtualmasonry.h:258` | 可直接用 | 页面/卡区用 `scrollView`（既有页面 `step(60)`）、`scrollbarOverlay()` 覆盖式不占内容宽（`scrollview.h:42`）；长日志/记录用 `virtualList`；瀑布流用 `virtualMasonry`。`contentKey` 用于内容高缓存失效（`scrollview.h:51`）。 |
| 加载占位 | 无（`components/` 下无 skeleton / placeholder 组件） | 图元 `../EUI-NEO/core/dsl.h:1446,1450`；空态 `src/ui_kit/empty_state.h:35` | 需自绘 | 自绘骨架：若干 `rect`（`hover`/`borderMuted`）+ 不同宽高模拟标题/副题/chip，整体 `animate(core::AnimProperty::Opacity)` 做呼吸。加载失败用 `ui_kit::emptyState`（纯文字 + 就近重试按钮），不要用加载占位冒充错误态。上游无需补。 |
| 全屏提醒窗 | 无专用组件；用 `app::openWindow` 开独立窗口，内容自绘 | `../EUI-NEO/include/eui/dsl_app.h:222,355` | 需组合（API 可用，内容全自绘） | 设计 §6.3 要求**独立全屏窗、不是应用内遮罩**：`app::openWindow(DslWindowConfig{...}, compose)`；`DslWindowConfig` 提供 title/size/decorated/alwaysOnTop 等（`dsl_app.h:222-245`），无 `fullscreen` 布尔，按屏幕工作区给 `windowWidthValue/windowHeightValue` 即可。**尺寸单位注意**：这两个值直接进 `glfwCreateWindow`（`../EUI-NEO/core/window/window_backend.cpp:1181`），Windows 下是客户区**物理像素**（逻辑值 × `app::contentScale()`，`../EUI-NEO/include/eui/app.h:103`），macOS 下是点；不能把主窗的逻辑尺寸原样传进去。窗内头部（铃铛 + 标题 + 级别切换）、分组列表、底部「关闭后 5 分钟再提醒」与关闭按钮全部自绘（可复用 `card`/`progress`/`button`）。`dialog .screen()` 只能作应用内兜底，不满足独立窗要求。 |

---

## 2. 上线原型中额外出现、需要一并落位的元素

| 原型元素 | EUI-NEO 组件 | 头文件 | 是否可直接用 | 备注 |
|---|---|---|---|---|
| 一级 Tab（服务目录 / 环境定义） | `components::segmented` 或 `components::tabs` | `../EUI-NEO/components/segmented.h:146`；`../EUI-NEO/components/tabs.h:141` | 可直接用 | 原型是胶囊容器（`big-tabs`）→ `segmented` 更接近；设置页现有 `ui_kit::wtab` 是块面风格（选中 `primarySoftBg` 底 + 8px 圆角 + `borderDefault` 描边，`src/ui_kit/wtab.cpp:15-31`），不是胶囊容器。二者选一，不要混用。 |
| 数据表格（服务目录、环境定义） | `components::dataTable` | `../EUI-NEO/components/datatable.h:52,197` | 需组合 | 该组件行/列只接受字符串（`rows(std::vector<std::vector<std::string>>)`），不能直接塞 chip、按钮、自定义列。若表格需要类型 chip/操作按钮：用 `dataTable` 只做纯文本列，或自绘行（`row` + 列宽 + `text`/`chip` + 右侧按钮）。上线模块的表格列含 chip 与操作，**决定自绘行**，表头可复用 `borderDefault` 底线。 |
| 时间选择（环境时间窗） | `components::timePicker` | `../EUI-NEO/components/timepicker.h:440` | 可直接用 | `.time(h,m).minuteStep(1..30).onChange()`；跨零点由业务规则判断（结束 ≤ 开始视为次日），组件只负责取值。 |
| 确认弹窗（撤回/移除/非常规日期） | `components::dialog` | `../EUI-NEO/components/dialog.h:242` | 可直接用 | `dialog(...).size(460, ...).title().message().primaryText().secondaryText().onPrimary().onSecondary()`；高风险操作（撤回/移除/作废）把 `DialogStyle.primary` 覆为 `colors.error`（DialogStyle 字段见 `../EUI-NEO/components/dialog.h:15-44`），普通确认用 `primary`；`ui_kit::button` 的 Danger 级不用于对话框内部。 |
| Toast 提示 | `components::toast`；DevDesk 封装 `ui_kit::drawToast` | `../EUI-NEO/components/toast.h:244`；`src/ui_kit/toast.h` | 可直接用 | EUI-NEO toast 单实例、无队列；需要排队/顶替语义时走应用级 `app::showAppNotice`（同帧后发顶替先发，`src/app_sidebar.cpp:52-53`）。 |
| 开关（模块/配置开关） | `ui_kit::wswitch` 或 `components::toggleSwitch` | `src/ui_kit/wswitch.h:31`；`../EUI-NEO/components/switch.h:173` | 可直接用 | 微信风格开关用 `wswitch`（44×24、轨道 pill、白色滑块，`src/ui_kit/wswitch.cpp`）；EUI 自带 `toggleSwitch` 可带文字，但视觉与微信风格不同，上线模块统一用 `wswitch`。 |

---

## 3. 缺口汇总（明确结论）

| 缺口 | 现状 | 本期方案 | 是否建议 EUI-NEO 上游补 |
|---|---|---|---|
| Accordion / Collapse（服务卡、二级导航折叠） | `components/` 下无对应组件 | 自绘：状态 + 条件绘制 + chevron rotate | 本期不补；跨 ≥2 模块复用再提 |
| Tree（侧边栏二级折叠树） | 无 | 自绘 row/column 缩进层级 | 本期不补 |
| Chip / Badge | 无 | 自绘 `rect + text`（形态照 todo chip） | 不补；可上提 `ui_kit::chip` |
| Skeleton 加载占位 | 无 | 自绘灰块 + opacity 动画 | 本期不补 |
| 月历网格 | `datePicker` 是三列滚轮，形态不符 | 自绘 7 列月历（业务规则需要） | 可选；若后续多个模块要月历再提 |
| 步骤清单格（八步） | `stepper` 是数值步进器 | 自绘 + `flow`/手工列 | 本期不补 |
| 圆形勾选态 | `checkbox` 只做方形 | 自绘圆形 + FontAwesome ✓ | 不补 |
| 文本删除线 | `TextBuilder` 无该属性 | 文字 + 1px 线 rect（按实测文字宽） | 可提 `strikethrough` 属性；本期不阻塞 |
| 全屏提醒独立窗 | 无专用组件 | `app::openWindow` + 自绘内容 | 不补 |

---

## 4. 已定实现决策（不留「待确认」）

1. **侧边栏一级/二级**：全部自绘，沿用 `app_sidebar.cpp` 的行高、内距与 `selection_bar` 选中态；不引入 EUI-NEO 抽屉。
2. **服务卡折叠**：`module_release` 内持有展开状态，卡头常显 + 卡体条件绘制；不等待上游 Accordion。
3. **八步清单**：自绘等高格子 + `flow` 换行（4/3/2 列），不使用 `stepper`。
4. **日期选择**：自绘月历格（周二/周四高亮 + 非常规日二次确认）；不套三列滚轮。
5. **跳过态**：灰底 + 灰字 + 「已跳过」；删除线作为可选的 1px 线叠加，不依赖文字 API。
6. **全屏配置编辑**：应用内 `dialog.screen()` + 多行 `input`；若产品明确要跨应用全屏，另开 `openWindow` 窗口，二者不混用。
7. **全屏提醒**：`app::openWindow` 独立窗，内容是自绘列表；`dialog.screen()` 仅当产品改口接受应用内遮罩时的兜底。
8. **进度条高度**：复用现有 4px 档；原型从 6px 改 4px，不新增全局档。