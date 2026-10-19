> **存档说明**（非原文内容）：来源：只读审计子代理 2026-10-19 产出，原文照录（HTML 转义已还原）。作为《DevDesk 微信风格 UI/UX 整改》工单白名单与测试同步的权威证据；工单引用本文时只许引用行号原文，不许转述。

---

# DevDesk UI/UX 整改前测试影响面审计（只读，2026-10-19）

**路径前缀**：下文所有 `src\…` / `tests\…` 均为绝对路径 `D:\project\gui\DevDesk\` 下的文件。测试框架为自制 mini-dojoint（`REGISTER_TEST` + `DOCTEST_CHECK`，fail-fast），headless compose 后按 **元素 id** `ui.find()` 断言为主。

## 任务一：7 界面渲染文件全量清单

**① 首页 home**（`src\module_home\`）
- `home_module.cpp`（页面主 compose：欢迎区 home.h1/home.date、header 右侧「刷新/设置」按钮、统计卡、快捷按钮行、分布图）
- `home_quota_card.cpp` + `.h`（供应商余量卡：图表/快捷按钮行/滚动模式分支）
- `home_service.cpp` + `.h`（统计/速率数据源）；`CMakeLists.txt`
- 桥/关联：`src\quota_home_bridge.h`、`src\model_slots.h`

**② Markdown 笔记本**（`src\module_markdown\`，全部 22 文件）
- 主 compose：`markdown_module.cpp`（顶栏 segmented「编辑/实时/双栏/预览」、大纲开关、`md.btn.new`、左栏 `md.search`/`md.tree`/树底 `md.tree.new`+`md.tree.newfolder`、标签行 `md.tags.add`+`md.tags.addbtn`、状态栏、新建/重命名/新建文件夹/外部修改各弹窗）
- 树/视图：`md_tree.cpp/.h`、`md_view_state.cpp/.h`、`md_ids.h`
- 编辑/渲染管线：`md_live_edit.cpp/.h`、`md_block_model.cpp/.h`、`md_block_interact.cpp/.h`、`md_sync_map.cpp/.h`、`md_keyboard.cpp/.h`
- 高亮：`md_highlight.cpp/.h`、`md_preview_highlight.cpp/.h`、`md_keyword_highlight.cpp/.h`、`md_line_scanner.cpp/.h`
- 其他：`md_file_open.cpp/.h`、`md_note_git.cpp/.h`、`markdown_service.cpp/.h`、`markdown_tools.cpp/.h`（服务/工具，非渲染）、`markdown_module_test_seam.h`

**③ Todo 看板**（`src\module_todo\`）
- `todo_module.cpp`（页面接线/状态/toast）、`todo_ui_board.cpp/.h`（表头：`todo.h1`/`todo.subtitle` 说明长文本/搜索框/`todo.btn.overdue` 逾期 badge/＋列/＋新建任务；列与卡片渲染）
- `todo_ui_dialogs.cpp`（详情弹窗含标签输入 `todo.dialog.detail.tag.input`、逾期弹窗、新建/重命名弹窗）、`todo_dialogs.h`（弹窗状态结构）
- `todo_ui_common.h`、`todo_ui_logic.h`（纯函数 columnDotColor/overdueDays/filterBySearch）、`todo_view_state.cpp/.h`
- 非渲染：`todo_service.cpp/.h`、`todo_tools.cpp/.h`

**④ Claude 切换器**（`src\module_claude_proxy\`）
- 页面：`claude_module.cpp`（装配/页级快捷键/启停 toast）、`claude_ui_list.cpp/.h`（头部 `claude.h1`/`claude.subtitle`、Profile 卡列表、`ACTIVE` 徽标、空态、右键菜单）
- 面板/弹窗：`claude_ui_detail.cpp/.h`（右栏详情 5 页签 `claude.detail.tabs`：基础信息/模型映射/环境变量/高级配置/最近调用）、`claude_ui_form.cpp/.h`（Profile 表单+共享片段弹窗）、`claude_ui_ccswitch.cpp/.h`、`claude_ui_quota.cpp/.h`、`claude_ui_logs.cpp/.h`
- 状态/逻辑：`claude_ui_state.cpp/.h`、`claude_ui_logic.cpp/.h`；服务层 `claude_service.cpp/.h`、`profile_repository.*`、`provider_config.*`、`call_log_store.*`、`proxy_runtime.*`、`proxy_router.*`、`streaming_proxy.*`、`upstream_dialect.*`、`settings_takeover.*`、`settings_sync.*`、`ccswitch_import.*`、`env_var_hints.h`、`json_scan.h`；`quota\` 子目录 14 对文件

**⑤ 设置页**（`src\module_settings\`）
- `settings_module.cpp`（主 compose：header + `settings.tabs` 顶部 Tab + 卡片注册表 + scrollView）、`settings_ui_logic.cpp/.h`（含 `kSettingTabLabels`「通用/模块/日志/同步」）、`settings_ui_cards.cpp/.h`（各卡渲染）
- GitHub 同步：`github_sync_card.cpp/.h`、`github_sync_config.cpp/.h`、`github_sync_engine.cpp/.h`、`github_sync_logic.cpp/.h`、`github_sync_exit_hook.h`
- 其他：`git_error_dialog.cpp/.h`、`app_tools.cpp/.h`、`settings_config.cpp/.h`、`local_git_probe.h` + `_win.cpp`/`_macos.cpp`、`window_effect_hint.h` + `_win.cpp`/`_macos.cpp`、`github_sync_test_seam.h`

**⑥ 关于页 about**（独立模块 `src\module_about\`，仅 3 文件）
- `about_module.cpp`（hero/应用信息卡/依赖卡/footer/光晕绘制 `about.glow.i.k`+`about.sweep.h`）
- `about_anim.h`（光晕状态机：6 光斑×3 层+扫带，全部常量）；`CMakeLists.txt`

**⑦ 新建任务弹窗**（`src\module_agent\` 内）
- `agent_module.cpp`（`composeTaskDialog`：goal/trigger/cron/slot 模型槽位/iter 迭代上限/destructive 允许破坏性操作/once/note 执行记忆(注入 system prompt)/meta.hint/AI 草稿子弹窗；页顶「+ 新建任务」）+ `agent_module.h`（弹窗状态、删除确认弹窗）
- `agent_ui_shared.h`（共享 UI 常量）、`task_draft.cpp/.h` + `task_draft_job.cpp/.h` + `task_draft_parse.h`（草稿生成/回填弹窗字段）
- 详情弹窗（同弹窗族）：`agent_detail_dialog.cpp`（`config.body.model/iterations/destructive` 字段）；渲染辅助 `message_blocks.cpp/.h`、`agent_time_text.cpp/.h`、`hot_parse.cpp/.h`、`tool_info.cpp/.h`
- 共享层（7 页通用）：`src\ui_kit\`（card/dialog_form/metrics/splash/toast）、`src\app_sidebar.cpp/.h`、`src\app_assembly.cpp/.h`、`src\core\theme.h`

## 任务二：受影响断言清单（按页面分组，★ = 工单改动直接命中）

**首页**
- 「欢迎回来」→「工作台」（`home_module.cpp:343`，id `home.h1`）：tests 全目录 grep 无命中——该文案**无测试保护**。
- 「暂无成功调用」badge（:425）：
  - ★ `tests\test_modules\test_home.cpp:310` `badge->text == "暂无成功调用"`（空态口径逐字断言）
  - ★ `tests\test_modules\test_font_em_layout.cpp:64` `textW("暂无成功调用") <= kHomeStatTextW`（新文案更宽即红）
- 全模块关闭空态（`home_module.cpp:431`）：★ `tests\test_modules\test_module_gating.cpp:244` 含「所有工具模块已关闭」
- 右上角「设置」按钮移除（:379-393，id `home.btn.settings`）：★ `tests\test_modules\test_module_icons.cpp:101` `checkPageIcon(create_home_module, "home.btn.settings.icon", 0xF013)`——删除即红；「刷新」（0xF021）不动则不红；probes 不含 header——**影响面 = 仅 test_module_icons 一条**
- 样式：`test_theme_hierarchy.cpp:214` `home.stat.tokens.bg == t.colors.surface`（token 映射，改色需同步）；`test_ui_modern.cpp:154/:783` id 型

**Markdown 笔记本**
- 顶部快捷键说明长文本（`markdown_module.cpp:381-382`）：
  - ★ `test_markdown.cpp:2480-2484` subtitle 须含 Ctrl+Shift+P / Ctrl+B/I/K / Tab / Enter 且不含「篇笔记」
  - ★ `:2497` 打开笔记后 subtitle 逐字不变（静态契约）
- 视图切换 Tab（:425）：★ `test_markdown.cpp:3584-3588` `md.mode.label.0..3` 逐字等于「编辑/实时/双栏/预览」；`:3593-3636` 四态循环+逆映射点击；`:2983`~`:4405` 各态几何/滚动依赖；`test_theme_hierarchy.cpp:144-148`、`test_ui_modern.cpp:984-996`
- 文件树底部新建按钮上移（:641-658）：24 处 id/onClick 命中（id 不变则移位不红）；★ 会红的是布局契约 `test_markdown.cpp:1811-1814` `md.search` frame 须等于 `md.tree` frame + (12,12)；`test_theme_hierarchy.cpp:153` `md.tree.bg`
- 标签红点加号按钮改内置（:1215-1224 `md.tags.addbtn`）：
  - ★ `:1972-1975` / `:2091-2094` addbtn REQUIRE + onClick 提交；★ `:2629` Esc 后 placeholder 逐字「＋ 标签」；★ `:7310-7313`、`:8030-8036`（addbtn 提交→git 同步计数）
  - 存在性：`:4285`（空态为 null）、`:4298`/`:4322`；输入链 `md.tags.add.hit` onTextInput 多处
  - 「红点加号」全 src 仅 markdown 此一处；todo 详情弹窗标签输入无「＋」按钮（回车提交）
- 样式：★ `test_ui_modern.cpp:1002-1004` `md.preview.bg` `gradient.enabled == true`（动渐变即红）

**Todo 看板**
- 顶部说明长文本（`todo_ui_board.cpp:59-61` `todo.subtitle`）：★ `test_todo_ui.cpp:2145-2147` 含「点击卡片查看详情」
- 「逾期」相关：★ `test_todo_ui.cpp:2502` `rowDays->text == "逾期1天"`（逐字）；★ `:2549/:2565` toast 含「逾期任务加载失败/查询失败」；badge 数字存在性 `:2405/:2452/:2491`；`:2413-2433`（行数据+点击跳详情）；`:2469`「逾期 1 秒仍显示逾期1天」；`:4658`（逾期弹窗行省略号三件套）；服务层文案 `test_todo_tools.cpp:212`（若全仓统一改词需核对）；home 统计卡标题「逾期任务」**无文本断言**
- 「暂无任务」空态：tests 无文本断言——无保护
- 样式：★ `test_todo_ui.cpp:2750-2753`（`todo.col.0.dot` color == #D97757）、`:2805-2812`（columnDotColor「claude-clay」== #D97757、「muted-sage」== #788C5D）；`test_theme_hierarchy.cpp:87-115`（列=background、卡/弹窗=surface、深浅 bg≠surface）

**Claude 切换器**
- 「ACTIVE」→「当前选中」（`claude_ui_list.cpp:210-220` + :203-205 胶囊定宽 48×20、fontSize 9）：
  - ★★ `test_claude_ui.cpp:6171-6172` `active.bg` 宽==48.0f 高==20.0f——4 个全角字 fontSize 9 下放不进 48px，胶囊必加宽，此条必红
  - ★ `:6162-6164` pillProblem：文字居中 + 行高==胶囊高（加宽且保持居中则绿）
  - id 型（改文案本身不红）：`:255-256`、`:2122`、`:2207-2208`、`:2471-2472`
  - toast「已设为激活（ACTIVE）」：tests 无命中——无保护
- 详情面板 5 页签（`claude_ui_detail.cpp:973-982`）：★ `test_claude_ui.cpp:5640-5645` `label.3 == "高级配置"`、`label.4 == "最近调用"`；索引点击依赖 27 处（顺序变即点错 pane）；`test_claude_calls_latency.cpp:129`、`test_ui_modern.cpp:723-726`
- 页顶说明 `claude.subtitle`（`claude_ui_list.cpp:321-331`）：tests 无命中——无保护
- 空态文案：文本无断言；`claude.calls.empty` 仅 id 存在性 5 处
- 样式：`test_theme_hierarchy.cpp:187-189`（profiles/detail bg == background）

**设置页**
- 顶部 Tab（`settings_module.cpp:1019-1032`；标签在 `settings_ui_logic.h:61`）：★ `test_settings.cpp:91` helper 经 `settings.tabs.hit.N` 点击，索引调用 27 处——Tab 增删/重排即全部点错；`:635-645`（hit 存在+各 tab 卡互斥）；标签文本无断言（改名不红）；probes：★ `settings_dropdown_z_probe.cpp:178-180`、`module_toggle_sidebar_probe.cpp:201-202`
- `settings.subtitle`：动态文案断言 `test_settings.cpp:740/:754/:923/:2023/:2071/:2520`——静态句本身无断言，改静态句不红
- 样式：★ `test_ui_modern.cpp:972-975` `settings.body.bg`/`gradient` == nullptr 反证 + appearance 卡存在——若重加主体底板即红

**关于页**
- 渐变光晕移除（`about_anim.h` 全部常量 + `about_module.cpp` 绘制）：
  - ★★ `test_ui_modern.cpp:1236-1410` `uiAboutGlow` 整条用例逐项锁死：元素数==20、`about.ring.0 == nullptr`、直径 240-400、层 alpha 0.15→0.05、呼吸 ±35%、漂移周期 18-26s、扫带 14s/宽 220/18°/alpha 0.05、`about.bg` onTimer==1/30、`about.glow.0.0` 存在——移除装饰后本条从 REQUIRE 起全红
  - 编译依赖：`test_ui_modern.cpp:28` `#include "module_about/about_anim.h"`——删头文件即编译红
  - `test_ui_modern.cpp:1030-1104` `uiAboutLayout`（hero 几何、`about.hero.sub/build/commit == nullptr`、两卡 900 宽贴底）——连带改内容区几何需同步核对
