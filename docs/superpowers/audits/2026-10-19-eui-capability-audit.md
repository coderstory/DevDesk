> **存档说明**（非原文内容）：来源：只读审计子代理 2026-10-19 产出，原文照录（HTML 转义已还原）。作为《DevDesk 微信风格 UI/UX 整改》的 EUI 能力权威证据；工单与文档引用本文时只许引用行号原文，不许转述。

# EUI-NEO × DevDesk 交互能力审计报告（只读，13 项）

路径约定：`EUI-NEO\…` = `D:\project\gui\EUI-NEO\…`；`DevDesk\…` = `D:\project\gui\DevDesk\…`。所有结论均一手源码取证。

## 总矩阵

| # | 能力 | 结论 | 关键证据 |
|---|------|------|----------|
| 1 | 主题/设计令牌 | 有（双侧均有，DevDesk 为唯一色源） | EUI-NEO\components\theme.h:81-98；DevDesk\src\core\theme.h:17-78 |
| 2 | hover | 有（事件/样式/绑定全有；无查询式 API） | EUI-NEO\core\dsl.h:726,799,1016 |
| 3 | tooltip | 有 | EUI-NEO\components\tooltip.h:32-146 |
| 4 | toast | 有（自动消失有；多实例队列无） | EUI-NEO\components\toast.h:39-240 |
| 5 | 弹出层 | 有机制、无通用组件（stack+zIndex/dialog/styledMenu/多窗） | EUI-NEO\components\dropdown.h:145-162；dialog.h:96-121 |
| 6 | 下拉/菜单 | 有（dropdown/contextMenu/styledMenu；可搜索 combo 无） | EUI-NEO\components\dropdown.h:51-84 |
| 7 | 骨架屏 | 无组件；自绘基础齐备且有官方自绘先例 | EUI-NEO\apps\calgary_gallery\pages\exhibitions_page.h:27 |
| 8 | 文本省略 | 单行省略有；多行省略无；hover 全文无自动机制 | EUI-NEO\core\render\text_types.h:30-33 |
| 9 | 富文本分段 | 有（颜色分段；字号/字重分段无） | EUI-NEO\core\render\text_types.h:37-41 |
| 10 | 拖拽 | 有元素级 onDrag；drop-target API 无；DevDesk Todo 拖拽已实现 | EUI-NEO\core\dsl.h:87-104,781；DevDesk\src\module_todo\todo_module.cpp:213,461 |
| 11 | 剪贴板 | 有（读写都在 EUI；DevDesk 仅用写） | EUI-NEO\core\window\window_backend.h:24-25 |
| 12 | 截图/探针 | 有（进程内 PNG 截图 + 按场景 probe，无任意页面参数） | DevDesk\tests\probes\probe_harness.h:235-238,367,495 |
| 13 | 图标 | 有（FA7 Solid 图标字体 + SVG/位图通道；线性风格字形未随包） | EUI-NEO\core\render\text.cpp:37-38；dsl.h:1246-1256 |

## 分项子能力详证

**1. 主题/样式集中机制**
- EUI 令牌结构：有 — theme.h:81-98 `ThemeMetricTokens`(Typography/Spacing/Radius/ControlSize) + `ThemeColorTokens`(background/primary/surface/surfaceHover/surfaceActive/text/border/dark)；light()/dark() :155-179；派生 helper `pageVisuals`:181、`fieldVisuals`:203、`buttonHover`:232、`buttonPressed`:236、`border`:240、`shadow`:248、`popupShadow`:270。
- EUI 控件取色方式：显式传值，无全局注册表 — 组件 Style 构造函数吃 tokens（button.h:14-24 `ButtonStyle() : ButtonStyle(theme::dark())`、:16-24 从 tokens.primary/surface 派生）；builder 一律 `.theme(tokens)`（button.h:58、tooltip.h:42、toast.h:58、dropdown.h:76、contextmenu.h:121、progress.h:30、panel.h:43）；默认构造回退内置 `theme::dark()`（button.h:14、tooltip.h:14、toast.h:16、panel.h:11）。EUI 无 ThemeProvider/当前主题查询（全仓 grep 仅命中 site\app.js 网页 JS）。
- DevDesk 侧：有完整集中令牌 — src\core\theme.h:17-50 `ColorTokens`（21 色，含 hover/active/primaryHover/primaryText/success/warning/error/purple/pink/blue 及 WCAG 注释）、:53-69 `SizeTokens`（17 值）、:78 `current()` 全局唯一当前主题、:80-81 setDark/setLight、:84 setFontScale、:94-95 setAccent；src\core\component_theme.h:15-26 `componentTokens()` 是 DevDesk token→EUI 组件 token 的唯一映射，头注释 :1-4 明令禁用 EUI 内置 light()/dark() 防色源分裂。模块消费：`theme::current()` 15 处/11 文件；`componentTokens` 43 处/18 文件。
- 手工色残留：DevDesk src 中 `(Color|color){0.x,…}` 字面量 19 处/9 文件（agent_detail_dialog.cpp×5、ui_kit\splash.cpp×3、markdown_module.cpp×3、todo_ui_board.cpp×1 等）。

