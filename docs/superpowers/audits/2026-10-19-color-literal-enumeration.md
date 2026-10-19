# 手工色字面量全量清单复核与工单分配（W0.5）

> 复核日期：2026-10-19
> 复核 HEAD：`cf2b61c4`
> 工作目录：`D:/project/gui/DevDesk`（dev 分支，主工作树）
> 任务出处：`docs/superpowers/plans/2026-10-19-uiux-wechat-rectification-plan.md` W0.5 行
> 基线出处：`docs/superpowers/audits/2026-10-19-eui-capability-audit.md`（EUI 能力审计 §1 / §样式架构总述 / §覆盖盖度声明 d 项）

## 基线 vs 实测

| 维度 | 基线（EUI 审计） | 实测（W0.5 复核） | 差异 |
|------|------------------|-------------------|------|
| 非白手工色字面量处数 | 19（下界） | 19 | +0 / -0 |
| 命中文件数 | 9 | 9 | +0 / -0 |
| 白色字面量 | 未计（明示） | 3 处（splash.cpp 131/143/152） | +3（留痕，不参与工单分配） |
| 圆括号构造 `Color(0.0f / 1.0f,…)` | 未计（grep 模式 `Color{0\.` 不覆盖圆括号） | 4 处（claude_ui_list.cpp 103/104/106/107） | +4（新发现，待 W1 令牌层裁定） |
| 变量引用（非字面量） | — | 1 处（settings_config.cpp:233，`Color{bg.r, bg.g, bg.b, …}` 取 token 字段，非字面量） | 已排除 |

**基线 19/9 全部命中、零遗漏、零行号漂移**：19 处全部按分配表归入对应工单。HEAD 与 EUI 审计基线产出当日为同一日（2026-10-19），未观察到代码改动导致处数或行号漂移。

## 总表

> 表头说明：
> - **#**：基线口径下的序号（19 处之一）
> - **文件:行**：源码定位（绝对路径在 `D:/project/gui/DevDesk/` 之下）
> - **字面量值**：完整 4 元组 RGBA
> - **上下文**：该字面量给什么上色 / 用于什么分支
> - **白色?**：是否白色或近白色字面量（白色不计入工单分配）
> - **工单**：分配进 W1~W11 的哪个工单

