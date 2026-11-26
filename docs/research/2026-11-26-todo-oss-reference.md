# DevDesk 待办模块：外部参照（业界口径 / 开源参照点 / 框架能力可行性）

- 日期：2026-11-26｜基线：本地 dev @ 1f70a5ce；EUI-NEO fork main @ a59ed12（2026-11-24）。
- 性质：只读调研的外部事实面。由 R4 底稿整理成正式稿，并入 R9 对框架能力的补证。未构建、未实机；外部事实全部取自 R4 于 2026-11-26 的检索，本稿没有新增检索。
- 底稿：`evidence/20261125-todo-research/R4-external-synergy.md`（主）、`R9-cross-module-consistency.md`（补证）；配套主报告 `docs/research/2026-11-26-todo-module-research.md`。
- 修订：初稿后经独立审计（`evidence/20261125-todo-research/R11-audit.md`）与主会话复核修订——参照仓库数由 14 更正为 13、一处仓名拼写更正、补回两条未覆盖、来源分级更正一处。
- 标注：结论级【已确认】= 本轮实测 / 官方页正文精读；【待验证】= 有线索未读实；【推测】= 推断。来源级：精读 = 抓到正文并摘录；扫描 = 仅搜索摘要 / 目录 / 仓库元数据。
- 口径提醒：外部引用经抓取工具转述摘录。全部参照只作设计参照，不建议引入依赖（Windows 单 exe 零外部 DLL；五维预算先问「能不能不新增」）。
- 时效：提交、许可证均为 2026-11-26 实查；本仓判断对应上述基线，有新提交后须复核标注。

## 结论先行

1. 【已确认】重复任务的两种锚点（从原截止日算 / 从完成日算）在 Todoist、Vikunja、Things 3、Obsidian Tasks 四家官方文档一致；Vikunja「完成后同一条任务前移」比「模板 + 副本」数据模型简单。
2. 【已确认】自然语言识别的「退出路径」是标配：Microsoft To Do 退格取消与设置开关、Todoist 点词还原、滴答清单点击已识别时间取消、Vikunja 引号跳过解析。
3. 【已确认】AI 接入的通行形态是 MCP 工具 + 权限分档（Vikunja 三档），与本仓工具注册表 + 镜像的方向一致；本仓 6 个待办工具早已对外，缺的是行为一致性（见主报告 §3.1）。
4. 【已确认】开源参照点 commit 级核实 13 个仓（17 个可用参照行，另 Focalboard 1 行已排除）；Focalboard 仓库已空不可用；C/C++ 自然语言日期解析库、撤销与模板的 commit 级参照本轮查不到；键盘快捷键只到仓库级参照，未定位到实现文件。
5. 【已确认 + 待验证】EUI-NEO fork 已有日期滚轮（待办在用）、带按钮 toast、tooltip、右键菜单、键盘事件；另有 timePicker（DevDesk 零使用、未做主题适配）；全局快捷键、多选、分隔条没有。

## 一、业界功能全集与交互口径（Q1，中英各一轮）

### 1.1 产品做法（15 个关注面）