**2. hover**
- hover 事件回调：有 — dsl.h:726-728 `onHover(std::function<void(bool)>)`（落到 :198 `onHoverChanged`）；mousearea.h:68-70 onHover/onEnter/onLeave。
- 悬停样式变化：有 — 形状三态 DSL `states(normal, hover, pressed)` dsl.h:1016；按钮接线 button.h:147；滚动条 thumb 三态 scroll.h:95（thumbHover :18）；表行 hover 色 datatable.h:23/:36 `rowHover = tokens.surfaceHover`；侧栏项 hover 混色 sidebar.h:142-143。
- 元素属性绑定他元素 hover：有 — `hoverOpacityFrom(id)` dsl.h:799/1929-1934（`hoverOpacitySourceId` :209；runtime 侧 `hoverTargetId` core\dsl_runtime.h:97）。
- 查询式 hover 状态 API（如 isHovered()）：无 — 于 core\ grep `hovered|Hovered|isHover|hoverState|hoverId|hoverElement|悬停|mouseEnter|mouse_enter|pointerEnter|onMouseMove` 0 命中；状态只能经回调/绑定获得。
- DevDesk 已有实例：有 — claude_ui_form.cpp:841-850（tooltip `.source()` 绑 hover）；markdown_module.cpp:1521/1529（mouseArea hit `.onHover`）；agent_ui_shared.h:56（buttonHover states）；页面统一走 `t.colors.hover` 10+ 处（app_sidebar.cpp:163/212、home_module.cpp:91/371/385/788、home_quota_card.cpp:349/480、about_module.cpp:153、agent_detail_dialog.cpp:501）。
- 列表行 hover：DataTable 内建有（datatable.h:23,36）；VirtualList 无内建（virtuallist.h grep hover|Hover 0 命中）→ 虚拟列表行 hover 需自挂 mouseArea。

**3. tooltip**
- tooltip 控件：有 — components\tooltip.h:32-146 `TooltipBuilder`：`.source(id).value(text).anchor(x,y).bounds(w,h).style/.theme/.zIndex`（:37-47），工厂 :142；build 内 `.hoverOpacityFrom(sourceId_)` :71 → 悬停源元素自动显隐；小三角指针 polygon :85-91 + 圆角边框阴影底板 :93-100；避让/翻轴逻辑 :50-64。
- DevDesk 实例：有 — claude_ui_form.cpp:843-850 `components::tooltip(...).source("claude.form.env.row.N.key.hit").value(hint).anchor().bounds().theme().zIndex(60)`；词表 env_var_hints.h:296。
- 无自动 tooltip：components\ 内 grep `tooltip|Tooltip` 仅 tooltip.h 自身——任何组件不会自动挂 tooltip；home_module.cpp:590 注释明写自绘柱状图"无悬停 tooltip，用柱顶数值标签"。
- 变体 hint/bubble：无 — components grep `hint|Hint` 仅命中 typography token 命名。