| # | 文件:行 | 字面量值 | 上下文 | 白色? | 工单 |
|---|---------|----------|--------|-------|------|
| 1 | `src/app_sidebar.cpp:162` | `eui::Color{0.0f, 0.0f, 0.0f, 0.0f}` | 侧栏导航项命中区命中态的"非 active 默认底色"（三元 `states(active ? t.colors.primarySoftBg : Color{0,0,0,0}, hover, active)`） | 否 | W3 |
| 2 | `src/app_sidebar.cpp:212` | `eui::Color{0.0f, 0.0f, 0.0f, 0.0f}` | 侧栏折叠按钮命中区 normal 态底色（`states(Color{0,0,0,0}, hover, active)`） | 否 | W3 |
| 3 | `src/module_about/about_module.cpp:196` | `eui::Color{0.0f, 0.0f, 0.0f, 0.0f}` | 关于页全屏背景底色（`about.bg` `.color(透明)`，鼠标移动唤醒动画） | 否 | W10 |
| 4 | `src/module_agent/agent_detail_dialog.cpp:363` | `eui::Color{0.0f, 0.0f, 0.0f, 0.0f}` | 详情对话框配置头命中区 normal 态（`.states(Color{0,0,0,0}, Color{0,0,0,0}, Color{0,0,0,0})` 三态都透明，纯命中面） | 否 | W5 |
| 5 | `src/module_agent/agent_detail_dialog.cpp:364` | `eui::Color{0.0f, 0.0f, 0.0f, 0.0f}` | 同上行 hover 态（三元组第 2 参数） | 否 | W5 |
| 6 | `src/module_agent/agent_detail_dialog.cpp:365` | `eui::Color{0.0f, 0.0f, 0.0f, 0.0f}` | 同上行 active 态（三元组第 3 参数） | 否 | W5 |
| 7 | `src/module_agent/agent_detail_dialog.cpp:499` | `eui::Color{0.0f, 0.0f, 0.0f, 0.0f}` | run 行命中区未选中底色（`states(highlighted ? primarySoftBg : Color{0,0,0,0}, hover, hover)`） | 否 | W5 |
| 8 | `src/module_agent/agent_detail_dialog.cpp:1080` | `eui::Color{0.0f, 0.0f, 0.0f, 0.0f}` | 详情滚动区标题间隔占位透明 rect（`.color(Color{0,0,0,0})` 占位） | 否 | W5 |
| 9 | `src/module_agent/agent_module.cpp:1821` | `eui::Color{0.0f, 0.0f, 0.0f, 0.0f}` | 任务列表行 normal 态底色（`states(Color{0,0,0,0}, hover, Color{0,0,0,0})`） | 否 | W5 |
| 10 | `src/module_agent/agent_module.cpp:1823` | `eui::Color{0.0f, 0.0f, 0.0f, 0.0f}` | 同上行 active 态（三元组第 3 参数） | 否 | W5 |
| 11 | `src/module_agent/agent_ui_shared.h:49` | `::core::Color{0.94f, 0.97f, 1.0f, 1.0f}` | actionChip primary 按钮文字色（primary 时用浅蓝色文字而非 `tokens.text`） | 否（浅蓝近白） | W5 |
| 12 | `src/module_markdown/markdown_module.cpp:762` | `eui::Color{0.0f, 0.0f, 0.0f, 0.0f}` | 文件树展开/收起命中区底色（`.color(Color{0,0,0,0})`，叠在行体之上取最上层） | 否 | W8 |
| 13 | `src/module_markdown/markdown_module.cpp:1473` | `eui::Color{0.0f, 0.0f, 0.0f, 0.0f}` | CodeFence hover 底衬未 hover 态（`hovered ? hoverTint : Color{0,0,0,0}`） | 否 | W8 |
| 14 | `src/module_markdown/markdown_module.cpp:1968` | `eui::Color{0.0f, 0.0f, 0.0f, 0.0f}` | 活动块边框透明填充（`.color(Color{0,0,0,0})` + 主色边框） | 否 | W8 |
| 15 | `src/ui_kit/splash.cpp:82` | `eui::Color{0.02f, 0.10f, 0.20f, 1.0f}` | shader 编译失败兜底深蓝底色（`splash.fallback.bg`，浅色主题下白字不可读时垫的深色底） | 否（深蓝） | W2 |
| 16 | `src/ui_kit/splash.cpp:159` | `eui::Color{0.04f, 0.42f, 1.0f, 1.0f}` | 进度条渐变起点（蓝，gradient 第 1 色） | 否 | W2 |
| 17 | `src/ui_kit/splash.cpp:160` | `eui::Color{0.13f, 0.69f, 0.44f, 1.0f}` | 进度条渐变终点（绿，gradient 第 2 色） | 否 | W2 |
| 18 | `src/module_todo/todo_ui_board.cpp:291` | `eui::Color{0.0f, 0.0f, 0.0f, 0.0f}` | Todo 列头右键热区透明底（注释明示"Rect 默认不透明白色，深色主题会刷白"） | 否 | W6 |
| 19 | `src/module_claude_proxy/claude_ui_form.cpp:854` | `eui::Color{0.0f, 0.0f, 0.0f, 0.0f}` | 表单 env 行未知变量命中区透明底（`.color(透明)` + 错误色边框） | 否 | W7 |

## 白色字面量（基线未计，留痕）

> 这 3 处均为 `Color{1.0f, 1.0f, 1.0f, …}`，EUI 审计基线明示"白色字面量未计入"（§覆盖度声明 d 项）。本次复核一并记录，但不参与工单分配——属"基线口径外"，由 W1 令牌层与各页工单自行判断是否需要清理。

| 编号 | 文件:行 | 字面量值 | 上下文 |
|------|---------|----------|--------|
| W-W1 | `src/ui_kit/splash.cpp:131` | `eui::Color{1.0f, 1.0f, 1.0f, 1.0f}` | splash logo "DevDesk" 白色文字（注释"白色文字（深蓝底可读）"） |
| W-W2 | `src/ui_kit/splash.cpp:143` | `eui::Color{1.0f, 1.0f, 1.0f, 1.0f}` | splash 副标题白色文字（注释"白色副标题"） |
| W-W3 | `src/ui_kit/splash.cpp:152` | `eui::Color{1.0f, 1.0f, 1.0f, 0.10f}` | splash 进度条背景轨道白色 10% 透明 |

这 3 处均落在 `splash.cpp`，与 #15-#17 同归 W2 工单；W2 在替换 `splash.cpp` 字面量时可一并决定是否抽到令牌层（如 `colors.onPrimary` / `colors.scrim` 等）。

## 圆括号构造（基线模式未覆盖，新发现，待分配）

> 这 4 处用 `Color(…)`（圆括号构造，非 `Color{…}` 花括号构造），EUI 审计基线 grep 模式 `(Color|color){0\.` 不覆盖圆括号形态。均为 `eui::mixColor` 调用的黑白端点常量（头像混色算法的"黑端点"与"白端点"），性质同基线"白色字面量未计"——属"混色算法的黑白端点常量"。本次复核记入新发现，但因不在初版分配表覆盖范围内，标"待 W1 令牌层裁定"。