- 其余：无 `test_about.cpp`；`test_module_icons.cpp:65`（about 图标 0xF05A 不受影响）

**新建任务弹窗（module_agent）**
- 弹窗全字段存在契约：★★ `test_agent_ui.cpp:354-407`（`.goal.caption/.trigger/.slot/.iter/.destructive/.cancel/.save/.close` 存在 + `.error==nullptr` + goal 高≥90 + cron 仅定时渲染）——字段增删/移动即红
- 模型槽位/迭代上限/破坏性操作：`:442-447`（slot 选 index 2 + iter 草稿 7 + destructive 点击 → 落库断言）；`:2140-2169`（trigger 弹层不挤动 destructive 行 y、slot.popup z 序/屏内）；`:4429-4502`（标签盒容纳 + 同排不重叠/右缘对齐——caption 改长且盒不加宽即红）；★ `:5261-5265` `meta.hint` 含「受限」+「高危」且长度>20——改提示需保关键词
- 执行记忆 note（placeholder 含 system prompt）：`:3557-3563/:3582/:3601/:3607/:3639-3640/:3655-3659/:3711/:4704-4709`——placeholder 文本本身无逐字断言
- 文案契约联动：`test_agent_ui.cpp:3143-3149`（错误串常量「LLM 配置不可用：无法解析模型槽位「default」…」）；`test_direct_llm_client.cpp:202`；`test_agent_loop.cpp:280`（error 含「迭代上限」）；`tests\live\agent_live.cpp:453-459`（含「破坏性操作」）
- 弹窗探针：★ `agent_dialog_probe.cpp:202-281`（trigger/cron/aichip/draft z 序 + `--shot-task-plain` 开关行帧）+ `agent_e2e_probe.cpp:583`（弹窗打不开即失败）
- 详情弹窗同族字段（`agent_detail_dialog.cpp:1204-1213`）：`test_agent_ui.cpp:825-827/:1220-1250/:1740-1743/:4254-4261/:4414-4415`——工单若同步改详情弹窗需一并更新