**4. toast**
- toast 控件：有 — components\toast.h:39-240 `ToastBuilder`：`.visible/.bindVisible(Signal)/.title/.message/.icon(codepoint)/.style/.theme/.transition/.zIndex/.duration(s)/.action(text,cb)/.onDismiss/.onAutoDismiss`（:44-73）。
- 自动消失：有 — :65 duration + :213-214 `timer.onTimer(autoDismissSeconds_, onAutoDismiss)`。
- 多条队列/堆叠：无 — toast.h grep `queue|Queue|single|单实例|next` 0 命中，单实例由调用方状态驱动；DevDesk 封装 ui_kit\toast.h:16-51 `ToastState{open,title,body,isError}` + `drawToast()`。
- DevDesk 实例：有 — claude_ui_ccswitch.cpp:160/225/232/238 `hooks.toast(...)`、claude_ui_logs.cpp:108、claude_ui_form.cpp:213 等。
- snackbar/notification 组件变体：无 — components grep 0 命中（DevDesk\src\platform\notification.* 是 OS 系统通知，非 UI 内 toast，勿混）。

**5. 弹出层/浮层**
- 通用 popup/overlay/popover/drawer/sheet 组件：无同名组件 — components grep `overlay|popover|drawer|sheet|浮层` 仅命中 token 命名（radius.overlay/spacing.overlay）、carousel 渐变 overlay（carousel.h:32-45）、调试浮层 helper layout_debug.h:35 `layoutDebugOverlay`。
- 实际浮层机制（三层形态，均可承载帮助面板/悬浮说明面板）：
  - 窗内浮层 = `stack + zIndex` 手工搭：dropdown.h:145-162 弹层子树 zIndex 抬档，注释列全仓 z 惯例（timepicker 1000 / contextmenu 1050 / toast 1100）；dialog.h:96-121 全屏 backdrop rect（:100-109 可点击关闭）+ 面板 = 模态浮层；分层层级有契约测试 tests\unit\zindex_escape.cpp。
  - 独立窗浮层 = styledmenu.h:7 注释"经 app::openWindow 开独立无边框透明置顶子窗"、:461 `app::openWindow(...)`；多窗 API include\eui\dsl_app.h:322-323 `openWindow(config, composeFn)`。
  - datepicker/timepicker/colorpicker 各自带 popup（components grep `popup` 12 文件 58 处）。
- DevDesk 实例：ui_kit\dialog_form.cpp:12 `components::dialog`；agent_detail_dialog.cpp、git_error_dialog.h（设置模块弹窗）。

**6. 下拉菜单**
- 单选下拉：有 — dropdown.h:51-84 `DropdownBuilder`：`.items/.selected/.bind(Signal)/.placeholder/.open/.openUp/.bindOpen/.itemHeight/.onChange/.onOpenChange/.zIndex`（:64 placeholder、:83 onChange）。
- 右键/级联菜单：有 — contextmenu.h:86-130 `ContextMenuBuilder`（`.position/.items/.onSelect/.onSelectPath` :98-129 级联路径选择）；DSL 钩子 `onContextMenu` dsl.h:719。
- 独立窗样式菜单：有 — styledmenu.h:363 `showStyledMenu`、:698 `closeStyledMenu`（FA 图标 :49 `iconCodepoint`、hover/pressed 态 :3 注释）。
- 可搜索/可输入 combo：无 — components grep `combo|searchable|autocomplete|suggest` 0 命中。
- DevDesk 实例：20+ 处 — markdown_module.cpp:2430、claude_ui_quota.cpp:110/220、claude_ui_form.cpp:467/547/611/661、settings_ui_cards.cpp:119/150（主题/字号下拉）、右键菜单 markdown_module.cpp:3352/3396、todo_ui_dialogs.cpp:235、claude_ui_list.cpp:740、托盘菜单 main.cpp:473 `showStyledMenu`。