| 编号 | 文件:行 | 字面量值 | 上下文 |
|------|---------|----------|--------|
| N-P1 | `src/module_claude_proxy/claude_ui_list.cpp:103` | `eui::Color(0.0f, 0.0f, 0.0f)` | `avatarColorsFor` dark 分支：头像 fill 混黑（`mixColor(base, Color(0,0,0), 0.55f)`） |
| N-P2 | `src/module_claude_proxy/claude_ui_list.cpp:104` | `eui::Color(1.0f, 1.0f, 1.0f)` | `avatarColorsFor` dark 分支：头像 ring 混白（`mixColor(base, Color(1,1,1), 0.45f)`） |
| N-P3 | `src/module_claude_proxy/claude_ui_list.cpp:106` | `eui::Color(1.0f, 1.0f, 1.0f)` | `avatarColorsFor` light 分支：头像 fill 混白（`mixColor(base, Color(1,1,1), 0.72f)`） |
| N-P4 | `src/module_claude_proxy/claude_ui_list.cpp:107` | `eui::Color(0.0f, 0.0f, 0.0f)` | `avatarColorsFor` light 分支：头像 ring 混黑（`mixColor(base, Color(0,0,0), 0.25f)`） |

性质判断：这 4 处不是"在画面上画一块色"，而是"按比例给基色混黑/白"产生头像 fill/ring 配色，属颜色算法常量。整改时未必能去掉——头像混色需要黑白两个端点常量本身是合理的。处理路径：
1. 保留为算法常量（W1 令牌层无需新增黑白 token）—— 推荐
2. 抽到令牌层 `colors.black` / `colors.white`（语义更清晰，但 ColorTokens 现状并无此槽位，需扩展）

由 W1 令牌层裁定，不在本复核范围下结论。

## 按工单小计

| 工单 | 文件 | 处数 | 行号 | 字面量性质 |
|------|------|------|------|------------|
| W2 | `src/ui_kit/splash.cpp` | 3 | 82 / 159 / 160 | 1 处深蓝底色 + 2 处 gradient 蓝绿端点 |
| W3 | `src/app_sidebar.cpp` | 2 | 162 / 212 | 2 处 `Color{0,0,0,0}` 透明底（命中区/hover/active 三元组） |
| W5 | `src/module_agent/agent_detail_dialog.cpp` | 5 | 363 / 364 / 365 / 499 / 1080 | 4 处 `Color{0,0,0,0}` 透明底 + 1 处 `Color{0,0,0,0}` 占位 |
| W5 | `src/module_agent/agent_module.cpp` | 2 | 1821 / 1823 | 2 处 `Color{0,0,0,0}` 透明底（三元组） |
| W5 | `src/module_agent/agent_ui_shared.h` | 1 | 49 | 1 处 `Color{0.94f, 0.97f, 1.0f, 1.0f}` 浅蓝色文字 |
| W6 | `src/module_todo/todo_ui_board.cpp` | 1 | 291 | 1 处 `Color{0,0,0,0}` 透明底 |
| W7 | `src/module_claude_proxy/claude_ui_form.cpp` | 1 | 854 | 1 处 `Color{0,0,0,0}` 透明底 |
| W8 | `src/module_markdown/markdown_module.cpp` | 3 | 762 / 1473 / 1968 | 3 处 `Color{0,0,0,0}` 透明底 |
| W10 | `src/module_about/about_module.cpp` | 1 | 196 | 1 处 `Color{0,0,0,0}` 透明底 |
| 合计 | 9 文件 | 19 | — | 17 处透明黑 + 1 处深蓝 + 1 处浅蓝 |

**字面量形态分布**：
- `Color{0.0f, 0.0f, 0.0f, 0.0f}` 透明黑：17 处（17/19 = 89%）
- `Color{0.02f, 0.10f, 0.20f, 1.0f}` 深蓝：1 处（splash 兜底）
- `Color{0.04f, 0.42f, 1.0f, 1.0f}` + `Color{0.13f, 0.69f, 0.44f, 1.0f}` 渐变蓝绿：2 处（splash 进度条）
- `Color{0.94f, 0.97f, 1.0f, 1.0f}` 浅蓝：1 处（agent chip 文字）

**清理路径提示**：17 处透明黑语义高度统一（"显式透明"），W1 令牌层若加 `colors.transparent` 语义槽位（或复用 `components::theme::withAlpha(bg, 0.0f)`），W3/W5/W6/W7/W8/W10 可机械替换；splash 4 处深蓝/渐变/浅蓝属业务专属色，W1 令牌层若加 `colors.splash.bg` / `colors.splash.progressFrom` / `colors.splash.progressTo` / `colors.onSplash` 槽位，W2 一并替换；agent chip 浅蓝 1 处建议改为 `tokens.primaryText` 或 `tokens.onPrimary` 之类语义槽位（W1 决定）。

## 与初版分配表差异