| # | 关注面 | 产品怎么做（来源 · 级别） |
|---|---|---|
| 1 | 快速捕获 / 全局快捷键 | Things 3：Quick Entry = Ctrl Space，系统级，可改键；Todoist：Quick Add 按 Q，桌面端可设全局快捷键；滴答清单：全局添加 Ctrl / Cmd + Alt + A，另有悬浮球（ Things 快捷键页、Todoist Quick Add 页、滴答新手指南 · 精读；TickTick 功能页 · 精读） |
| 2 | 自然语言输入 | Things 3：在 When 弹层和截止日选择器里输入 tomorrow / next week / 10-3 / tues 8am 等，支持含中文在内 8 种语言；Todoist Quick Add：标题里写 tomorrow at 4 PM、every other Tuesday，{march 30} 截止日、p1–p3 优先级、!14:00 提醒，点误识别词可还原；Microsoft To Do：tomorrow / Apr 27 / Friday 得到期日，daily 得重复，对高亮词按退格即取消，设置可整体关闭，Windows 端仅英文；Vikunja Quick Add Magic：!1–!5 优先级、every day、at 5pm，Shift+Enter 多行建多任务，缩进建子任务，标题加引号跳过解析；滴答清单：智能识别时间，点已识别时间可取消（均精读；TickTick 英文帮助中心 · 扫描） |
| 3 | 重复任务 | Todoist：every = 从原日期算下一次，every! = 从完成日算；Vikunja：Default / Monthly / From completion date，完成后不新建任务而是同一条任务前移，不支持「每两周的周二」这类规则；Things 3：「模板 + 副本」，可 After Completion 或按天 / 周 / 月 / 年，副本不能再设重复；Obsidian Tasks：完成时新建一条任务，when done 则以完成日为基准；Microsoft To Do：标题里写 daily / every Monday；滴答清单：农历每年与艾宾浩斯重复（均精读） |
| 4 | 提醒与通知渠道 | Todoist：automatic / custom / recurring / location 四类，相对提醒必须任务带时间，可关自动提醒；Vikunja：每任务多个提醒，分绝对与相对，触发时界面内通知，可选邮件；TickTick：多个提醒时间、Constant Reminder 持续响到完成；滴答清单：未处理的提醒重复到完成 / 延后 / 处理为止；Things 3：提醒只能挂开始日（均精读） |
| 5 | 子任务 / 清单项 | 滴答清单：任务详情「...」→「添加子任务」；Apple 提醒事项：Go deeper with subtasks（目录级）；Things 3：待办内有清单项，但重复模板内的清单项不能勾选；Vikunja：subtask / parent 关系，也可在 Quick Add 里缩进建（Vikunja、Things、滴答 · 精读；Apple · 扫描；Microsoft To Do 的 steps 未核实） |
| 6 | 标签与筛选 / 智能清单 | Things 3：标签可设快捷键；TickTick：smart filters；Apple 提醒事项：Smart Lists（目录级）；Vikunja：Filters 与 Saved Filters；滴答清单：搜索结果按任务 / 清单 / 标签筛选（Things、TickTick、滴答 · 精读；Apple、Vikunja · 扫描）。本仓标签已裁决全删（clue#365），标签相关只作对标背景，不构成本仓候选 |
| 7 | 手动排序与拖拽的键盘替代 | Things 3：⇧⌘M 移到其他清单；⌘T 今天、⌘E 今晚、⌘R 随时、⌘O 将来；Ctrl ] 开始日 +1 天、Ctrl ⇧ ] +1 周；截止日 Ctrl . / , ±1 天、Ctrl ⇧ . / , ±1 周（精读）；Obsidian Tasks：纯文本，改顺序就是编辑文本 |
| 8 | 今日 / 我的一天 | Microsoft To Do：My Day 每晚重置，未完成任务回到原清单并进入次日建议；Things 3：Today 与 This Evening 两档（精读） |
| 9 | 回顾与统计 | 滴答清单：成就值，按创建、完成、专注、打卡累计；其余产品官方统计页未核实（仅滴答精读） |
| 10 | 模板 | 滴答清单：把任务、笔记保存为模板，新建时「从模板中添加」；Vikunja 的 Quick Add 与导入导出页未提模板（仅滴答精读） |
| 11 | 导入导出 | Vikunja：导出 ZIP；导入支持 Vikunja ZIP、CSV（至少一列映射到标题）、TickTick、Todoist、Trello、Microsoft To Do、WeKan JSON、Planka，另可 CalDAV 同步；滴答清单：Web 端备份与还原；Things 3：有「Importing Data From Other Apps」文章（目录级）（Vikunja、滴答 · 精读；Things · 扫描） |
| 12 | 撤销 | 本轮未抓到任何官方撤销说明，未覆盖 |
| 13 | 键盘快捷键全集 | Things 3（Mac，精读）：⌘N 新待办、⌥⌘N 新项目、⇧⌘N 新标题、⌘K 完成、⌥⌘K 取消、⌘D 复制、⌘S 显示 When、⇧⌘D 加截止日、⇧⌘T 编辑标签；滴答清单：Shift+回车 加描述、Command+回车 创建，悬浮窗快捷键可自定义 |
| 14 | 与笔记双向链接 | 未覆盖：没抓到哪个产品把任务与笔记双向互链的官方说明；Vikunja 只有任务之间的关系表 |
| 15 | AI 能力 | 滴答清单：AI 助手用自然语言建任务、拆解项目、整理清单、复盘总结，并对外提供 MCP 工具；Todoist Quick Add 可把文本、图片、文档一次转成多条任务；Vikunja：MCP（v2.7.0+），令牌鉴权，权限三档 Read only / Typed tools read + write（默认）/ Full access（滴答、Todoist Quick Add、Vikunja · 精读；Apple · 扫描；Todoist AI Assistant 详情页未核实） |

### 1.2 对本仓最有用的三条口径