**7. 骨架屏/加载占位**
- skeleton/spinner/shimmer 组件：无 — components\ grep `skeleton|spinner|shimmer|placeholder|loading|占位|骨架` 0 命中（`placeholder` 命中均为 input.h:26/58、dropdown.h:64 的输入提示文案语义，非加载占位）。
- 加载进度条：有 — progress.h:22-36 `ProgressBuilder`（:28 value clamp 0..1；grep indeterminate 0 命中，无不确定态模式）。
- 自绘基础（全部现成）：rect + radius + border + shadow + gradient（panel.h:28-36、dialog.h:123-128、tooltip.h:93-100）；任意多边形 polygon（dsl.h:1142-1156、tooltip.h:85-91）；透明度/变换动画 `transition + .animate(AnimProperty::Opacity|Transform)`（dialog.h:105/120）；二色渐变 panel.h:21。
- 官方自绘先例：有 — apps\calgary_gallery\pages\exhibitions_page.h:27/67 `ui.rect(id + ".skeleton").size(width, imageHeight).color(...)`、apps\calgary_gallery\components\detail_media.h:15（图片位骨架占位）。DevDesk src 无骨架屏（grep skeleton 0 命中）。

**8. 文本省略**
- 单行省略号：有 — `TextOverflow::Ellipsis` core\render\text_types.h:30-33（TextStyle.overflow :67）；DSL `TextBuilder::overflow()` dsl.h:1207-1209；契约测试 tests\unit\text_ellipsis.cpp:1-9（仅 wrap==false 且 maxWidth>0 生效；省略号 advance 预留预算；CJK 按码点粒度）。
- 多行省略（末行截断加 …）：无 — text_types.h:24-28 注释明写 wrap==true 时 Ellipsis 退化为无操作（默认 `Clip` 硬裁）。
- 截断后 hover 显示全文：无自动机制 — 组件不自动挂 tooltip（见第 3 项）；可用组合件自建：tooltip.h:37 `.source()` + tooltip.h:71 hoverOpacityFrom + mousearea.h:68 onHover。DevDesk 现状：18 处 `.overflow(Ellipsis)` 调用点（home_quota_card.cpp:427/438/557、markdown_module.cpp:416/1260、todo_ui_board.cpp:65/320/613/701、todo_ui_dialogs.cpp:483、claude_ui_detail.cpp:225/355/364/388/748、claude_ui_quota.cpp:204/257、settings_ui_cards.cpp:217）无一接 tooltip；全 DevDesk tooltip 实例仅 claude 表单 env 提示一处。

**9. 富文本/分段高亮**
- 混合颜色分段渲染：有 — `TextRun{byteStart, byteEnd, color}` text_types.h:37-41（UTF-8 字节偏移，与光标/度量同坐标系）；TextStyle.runs :72；DSL `.runs(vector)/.run(start,end,color)/.clearRuns()` dsl.h:1230-1243；归一化 `normalizeTextRuns` core\render\text.cpp:1189（clamp 到码点边界/去重叠/合并同色，text_types.h:52-56 注释点名"供 DevDesk 高亮器使用"）；顶点着色 core\render\text.cpp:1995-2005、setRuns core\render\text.h:72。
- 分段字号/字重/斜体：无 — text_types.h:40 注释 `color // v1 唯一样式维度`。
- DevDesk 实例：有 — markdown_module.cpp:1833 `.runs(hl.runs)`；五色代码词法器 md_keyword_highlight.h:20-33（关键字/字符串/注释/数字/默认 → `eui::TextRun`）；行级高亮 md_highlight.h:1。
- 文件树搜索命中关键词高亮：无现成实例 — DevDesk grep `高亮|highlight` 命中均为代码高亮/todo 拖拽列高亮/侧栏当前页高亮；实现基础即 `.runs()`（byte offset 由搜索串定位，normalizeTextRuns 已处理边界与去重叠）。
- 输入选区渲染：有 — input.h:156-181（selectionStart/End、selectionRects）。
- 整篇富文本文档：有 — components\markdown.h:29-58 `MarkdownStyle`（heading/codeText/codeBackground/h2Size/codeFontFamily 等，按块多元素渲染）。