**样式断言（跨页汇总）**
- `E67E45` / `FF6B6B`：src 与 tests 全目录**均无命中**（评审文档建议值，非现状）
- `F5F2EC`：★ `tests\test_core\test_theme_accents.cpp:96` `eui::Color lightBg("#F5F2EC")`（浅色底对比度护栏 ≥3.0）——改浅色 surface 需同步并重验；`:105` `#232120` 深色底；`:164-196` 各 accent primary 档 hex 断言
- `tests\test_core\test_theme.cpp:113-150` componentTokens↔Tokens 等值断言（注释列 #FAF9F5/#D97757/#FFFFFF/#1A1816/#232120——**当前主色为 #D97757 橙系**）
- 其余 hex 断言仅 todo 列色点两处；theme token 映射断言 `test_theme_hierarchy.cpp` 4 条；渐变断言 `test_ui_modern.cpp:1004`/`:972-973`

## 兜底扫描：7 页面模块的全部引用测试文件

- **home**：`test_home.cpp`、`test_home_quota_card.cpp`、`test_module_gating.cpp`（:215-320）、`test_module_icons.cpp`（:100-101）、`test_font_em_layout.cpp`、`test_ui_modern.cpp`（:154/:783）、`test_theme_hierarchy.cpp`（:204）、`tests\probes\home_probes.cpp`
- **markdown**：`test_markdown.cpp`、`test_markdown_service.cpp`、`test_markdown_tools.cpp`、`test_md_block_interact.cpp`、`test_md_block_model.cpp`、`test_md_file_open.cpp`、`test_md_highlight.cpp`、`test_md_keyboard.cpp`、`test_md_keyword_highlight.cpp`、`test_md_live_edit.cpp`、`test_md_live_spike.cpp`、`test_md_note_git.cpp`、`test_md_preview_highlight.cpp`、`test_md_sync_map.cpp`、`test_editor_font.cpp`（:68 树行）、`test_view_state.cpp`、`test_module_icons.cpp`、`test_theme_hierarchy.cpp`（:123）、`test_ui_modern.cpp`（:569/:589/:983）
- **todo**：`test_todo_ui.cpp`、`test_todo_service.cpp`、`test_todo_tools.cpp`、`test_view_state.cpp`、`test_module_icons.cpp`、`test_theme_hierarchy.cpp`（:67）、`test_ui_modern.cpp`（:238/:504/:754）
- **claude_proxy**：`test_claude_ui.cpp`、`test_claude_ccswitch_ui.cpp`、`test_claude_quota*.cpp`（7 个）、`test_claude_service.cpp`、`test_claude_tools.cpp`、`test_claude_calls_latency.cpp`、`test_ccswitch_import.cpp`、`test_provider_config.cpp`、`test_settings_sync.cpp`、`test_theme_hierarchy.cpp`（:173）、`test_ui_modern.cpp`（:292/:650/:694）、`test_module_icons.cpp`
- **settings**：`test_settings.cpp`、`test_settings_sync.cpp`、`test_github_sync_card.cpp`、`test_github_sync_logic.cpp`、`test_github_sync_engine.cpp`、`test_local_git_probe.cpp`、`test_editor_font.cpp`、`test_ui_modern.cpp`（:953）、`test_module_icons.cpp`（:105）、`settings_dropdown_z_probe.cpp`、`module_toggle_sidebar_probe.cpp`、`tests\live\github_sync_live.cpp`
- **about**：`test_ui_modern.cpp`（:1030/:1236）、`test_module_icons.cpp`（:65）、`test_settings.cpp`（:2286 注释）、`tests\test_core\test_router.cpp`（PageId::About）、`tests\CMakeLists.txt`（:215/:268/:391）
- **agent/新建任务弹窗**（18 文件）：`test_agent_ui.cpp`、`test_agent_detail_layout.cpp`、`test_agent_draft.cpp`、`test_agent_loop.cpp`、`test_agent_store.cpp`、`test_agent_tools.cpp`、`test_agent_cron.cpp`、`test_agent_scheduler.cpp`、`test_agent_time_text.cpp`、`test_agent_trigger_config.cpp`、`test_direct_llm_client.cpp`、`test_hot_parse.cpp`、`test_hot_tool.cpp`、`test_message_blocks.cpp`、`test_notify_tool.cpp`、`agent_dialog_probe.cpp`、`agent_e2e_probe.cpp`、`tests\live\agent_live.cpp`
- **app 壳（7 页共用）**：`test_app_assembly.cpp`（:312-316 托盘页名逐字「首页/笔记/待办/Claude/设置」）、`test_app_sidebar.cpp`（:232「首页」/:371「智能体」）、`test_module_toggle_bridge.cpp`、`test_menu_beautify.cpp`、`test_tray_dynamic.cpp`、`test_probe_gate_labels.cpp`