- 重复任务的两种锚点（原截止日 / 完成日）是四家一致的已确认行业口径。
- 自然语言识别的退出路径是标配：四家都给了「不识别」的出口。
- AI 接入的通行形态是 MCP 工具 + 权限分档，与本仓方向一致。

## 二、开源参照点（Q2）

核对方式：许可证、默认分支、最近提交、文件路径均为 2026-11-26 `gh api` 实测。核对 URL：`https://github.com/{仓库}/blob/{head}/{路径}`。

| 功能点 | 仓库 | 许可证 | 分支 @ head（日期） | 关键文件 | 可迁移性 |
|---|---|---|---|---|---|
| 重复规则模型（C++/Qt） | [KDE/kcalendarcore](https://github.com/KDE/kcalendarcore) | LICENSES 目录含 BSD-3-Clause、LGPL-2.0-or-later、LGPL-3.0-or-later；文件级 SPDX 待核对 | master @ 794c9e02b0d5（2026-11-23） | `src/recurrencerule.h`、`src/recurrencerule.cpp`、`src/recurrence.h`、`src/recurrence.cpp`、`src/todo.h`、`src/todo.cpp`、`src/icalformat.cpp` | C++，规则建模思路可借鉴；LGPL 不宜静态链入 |
| 重复展开迭代（C） | [libical/libical](https://github.com/libical/libical) | 双许可 MPL-2.0 / LGPL-2.1-only（LICENSE.txt 原文） | 4.1 @ dd929595d87e（2026-11-23） | `src/libical/icalrecur.c`、`icalparser.c`、`icalproperty.c`、`icalcomponent.c`、`icaltime.c` | C，完整 RRULE；本仓最小闭环用不到全量 |
| 完成后推进 + 关系表 + 逾期提醒（Go） | [go-vikunja/vikunja](https://github.com/go-vikunja/vikunja) | AGPL-3.0（GitHub 识别） | main @ eb3cd6bad2a1（2026-11-25） | `pkg/models/tasks.go`、`task_relation.go`、`task_reminder.go`、`task_overdue_reminder.go`、`kanban_task_bucket.go`、`saved_filters.go` | 逻辑可借鉴，AGPL 不得拷贝代码；`task_overdue_reminder.go` 可供 #384 参考 |
| 快速添加解析器（TS） | go-vikunja/vikunja | 同上 | 同上 | `frontend/src/modules/quickAddMagic/quickAddMagic.ts`、`dateParser.ts`、`repeatParser.ts`、`priorityParser.ts`、`prefixParser.ts`、`numberWords.ts`、`textCleanup.ts`、`quickAddMagic.test.ts` | 按日期 / 重复 / 优先级 / 前缀拆解析器的架构可照搬 |
| 看板拖拽（Vue） | go-vikunja/vikunja | 同上 | 同上 | `frontend/src/components/project/views/ProjectKanban.vue`、`frontend/src/components/tasks/partials/KanbanCard.vue`、`pkg/models/kanban.go` | 本仓拖拽已交付，只作行为对照 |
| MCP 工具暴露（Go） | go-vikunja/vikunja | 同上 | 同上 | `pkg/modules/mcp/mcp.go`、`catalog.go`、`tools.go`、`spec.go`、`exposure.go`、`docs.go`、`filter_syntax.md` | 工具目录 / 暴露 / 权限三档的设计参照 |
| 导入格式（Go） | go-vikunja/vikunja | 同上 | 同上 | `pkg/modules/migration/csv/csv.go`、`csv/handler.go`、`ticktick/ticktick.go`、`todoist/todoist.go`、`microsoft-todo/microsoft_todo.go`、`create_from_structure.go`、`item_limit.go` | 各厂商字段映射清单；本仓按总纲默认不做兼容 |
| 自然语言日期解析（TS） | [wanasit/chrono](https://github.com/wanasit/chrono) | MIT | master @ 59d0b2b3004a（2026-11-03） | `src/chrono.ts`、`src/results.ts`、`src/types.ts`、`src/locales/zh/index.ts`（18 个语种目录，含 zh、ja、ko） | 解析器 + 各语种目录的分层架构可照搬 |
| 重复规则 + 自然语言（TS） | [jkbrzt/rrule](https://github.com/jkbrzt/rrule)（原 jakubroztocil/rrule） | LICENCE 为 MIT 式文本（GitHub 标 NOASSERTION） | master @ 1626fd86ea6c（2026-11-23） | `src/rrule.ts`、`src/rrulestr.ts`、`src/nlp/parsetext.ts`、`src/nlp/index.ts`、`src/dateutil.ts` | RRULE 字符串解析参照 |
| 完成后重复的表单映射（Angular） | [super-productivity/super-productivity](https://github.com/super-productivity/super-productivity)（原 johannesjo） | MIT | master @ 4225db139642（2026-11-24） | `src/app/features/task-repeat-cfg/dialog-edit-task-repeat-cfg/after-completion-form-mapping.ts`、`build-repeat-quick-setting-options.ts`、`src/app/features/worklog/worklog.service.ts`、`src/app/features/reminder/reminder.service.ts`、`src/app/features/planner/planner-today-sync.spec.ts` | 完成后重复的设置项最小集 |
| C++/Qt 待办应用 | [KDE/zanshin](https://github.com/KDE/zanshin) | LICENSES 目录含 GPL-2.0-only、GPL-3.0-only、LGPL-2.0-or-later、MIT、CC0-1.0、LicenseRef-KDE-Accepted-GPL；文件级 SPDX 待核对 | master @ ae03a4098313（2026-11-24） | `src/domain/task.h`、`src/domain/task.cpp`、`src/presentation/workdaypagemodel.cpp`、`src/presentation/runningtaskmodel.cpp`、`src/presentation/taskfilterproxymodel.cpp`、`src/utils/datetime.cpp` | 唯一查到的 C++ 生产级待办应用；workday 页模型可供今日视图参考；未定位到快捷键实现文件，对键盘快捷键只算仓库级参照 |
| 跨平台全局快捷键（C++/Qt） | [Skycoder42/QHotkey](https://github.com/Skycoder42/QHotkey) | BSD-3-Clause | master @ 4e3a244d87f1（2025-09-30） | `QHotkey/qhotkey.h`、`qhotkey.cpp`、`qhotkey_p.h`、`qhotkey_mac.cpp`、`qhotkey_win.cpp` | Windows 与 macOS 各有实现文件，登记 / 注销逻辑可迁移到 EUI-NEO 平台层 |
| 全局键盘 hook（C） | [kwhat/libuiohook](https://github.com/kwhat/libuiohook) | LGPL-3.0（COPYING.LESSER.md 为 LGPLv3；COPYING.md 为 GPLv3） | 1.2 @ 1ebe3b43193d（2026-11-14） | `include/uiohook.h`、`src/darwin/input_hook.c`、`src/windows/input_hook.c` | 监听型 hook，对「热键」场景偏重 |
| CSV 解析（C++） | [vincentlaucsb/csv-parser](https://github.com/vincentlaucsb/csv-parser) | MIT | master @ 488581faf09a（2026-11-12） | `single_include/csv.hpp` | 单头文件；本仓导出只需写 CSV，可自写 |
| CSV 导入导出 + 子任务表（PHP） | [kanboard/kanboard](https://github.com/kanboard/kanboard) | MIT | main @ 8bce30653398（2026-11-25） | `app/Export/TaskExport.php`、`app/Export/SubtaskExport.php`、`app/Import/TaskImport.php`、`app/Model/SubtaskModel.php`、`TaskPositionModel.php`、`TaskDuplicationModel.php`、`TaskLinkModel.php` | 独立子任务表与位置表的结构参照 |
| 子任务清单 + 到期提醒（Node） | [wekan/wekan](https://github.com/wekan/wekan) | MIT | main @ ffe26d7240f4（2026-11-25） | `models/checklists.js`、`models/cards.js`、`models/lists.js`、`server/methods/boardDueReminders.js`、`docs/Features/Board-Sidebar/Board-Settings/Import-Export/iCalendar/ICS-Calendar-Import.md` | 清单项模型；到期提醒的服务端实现 |
| 完成时新建任务的规则（文档源） | [obsidian-tasks-group/obsidian-tasks](https://github.com/obsidian-tasks-group/obsidian-tasks) | MIT | main @ b91ad5325d39（2026-11-25） | `docs/Getting Started/Recurring Tasks.md` | 规则语义的文字定义 |
| 看板拖拽状态机 / 撤销栈 | focalboard/focalboard | — | 最近提交 f66853df321b（2025-02-24） | 无：仓库根目录现仅剩一个默认 profile README | 不可用，已排除 |

可用参照仓库共 13 个（go-vikunja/vikunja 占 5 行；表内共 17 个可用参照行，另 Focalboard 1 行已排除）。表里的 head 是 2026-11-26 实查时点的值，此后会有新提交（主会话复核时 obsidian-tasks 的 main 已前进）。查不到的：C/C++ 自然语言日期解析库（三组仓库搜索零命中，搜索工具限流）；撤销栈的 commit 级参照；模板的 commit 级参照；键盘快捷键的文件级参照（zanshin 只到仓库级，列出的关键文件与快捷键无关）。

## 三、EUI-NEO 能力可行性（Q4；并入 R9 补证）

| 能力 | 结论 | 依据 | 备注 |
|---|---|---|---|
| 拖拽排序 / 跨列 | 已有 | `docs/EUI-NEO-CAPABILITIES.md:72`（mouseArea 的 onDragStart / onDrag / onDragEnd）；待办已用 | |
| 右键菜单 | 已有 | 同文件 `:102`、`:470-477`；`EUI-NEO/components/contextmenu.h`、`styledmenu.h` | 支持嵌套子菜单 |
| 窗口内键盘快捷键 | 已有 | 同文件 `:452-468`、`:633`；分发点 `main.cpp:1201-1212` → `core/router.h:45-46`（R9 面 3） | 全应用唯一全局快捷键 = 修饰键 + B 切侧栏；没有快捷键表 |
| 全局快捷键 | 需补框架 | 同文件 `:36`、`:468`；fork 平台层无 hotkey 文件 | 参照 Skycoder42/QHotkey；所用系统接口名称未核实 |
| 日期选择器 | 已有（盘点文档滞后） | `EUI-NEO/components/datepicker.h:48`、`:77`，滚轮式，回调年月日三个整数；待办与上线模块在用（`todo_ui_dialogs.cpp:17`、`:525`；`release_dialogs.cpp:497`） | 更正 R4「待办对日期选择器引用为 0」；`core/component_theme.h:42` 的「未使用」注释已过时 |
| 时间（时:分）选择器 | 件在位，未接入 | `EUI-NEO/components/timepicker.h:440`（入口）；`.time(hour, minute)` `:61-63`、`.minuteStep(1~30)` `:66`、`.onChange` `:75`、`.open` / `.bindOpen` `:53-54`；三列 12 小时制 `:197-204` | 更正 R4「没查到」；DevDesk 零使用、未做主题适配、未做 POC；定时模块在用做法是文本框（`schedule_ui_edit.cpp:825-839`） |
| 行内编辑 | 部分 | `docs/EUI-NEO-CAPABILITIES.md:79`（input 已有 value / multiline / onChange / onEnter / onCancel） | 卡内行内编辑的焦点与提交交互需自建 |
| 带按钮的 toast | 已有构件 | `EUI-NEO/components/toast.h:65`（duration）、`:67`（action）；`EUI-NEO/tests/unit/toast_action.cpp` | 单实例无队列（能力盘点 `:44`）；全仓带按钮 toast 仅 1 处（`markdown_module.cpp:545`，R9 面 5） |
| tooltip | 已有 | 能力盘点 `:101`；`EUI-NEO/components/tooltip.h` | |
| 多选 / 批量操作 | 无 | 能力盘点零命中 | |
| 列宽拖拽 / 分隔条 | 无 | 能力盘点零命中（splitter / 列宽） | |
| 虚拟化长列表 | 已有 | 能力盘点 `:114`、`:115`；`EUI-NEO/components/virtuallist.h:43`、`virtualmasonry.h:46`（R9 面 8） | 待办用手写定高窗口；Claude 配置用框架 virtualList；笔记照抄待办算法一份 |
| 图表 | 已有 | 能力盘点 `:20`（lineChart / barChart / pieChart） | |
| 托盘 | 已有（受限） | 能力盘点 `:355-366`、`:632`；`EUI-NEO/include/eui/app.h:123-131`；`src/core/tray_menu_spec.h`；`src/main.cpp:480-525` 壁纸托盘桥 | 托盘徽标【待验证】：`EUI-NEO/3rd/tray/tray.h` 搜 badge 零命中，未读 tray_bridge.c |
| 独立置顶浮窗 / 第二顶层窗口 | 未核对 | — | 未覆盖 |
| 标准弹窗壳（R9 补证） | DevDesk 自有件，非框架 | `ui_kit/dialog_shell.h:39-74`、`dialog_shell.cpp:70`；36 处在用 | 旧 `components::dialog` 仍 9 处；待办 3 比 2（R9 面 6） |
| 减弱动态（R9 补证） | 平台接口在位，DevDesk 仅首页接入 | `platform/reduced_motion.h:11-17`；`home_anim_impl.cpp:46-51`、`:66`；EUI-NEO 与 `src/ui_kit` 零命中 | ui_kit 不依赖 platform，先例是 app 层桥传参（`home_anim_bridge.h:13-18`） |
| Esc 弹窗栈 / 搜索防抖（R9 补证） | 无公共实现 | EUI-NEO components 里只有 `input.h:92-94`、`:387-388` 与 styledmenu.h 处理 Esc；各页手写关最上层 | 框架无 Tab 焦点循环（能力盘点 `:45`、`:428`）；待办自己已实现「Esc 先关最上层弹窗」 |

待办模块对框架构件的引用计数（R4，仅供参考）：toast 0、styledMenu 0、dropdown 0、virtualList 0、tooltip 1、contextMenu 1、input 7、dialog 4；待办的提示是模块自绘，不经框架 toast。

## 四、可供池 #384（待办到期接通知）引用的口径

### 4.1 外部口径（已确认）

1. Todoist 的相对提醒以任务带时间为前提，区分「自动提醒」与「自定义提醒」两档，可整体关闭。
2. 滴答清单与 TickTick 都有「持续提醒」：未处理的提醒重复到任务完成、延后或处理为止——对应 #384 待裁决①「是否重复打扰」。
3. Vikunja 另有每日一次的逾期汇总邮件，是「逐条通知」之外的另一种形态；仓库内有 `pkg/models/task_overdue_reminder.go` 可作实现对照。
4. Things 3 的提醒只能挂开始日、不能挂截止日；Vikunja 每任务可挂多个提醒（绝对 / 相对）。

### 4.2 仓内事实

| 面 | 事实 | 依据 | 确认度 |
|---|---|---|---|
| 系统通知 | 已有，三平台文件齐 | `src/platform/notification.h:22-77`；`notification_macos.mm` / `_win.cpp` / `_posix.cpp`；`src/module_schedule/channels/os_notification.cpp` | 已确认 |
| 定时模块四渠道 | 已有 | `src/module_schedule/channels/`：fullscreen（264 行）、os_notification（51）、silent（29）、toast（69）、wecom（458） | 已确认 |
| schedule 侧逾期扫描器 | 无 | 池 #384 正文自述；模块内未按 overdue 关键词复核 | 待验证 |
| 托盘 / 开机自启 | 已有 | `src/platform/tray_icon.h` + 三平台实现；`.zcode/cheatsheets/platform-file-map.md:43` | 已确认 |
| 点通知跳页 | 无实现：通知无动作字段，点击只写日志；定时模块准备好的跳转地址被两个渠道丢弃 | `platform/notification.h:41-47`；`notification_macos.mm:80-83`；`action_runner.cpp:339`；`channels/os_notification.cpp:9`、`:34`；`channels/toast_channel.cpp:11` | 已确认（R9 面 10） |
| 页间跳转原语 | 已有 | `Router::navigateTo(PageId, payload)`：`core/router.h:25`、`core/router.cpp:43-51`；待办已有 payload 约定 `action:new_task`（`todo_module.cpp:719-734`） | 已确认 |
| 装配层只读源适配器 | 已有 | `src/main.cpp:400-432`（ReleaseScheduleProviderAdapter）；`schedule_provider.h:22-30`，`executable` 默认 false 即只读展示 | 已确认 |
| 模块互不依赖 | todo 不能 include schedule 头文件；桥只能放 main 装配层或 core 层 | `src/main.cpp:38-40`；AGENTS.md §六 | 已确认 |
| 时:分 录入 | 待办截止只到天；框架 timePicker 未接入 | 见 §三 | 已确认 / 接入可行性待验证 |
| GitHub Sync 对待办的覆盖 | 整库，无待办独立格式导入导出 | `github_sync_engine.h:73`；`github_sync_logic.h:92` | 已确认 |
| 笔记 ↔ 待办 | 待办 → 笔记单向（只存路径键）；反向未读 | `todo_service.h:39-41` | 正向已确认 / 反向待验证 |
| 现存等价提醒入口 | 出厂模板「逾期待办巡检」已于 11-22 删除（`2894adf5`）；现存三条：系统晨检模板（逾期是五项之一）、AI 起草示例、端到端探针 | `agent_task_templates.h:47-56`；`task_draft.cpp:244-248`；`tests/probes/agent_e2e_probe.cpp:76-79` | 已确认（R8 ②） |

「点提醒直达任务」需要自建三段：投递渠道带载荷、app 层消费、待办扩展 payload 解析（R9 面 10）。

## 五、对 R4 底稿「本仓现状」判断的更正（不传播被否决的说法）

| R4 原说法 | 更正 | 依据 |
|---|---|---|
| §3.1「智能体 ↔ todo 工具：部分（只读）」「写工具零命中」；§6.1 的 K3；§6.5 仓内第 10 条 | 6 个工具早已存在（查 / 建 / 改 / 移 / 完成 / 删），智能体面与 MCP 面同源；「智能体写待办工具」作废 | `todo_tools.cpp`（R1 ③）；R8 ①④；主会话核实 |
| §4「todo 模块对 datepicker 的直接引用计数 0」「截止日用什么控件录入未确认」 | 用的是框架日期滚轮，外包一层待办面板（`drawDatePicker`），只有年月日 | `todo_ui_dialogs.cpp:17`、`:495-565`；主会话核实 |
| §4「时间（时:分）选择器：待验证 / 没查到」 | 框架有 `components/timepicker.h`（`:440`），DevDesk 零使用、未做主题适配 | R9 面 1 |
| K5 的价值描述含「标签」；K10 称「现有搜索 + 标签已覆盖大部分场景」 | 标签已裁决全删（clue#365），相关候选不含标签维度 | R3 ②#20 |
| K3 的「写操作要确认门」 | 对象改为既有工具：MCP 面调删除不经应用内确认，是否另有总闸未核 | R8 ④ |
| §6.3 自检称「A 档三项参照点齐全」 | K3 作废后 R4 原列 A 档只剩 K1、K2；其中 K1 的参照经审计标黄（见下一行） | 本稿 §六 |
| §5 K1「生产级参考 过：KDE/zanshin」 | 只有仓库级参照，未定位到快捷键实现文件；第三问标黄 | 审计 S2；本稿 §二 zanshin 行 |
| §5 K4 / K6「落地 过」 | K4 卡片变高未做 POC、联动未定义；K6 保存入口未核；第一问标黄 | 审计 S3；主报告 §4.1.2 |
| §2「可用参照仓库共 14 个」 | 13 个仓（vikunja 占 5 行，表内 17 个可用参照行） | 审计 S1 |

## 六、闭环三问自检

| 三问 | 回答 |
|---|---|
| 能否落地 | 一处黄。R4 原判过门的 5 项里：K1 / K2 / K10 零平台代码、落地条件齐；K4 卡片变高未做 POC、子任务与归档 / 完成的联动未定义（黄）；K6 保存入口未核（黄）。K11 明确第一问不过并给出 S 级替代（托盘菜单入口，归 #382 C 档） |
| 100% 覆盖 | 是。15 个关注面里 13 个有精读来源（回顾统计、模板各只有滴答一家），「撤销」与「与笔记双向链接」两项未覆盖并已明示；八类基建逐项标了状态；框架能力点名的八类逐项有结论，另补五类与 R9 四类。K1 的覆盖缺口（键盘选卡）已写明由谁补 |
| 有生产级参考 | 一处黄。R4 原判过门 5 项里 K2 / K4 / K6 / K10 有仓库 + head + 文件路径级参照；K1 只有仓库级参照（黄）。带黄标的 K5 / K7 / K8 与不过的 K9 已在主报告 §4.2 写明，未计入推荐 |

## 七、产出标准硬清单自检

1. 落地性：一处黄。K1 / K2 / K10 含落地条件清单；K4（未做 POC）、K6（保存入口未核）第一问标黄，未计入「三问全过」。
2. 覆盖率：通过（带缺口）。覆盖对照见 §一、§三、§六；缺口逐条写明，含补回的两条未覆盖。
3. 生产级参照点：一处黄。13 个仓均到 repo + head + 路径 + 许可证；查不到的四类已明示；K1 的参照只到仓库级，已标黄。
4. 结论分级：通过（修订后）。关键结论标 已确认 / 待验证 / 推测，来源标 精读 / 扫描。
5. 三问门：通过（修订后口径）。三问全过且无黄标的只有 K2、K10；K1 / K4 / K6 标黄；淘汰项败因见主报告 §4.4。

## 八、未覆盖项与遗留

外部（联网工具限流：搜索 10 次中 6 次失败，网页抓取 19 次中 6 次失败或空页）：
- Todoist AI Assistant 详情页、Todoist 统计类功能。
- Apple 提醒事项的重复、提醒、智能清单、Apple Intelligence 细页（只读到主题目录）。
- TickTick 英文帮助中心的模板、导入导出文章（页面 JS 渲染，抓取为空）。
- Focalboard、Wekan、Super Productivity、Kanboard 的产品功能官方文档（只核到仓库元数据级）。
- C/C++ 自然语言日期解析开源库（三组仓库搜索零命中）。
- 撤销、模板的 commit 级参照点。
- 全局快捷键所用的 macOS / Windows 系统接口名称（仅有参照仓库的文件名佐证两端可实现）。
- Obsidian Tasks 除重复之外的日期语法与快捷键；各产品「拖拽本身」的官方交互说明。

仓内（R4 §6.6 原列；已由 R1 / R2 / R7 / R9 补齐或仍缺）：
- 已补齐：截止日控件（R2 + 主会话）；`src/module_agent/task_draft*` 与 todo 的关系（R8：仅 AI 起草示例引用 `todo.list`）。
- 两条未覆盖（初稿漏列，补回）：`src/module_markdown/markdown_module_internal.h` 的 hotkey 关键词命中内容未读——审计 grep 该文件 0 命中，已闭环；「复制任务」「筛选」类关键词未扫描。
- 仍缺：托盘徽标未读 `tray_bridge.c`；设置页模块开关界面未读；独立置顶浮窗能力未核；重复任务与冷启动归档清扫的交互未验证；Markdown 模块里 5 处 todo 字样的内容未读。

## 九、来源清单

精读（官方页正文，2026-11-26）：
- Things 3 键盘快捷键 https://culturedcode.com/things/support/articles/2785159/
- Things 3 重复待办 https://culturedcode.com/things/support/articles/2803564/
- Things 3 自然语言输入 https://culturedcode.com/things/support/articles/9780167/
- TickTick 功能页 https://ticktick.com/about/features
- 滴答清单新手指南 https://help.dida365.com/articles/6950689598586486784 （页面为 JS 渲染，正文取自页面内嵌数据）
- Todoist Quick Add https://todoist.com/help/articles/use-task-quick-add-in-todoist-va4Lhpzz
- Todoist 重复日期 https://www.todoist.com/help/todoist/features/introduction-to-recurring-dates-YUYVJJAV
- Todoist 提醒 https://www.todoist.com/help/todoist/features/introduction-to-reminders-9PezfU
- Microsoft To Do 智能识别 https://support.microsoft.com/en-us/todo/smart-due-date-reminder-recognition-in-microsoft-to-do
- Microsoft To Do My Day https://support.microsoft.com/en-us/todo/my-day-and-suggestions
- Vikunja Quick Add Magic https://vikunja.io/help/quick-add-magic
- Vikunja 日期与提醒 https://vikunja.io/help/dates-and-reminders
- Vikunja 任务关系 https://vikunja.io/help/task-relations
- Vikunja 导入导出 https://vikunja.io/help/import-and-export
- Vikunja MCP https://vikunja.io/help/mcp
- Obsidian Tasks 重复任务 https://github.com/obsidian-tasks-group/obsidian-tasks/blob/main/docs/Getting%20Started/Recurring%20Tasks.md

扫描（搜索摘要 / 目录 / 仓库元数据）：
- Apple 提醒事项指南首页 https://support.apple.com/guide/reminders/welcome/mac （只读到主题目录；初稿误列精读，已改）
- Things 3 支持首页目录 https://culturedcode.com/things/support/
- TickTick 英文帮助中心 Smart Recognition https://help.ticktick.com/articles/7081924556310446080 （抓取为空）
- §二 表中各仓库的许可证、分支、提交、路径（`gh api` 元数据）

仓内（R4 + R9）：`src/main.cpp`、`src/app_registry.h`、`src/module_todo/todo_service.h`、`todo_tools.h/.cpp`、`todo_module.cpp`、`todo_ui_dialogs.cpp`、`src/module_schedule/schedule_provider.h`、`schedule_ui_edit.cpp`、`src/platform/notification.h`、`notification_macos.mm`、`src/module_settings/github_sync_*.h`、`src/ui_kit/dialog_shell.h`、`docs/EUI-NEO-CAPABILITIES.md`、`EUI-NEO/components/datepicker.h`、`timepicker.h`、`toast.h`、`virtuallist.h`、`virtualmasonry.h`、`EUI-NEO/include/eui/app.h`、`EUI-NEO/3rd/tray/tray.h`、`.zcode/cheatsheets/platform-file-map.md`；池 `docs/pool/pool.db` 只读读取 #382、#384。