**10. 拖拽**
- EUI 元素级拖拽回调：有 — dsl.h:781-783 `onDrag(DragEvent)`；DragEvent dsl.h:87-104（x/y/deltaX/deltaY/totalX/totalY/button/modifiers + U1 scale/space 契约）；mousearea.h:81-83 `onDragStart/onDrag/onDragEnd`、:61 `dragThreshold`、:62 `suppressClickAfterDrag`。
- drop-target / onDrop API：无 — 全仓（排除 3rd/build/.claude/site/docs）grep `onDrop|FileDrop|fileDrop|dropFile|onFileDrop|dropTarget` 0 命中；window_backend.cpp、input_state.h、input.h 单独 grep `drop` 0 命中（glfw drop 回调未接线）。
- OS 文件拖入窗口：无（同上，0 命中）。
- DevDesk Todo 卡片拖拽排序 + 跨列移动：已完整实现 — todo_ui_board.h:58-72 `DragState`（dragActive/dragTaskId/dragHoverColumn/dragHoverRow/dragGhost*）；几何纯函数 todo_ui_logic.h:133 `dragDropTarget`（指针→目标列）、:165 `dragDropRowIndex`（插入行号）、:206 `dragGhostAnchor`（跟手副本）；落点收尾 todo_module.cpp:461（"跨列 → 有悬停行按行插入否则追加列尾；同列 → 上下重排"）、:213/257 `service_->moveTask(taskId, toColumnId, newPosition)`、:355 `reorderColumns`、:504 `moveTaskByKeyboard`（键盘挪卡）；拖拽视觉 todo_ui_board.cpp:244-253（目标列高亮/半透占位条）。注意：命中换算是模块自建（makeDragHitInput todo_module.cpp:412-426），非框架 drop API。
- 若新增拖拽面的实现基础：mousearea onDrag 三回调 + dragThreshold；命中换算先例 ui::dragDropTarget/dragDropRowIndex（todo_ui_logic.h:121-206）。

**11. 剪贴板**
- EUI 写剪贴板：有 — `core::window::setClipboardText(const std::string&)` core\window\window_backend.h:25（返回 void）。
- EUI 读剪贴板：有 — `std::string clipboardText(Handle window)` window_backend.h:24。
- DevDesk platform 层剪贴板文件：无 — src\platform\ 目录清单核对，无 clipboard_* 文件；DevDesk 直接调 EUI 的 core::window API。
- DevDesk 写入实例：有 — claude_module.cpp:727（导出 JSON）、agent_detail_dialog.cpp:713、agent_module.cpp:1436、settings_module.cpp:600；claude_module.cpp:673 注释"setClipboardText 无回读校验 → 固定 LOG_WARN 留痕"（写后不可验证是既知事实）。
- DevDesk 读剪贴板实例：无 — src grep `clipboardText(` 0 命中。

**12. 截图/探针**
- 自动化探针机制：有 — DevDesk\tests\probes\：agent_dialog_probe / agent_e2e_probe / home_probes / module_toggle_sidebar_probe / screenshot_probe / settings_dropdown_z_probe + probe_harness.h；tests\CMakeLists.txt:222-225 每个 .cpp 是独立可执行、以 `LABELS "probe"` 注册 ctest；构建入口 scripts\build-test-run.sh（:5 用法行含 `--no-run`，:136 `--no-run) RUN_APP=0`，:484 段 3a UI probe 门禁）；CI 编译开关 DEVDESK_BUILD_PROBES=0（.github\workflows\ci.yml:28）；跳过守卫 scripts\check-probe-skip-gate.sh。
- 进程内截图（无需系统截屏授权）：有 — probe_harness.h:235-238 `--screenshot <路径>` / `--screenshot-seeded`（中途态落图）、:274-276 参数解析、:367 `FrameCapture`（glReadPixels 读自身 GL 后备缓冲）、:449 `capturePng`、:495 `captureIfRequested` 标准接线点；无 GUI 会话走 SKIP_RETURN_CODE 77 诚实跳过。
- 对指定页面截图：按场景而非任意页面 — probe 参数表仅 `--frames/--size/--manual/--screenshot/--screenshot-seeded/--quota-fixture`（probe_harness.h:259-260），无页面名参数；每个 probe 硬编码场景（home_probes.cpp:342 `pageId = "home"`、:473 `probe::composeModuleFrame(...)` 驱动模块 compose）。已覆盖：home（余量卡真数据落图）、settings（下拉 z 序）、sidebar（真点击派发）、agent（对话框/E2E）、screenshot_probe（帧捕获自测）；Todo/Markdown/Claude/About 页无专用 probe（tests\probes\ 目录清单核对）——新页面截图 = 新增/改造 probe 并接 captureIfRequested（机制通用：probe_harness.h:300 composeModuleFrame + module_interface.h 各模块 compose 接口）。
- 系统级截屏备份路径：scripts\check-consistency.sh:5-34（Playwright / Windows CopyFromScreen / macOS screencapture，依赖会话权限）。
- EUI-NEO 侧探针：tests\probes\（gpu_image/input_runtime/render_clear/render_context/shadertoy/window_only）— 仅单像素 glReadPixels 断言（gpu_image_probe.cpp:324、shadertoy_probe.cpp:367），无整帧 PNG 落图 CLI。