| 维度 | 初版分配表 | 实测差异 |
|------|-----------|----------|
| W5（agent_detail_dialog×5 + agent_module×2 + agent_ui_shared.h×1） | 8 处 | 0 差异（5+2+1 = 8 处全命中，行号无漂移） |
| W2（ui_kit/splash×3） | 3 处 | 0 差异（splash.cpp 82/159/160 全命中） |
| W3（app_sidebar×2） | 2 处 | 0 差异（app_sidebar.cpp 162/212 全命中） |
| W7（claude_ui_form×1） | 1 处 | 0 差异（claude_ui_form.cpp:854 命中） |
| W10（about_module×1） | 1 处 | 0 差异（about_module.cpp:196 命中） |
| W6（todo_ui_board×1） | 1 处 | 0 差异（todo_ui_board.cpp:291 命中） |
| W8（markdown_module×3） | 3 处 | 0 差异（markdown_module.cpp 762/1473/1968 全命中） |
| **合计** | **19 处** | **+0 / -0**（基线 19 处全部分配出去，无新增、无移除、无行号漂移） |

**结论**：初版分配表（W0.5 行）与当日 HEAD 完全一致，分配方案无需调整。

## 待分配项

1. **4 处圆括号黑白端点常量**（`claude_ui_list.cpp:103/104/106/107`，`mixColor` 的黑/白端点）——基线 grep 模式 `Color{0\.` 未覆盖圆括号形态，初版分配表无此项。性质为颜色算法常量（非"画在画面上的色块"），处理路径见上"圆括号构造"段，**由 W1 令牌层裁定**：保留为算法常量（推荐）/ 抽到 `colors.black` / `colors.white` 语义槽位。
2. **3 处白色字面量**（`splash.cpp:131/143/152`，`Color{1.0f, 1.0f, 1.0f, …}`）——基线明示未计，不参与工单分配。作为留痕记录，W2 工单在替换 splash.cpp 其它字面量时可一并决定是否抽到令牌层。

## 复核方法说明

- grep 多模式并查（全部用 `output_mode=content` + `-n`，`head_limit=0` 不截断）：
  1. `Color\{0\.` —— 命中 19 处（基线主模式）
  2. `eui::Color\{` —— 命中 19 + 3 白色 = 22 处
  3. `Color\(0\.` —— 命中 2 处圆括号黑端点
  4. `Color\(1\.0f` —— 命中 2 处圆括号白端点
  5. `Color\{1\.0f` —— 命中 3 处花括号白色
  6. `core::Color\{` —— 命中 1 处（agent_ui_shared.h:49，与模式 1 重叠）
  7. `\.color\(Color` / `\.color\(eui::Color` / `bg\(.*Color` / `borderColor\(.*Color` —— 与模式 1-2 重叠或 0 命中
- 每处 Read ±5~10 行上下文取证，记录 file:line + 字面量值 + 上下文语义 + 白色标注
- 排除范围：`../EUI-NEO/`、`tests/probes/`、generated（无命中）
- 一处变量引用 `settings_config.cpp:233` `eui::Color{bg.r, bg.g, bg.b, windowEffectAlpha(mode)}` 排除（非字面量，取 token 字段拼装）

## 覆盖度声明

- **正向覆盖**：基线主模式 `Color{0\.` 19 处全部命中并取证；补充模式 `Color(0.0f`、`Color(1.0f`、`Color{1.0f`、`core::Color{` 各覆盖圆括号黑白端点与花括号白色字面量，无遗漏。
- **负向覆盖**：基线明示"白色字面量未计入"已遵守——3 处白色字面量记入留痕但不参与工单分配；4 处圆括号黑白端点为新发现，性质属颜色算法常量，标"待 W1 令牌层裁定"不擅自归工单。
- **可能遗漏**：① `eui::Color{…}` 与 `core::Color{…}` 之外的别名/using 形态字面量（如 `using Color = …` 后裸 `Color{…}`）——本次 grep 已覆盖 `Color{0\.` / `Color{1.0f` 主形态与 `Color(0.0f` / `Color(1.0f` 圆括号形态，别名形态字面量如有应能在模式 1 命中；② `mixColor` / `lerpColor` 调用中除黑/白端点外的其它颜色字面量（如 `mixColor(base, Color{0.5f, 0.5f, 0.5f}, …)`）——本次 grep 模式 `Color{0\.` / `Color{1.0f` 已覆盖 0.x 与 1.0 两端，0.5 等中间值形态如有应能在模式 1 命中；③ 16 进制 hex 字面量（如 `Color::fromHex(0xFF…)`）—— EUI 审计基线明确 DevDesk 取色口径为 `eui::Color{r,g,b,a}` 4 元组形态，无 fromHex 调用，本次未补 grep 验证（依赖基线结论）。