## 任务三：现有 UI 测试形态（≤6 行）

1. 断言分三层：①元素 id 存在性与 onClick/onTextInput 接线（headless compose 后 `ui.find`，数量最多）；②文本内容断言（`->text == / contains`，少数且集中）；③几何数值断言（frame 恒值、不重叠、屏内、z 序）与颜色断言（theme token 或 hex 字面量）。
2. 无截图像素对比测试；截图仅作人工审阅证据（probe `--screenshot` 进程内 glReadPixels 落 PNG，不进断言）。
3. 真窗口探针已存在：`tests\probes\` 6 个可执行（home_probes、screenshot_probe、settings_dropdown_z_probe、module_toggle_sidebar_probe、agent_dialog_probe、agent_e2e_probe）由 `tests\CMakeLists.txt:236-320` GLOB 注册进 `ctest -L probe`；另有 `quota_live_probe`（live）与 `tests\live\` 2 个。
4. 入口：单测 build\ 下 ctest 全量；probe 需单独 `ctest -L probe`（`scripts/build-test-run.sh` 不跑 probe）；CI 设 `DEVDESK_BUILD_PROBES=0` 不编 probe。
5. headless 几何单测与 probe 的分工契约写在 `tests\test_modules\test_agent_ui.cpp:1-40` 文件头（同类文件同款）。

## 覆盖度声明

**已用搜索词**（`tests\` 全目录含 probes/live；`src\` 用于出处确认）：欢迎回来 / 欢迎 / 工作台 / ACTIVE / 暂无 / 逾期 / 逾期任务 / 点击卡片查看详情 / todo.subtitle / md.subtitle / settings.subtitle / claude.subtitle / 切换视图 / md.tree.new / md.tree.newfolder / md.tags.add / md.tags.addbtn / md.search / md.mode.label / 双栏 / home.btn / home.header / kHeaderActionsW / AboutAnim / GlowBlob / module_about / E67E45 / FF6B6B / F5F2EC / `#[0-9A-Fa-f]{6}` / 模型槽位 / 迭代上限 / 允许破坏性 / 破坏性操作 / system prompt / agent.dialog / 下次触发 / 已设为激活 / settings.tabs / claude.detail.tabs / config.body / calls.empty / 标签输入 / subtitle（src）/ 快捷键（src）/ create_*_module / AgentModule 等。
**已查范围**：`tests\` 全部 .cpp（test_modules 71 + test_core 19 + test_platform 13 + probes 7 + live 2）与 `tests\CMakeLists.txt`；`src\` 7 个 module_* 目录 ls 全量；关键断言段落逐行读毕（各页 ★/★★ 行号均实读）。
**可能遗漏与原因**：① 工单清单之外的文案/控件不在词表内，范围扩充需另行扫描；② `E67E45`/`FF6B6B` src 与 tests 均无命中，实值定义在 `src\core\theme.h`（theme 全量断言面以 test_theme*.cpp 四文件为准，`test_theme_fontscale.cpp` 仅 grep 确认）；③ 期望值经变量/常量间接引入的文案断言无法逐词穷举，已用弹窗/控件 id 前缀补查主要面；④ `scripts\` 守卫脚本断言对象为结构/ctest 标签，不含页面文案（经间接确认）；⑤ 页顶说明长文本已用 `\.text\("[^"]{28,}` + subtitle 两条 src 全扫穷举出全部 4 处并给出各自测试命中。