**13. 图标**
- 图标字体机制：有 — EUI-NEO\assets\"Font Awesome 7 Free-Solid-900.otf"；默认注册 core\render\text.cpp:37-38 `kDefaultUiFontFile` / `kDefaultIconFontFile`；app 级覆盖 `.iconFont()` / `.fonts(text, icon)` include\eui\dsl_app.h:141-144；查找顺序与兜底 docs\DSL.md:195/453（assets 缺失时回平台 symbol 字体，FA codepoint 不保证有兜底字形 → 发布须带字体）。
- DSL API：有 — `TextBuilder::icon(unsigned codepoint|std::string)` 自动切 `fontFamily="Icon"` dsl.h:1246-1256；按钮内嵌图标 button.h:179-181（:106 iconFont 尺寸推导）；导航条目图标 navbar.h:18 `unsigned int icon`、:203 `.icon(item.icon)`、:32 brand(title, icon)；toast.h:55 `.icon(codepoint)`；styledmenu.h:49 iconCodepoint。
- 风格口径：随包字体是 FA7 Free-**Solid**（实心体，见资产文件名）；assets 内无 outline/线性图标字体（其余资产仅 icon.svg/icon.png/一张插画 SVG）；线性图标路线的现实形态 = SVG 通道。
- SVG/位图通道：有 — ImageBuilder 按 `ElementKind::Svg` 走 svgSource（dsl.h:1266-1268，SVG 栅格化），位图走 imageSource 同 builder。
- DevDesk 实例：28 处 `.icon(0x…)` —— 导航每模块 iconGlyph()（core\module_interface.h:46 纯虚；home 0xF015 home_module.cpp:228、todo 0xF058 todo_module.cpp:39、markdown 0xF044 :201、agent 0xF544 agent_module.h:112、settings 0xF013 :146、about 0xF05A :183）；文件夹/打开 0xF07B markdown_module.cpp:784（文件树行）、0xF07C settings_ui_cards.cpp:453；折叠箭头 0xF0C9/0xF100 app_sidebar.cpp:222；搜索 0xF002 markdown_module.cpp:525；关闭 0xF00D claude_ui_form.cpp:870。
- 问号帮助图标：无现成调用点 — DevDesk src grep `0xF059|0xF128|question` 0 命中；可从同一 FA 字体取 question 类 codepoint，**但该具体字形在 FA7 Solid otf 内的可用性未做渲染验证（存疑）**。

## 样式架构总述

DevDesk 当前取色/布局实际模式：`core::theme::current()`（theme.h:78，全局单例 Tokens，支持 dark/light/accent/fontScale 切换 + ChangeHandler 联动）是唯一色源 → 模块取 tokens 后经 `component_theme.h componentTokens()` 桥成 EUI 组件主题（43 处），hover/active 一律走 `t.colors.hover / t.colors.active`（10+ 处），共享底板集中在 `src\ui_kit\`（card/dialog_form/toast/splash/metrics）；布局以绝对 `.position().size()` 链式为主（如 claude_ui_form.cpp:859-862），间距/圆角/字号引用 `t.sizes.*`。EUI-NEO 侧是"令牌显式传值"：components\theme.h 定义令牌与派生函数，无全局注册表，组件默认构造回退内置 dark()（button.h:14 等）。结论：令牌系统已有且应继续落在 DevDesk `src\core\theme.h` 的 ColorTokens/SizeTokens（含扩展新语义槽位）；EUI 组件层仅在需要新令牌字段时改 `ThemeColorTokens`（theme.h:88-98）并在 componentTokens() 补一行映射。整改需清理的偏离面是 19 处 `Color{0.x}` 手工字面量（9 文件，agent_detail_dialog/splash 最多）。

## 覆盖度声明

- 方法说明：本审计用 Read（theme/tooltip/toast/mousearea/dialog/panel/text_types/card/claude_ui_form/md_keyword_highlight 全文或大段）+ Grep（其余全部）。**重要陷阱**：正向 glob（`components/*.h`、`{core,components}/**`）在本环境产生假阴性——首次用该写法得出的"popup/overlay/combo/skeleton/hover-state 无匹配"全部已用直接路径重跑推翻或复核（popup 实为 12 文件 58 处）；下列否定性结论均为直接路径或经复核的负 glob 结果。
- 逐项搜索词与范围：①`ThemeProvider|currentTheme|setTheme|activeTheme|globalTheme`（EUI-NEO 全仓负 glob 排除 3rd/build/.claude/site/docs）+ theme.h/component_theme.h 全读 + DevDesk `theme::current()`/`(Color|color){0\.` 计数。②`hover|Hover`（components 计数 27 文件）、`onHover|onEnter|onLeave`、`hoverOpacityFrom`、`hovered|Hovered|isHover|hoverState|hoverId|hoverElement|悬停|mouseEnter|mouse_enter|pointerEnter|onMouseMove`（core\ 0 命中）、DevDesk `onHover|hoverOpacityFrom|colors\.hover`。③`tooltip|Tooltip`（EUI components、DevDesk src）、`hint|Hint`。④`queue|Queue|single|单实例|next`（toast.h）、`snackbar|notification|bubble`（components）。⑤`popup|Popup`、`overlay|popover|drawer|sheet|浮层`（components）、`openWindow`（全仓）。⑥`combo|searchable|autocomplete|suggest`（components）、dropdown/contextmenu/styledmenu API 行。⑦`skeleton|Skeleton`（全仓）、`spinner|shimmer|placeholder|loading|占位|骨架`（components、image.h）。⑧`Ellipsis|省略`（EUI 全仓）、`TextOverflow::Ellipsis`（DevDesk src）。⑨`TextRun|styledRun|runs|normalizeTextRuns`（EUI core）、DevDesk `.runs\(|高亮|highlight`。⑩`drag|Drag|drop|Drop`（EUI）、`onDrop|FileDrop|fileDrop|dropFile|onFileDrop|dropTarget`（EUI 全仓 0 命中）、`drop`（window_backend.cpp/input_state.h/input.h 0 命中）、DevDesk module_todo `onDrag|DragEvent|drag|moveTask|reorder`。⑪`clipboard|Clipboard|剪贴板`（EUI 全仓、DevDesk src）、platform 目录清单核对。⑫probes 目录清单、probe_harness/home_probes grep、tests\CMakeLists.txt、ci.yml、build-test-run.sh、check-consistency.sh、EUI tests\probes `glReadPixels`。⑬assets 目录清单、`FontAwesome|iconFont`（EUI 全仓）、`\.icon\(0x`/`iconGlyph`/`0xF059|0xF128|question`（DevDesk src）。
- 可能遗漏：a) EUI core 原生 pointer-move 回调派发细节未逐行列（若存在裸 onPointerMove，可自实现额外 hover 检测）；b) EUI examples\apps 内是否有 tooltip/浮层封装惯例未逐文件审（只审了 calgary_gallery 骨架先例与 gallery_controls.h:512 openWindow）；c) toast.h 200 行后布局细节未读；d) `Color{1.0f` 等白色字面量未计入手工色计数（19 处为下界）；e) FA7 otf 内具体字形集（question/folder 等）未渲染验证，仅确认加载机制与 28 处既有 codepoint 可用；f) EUI-NEO\examples 的大量 demo 未全审（负结论均基于 core/components/include 主体 + 全仓关键词搜索，examples 中关键词搜索已覆盖）；g) DevDesk docs\ 下 EUI-NEO-CAPABILITIES.md 为 2026-09-20 审计快照，本报告与其不一致处（如 toast 单实例、无 skeleton）均以本次源码取证为准。
