> **存档说明**（非原文内容）：来源：只读盘点子代理 2026-10-19 产出，原文照录。作为微信风格 HTML demo 的**界面全量制作清单**（用户口径：所有界面含弹窗一个不能少）与后续工单"各页弹窗归页"口径的依据。弹窗判据：`components::dialog` 唯一调用点 `src\ui_kit\dialog_form.cpp:12`，`frame.id` 全集已穷尽。

---

# DevDesk 产品 UI 弹窗/浮层/右键菜单 全量清单

文件路径均相对 `D:\project\gui\DevDesk\src\`。

## 1. 模态弹窗清单（24 条 = 22 个 DialogFrame + 1 弹窗内覆盖浮层 + 1 全屏覆盖层）

| # | 弹窗名 | 归属 | 触发入口 | 位置 | 用途 | demo 建议 |
|---|---|---|---|---|---|---|
| 1 | 新建/编辑任务 `agent.dialog.task` | 智能体页 | 头部「+ 新建任务」（agent_module.cpp:1588）/ 行「编辑」chip（:1895） | agent_module.cpp:1963（frame :1989） | 任务名+触发类型+cron 模板+迭代上限等表单 | 智能体页右上「+ 新建任务」 |
| 2 | AI 生成草稿子弹窗 `agent.dialog.task.draft` | 智能体页，弹窗内弹窗 | 任务弹窗内「AI 草稿」chip（:2020-2025） | agent_module.cpp:2356（frame :2366，z=1100 叠加） | 一句话→LLM 生成草稿回填 | 打开 #1 后点「AI 草稿」，做叠层 |
| 3 | 任务详情 `agent.dialog.detail` | 智能体页 | 任务行点击/「详情」chip（:1826,1890,1932） | agent_detail_dialog.cpp:58（frame :100） | 摘要三卡+配置头+执行记录+消息区 | 点任务行 |
| 4 | 任务配置覆盖浮层 `.config.scrim/.popover` | #3 内 | 详情「任务配置」chip（:350-366） | agent_detail_dialog.cpp:1158-1313 | 弹窗内浮层：目标/限次/破坏性/异常/执行记忆 | #3 内点「任务配置」 |
| 5 | 删除任务确认 `agent.dialog.delete` | 智能体页 | 行「删除」chip（:1903） | agent_module.cpp:2484 | 「确认删除？」红字 | 行「删除」 |
| 6 | 新建任务 `todo.dialog.newtask` | 待办页 | 头部「+ 新建任务」（todo_ui_board.cpp:145）/ 列底「＋」（:344） | todo_ui_dialogs.cpp:38 | 选列+任务名 | 待办页「+ 新建任务」 |
| 7 | 新建列 `todo.dialog.newcol` | 待办页 | 头部「+ 列」（:134） | todo_ui_dialogs.cpp:123 | 列名 | 「+ 列」 |
| 8 | 重命名列 `todo.dialog.rename` | 待办页 | 列头 ✎（:333）/ 右键 | todo_ui_dialogs.cpp:262 | 列改名 | 列头 ✎ |
| 9 | 删除确认 `todo.dialog.delcol` | 待办页 | 右键「删除」、详情「删除」（:804） | todo_ui_dialogs.cpp:333（标题动态「删除任务/删除列」:348） | 不可恢复确认 | 卡片右键→删除 |
| 10 | 逾期任务清单 `todo.dialog.overdue` | 待办页 | 头部「逾期 N」badge（:107-123） | todo_ui_dialogs.cpp:411 | 标题「逾期任务（N）」，行点击跳详情 | 造逾期数据点 badge |
| 11 | 任务详情 `todo.dialog.detail` | 待办页 | 卡片点击（:468）/ 右键「编辑」 | todo_ui_dialogs.cpp:594（归档列只读态「任务详情（只读）」:615） | 标题/描述/截止（日期滚轮 Popup::DatePick）/优先级/标签；未保存 backdrop 双击守卫 | 点卡片 |
| 12 | 新建笔记 `md.dialog.new` | 笔记页 | 侧栏「+ 新建笔记」（markdown_module.cpp:451）/ 空态「+ 笔记」（:644）/ Ctrl+N（:2597） | markdown_module.cpp:2316 | 笔记名 | 「+ 新建笔记」 |
| 13 | 新建文件夹 `md.dialog.newfolder` | 笔记页 | 目录树「+ 文件夹」（:655） | markdown_module.cpp:2384 | 父目录 dropdown+文件夹名 | 目录树「+ 文件夹」 |
| 14 | 删除笔记确认 `md.dialog.del` | 笔记页 | 文件行右键「删除笔记」（:3366） | markdown_module.cpp:3426 | 不可恢复 | 文件右键 |
| 15 | 重命名笔记 `md.dialog.rename` | 笔记页 | 文件行右键「重命名」（:3361） | markdown_module.cpp:3465 | 路径输入（预填） | 文件右键 |
| 16 | 新建/编辑 Profile `claude.dialog.form` | Claude 页 | 左栏「+ 新建 Profile」（claude_ui_list.cpp:562）/ Ctrl+N / 右键「编辑」 | claude_ui_form.cpp:1012 | 名称/base URL/模型/API Key/环境变量（带 tooltip）/高级 JSON | 「+ 新建 Profile」 |
| 17 | Profile 删除确认 `claude.dialog.delete` | Claude 页 | 右键「删除」→claude_module.cpp:446 | claude_ui_form.cpp:1187 | 「确认删除？」 | Profile 右键 |
| 18 | 共享配置片段 `claude.dialog.shared` | Claude 页 | 左栏「+ 新建片段」（:708）/ 片段卡编辑 | claude_ui_form.cpp:1307 | 片段名+JSON；编辑态多「删除」 | 「+ 新建片段」 |
| 19 | 片段删除确认 `claude.dialog.shared.delete` | Claude 页，弹窗叠弹窗 | #18 内「删除」（:1405） | claude_ui_form.cpp:1435 | 「确认删除片段？」 | 编辑片段→删除 |
| 20 | 调用记录 `claude.dialog.calls` | Claude 页 | 详情「查看全部 →」（claude_ui_detail.cpp:629-641） | claude_ui_logs.cpp:177 | 搜索+状态码过滤+分页 | 详情面板 |
| 21 | 余量查询配置 `claude.dialog.quota` | Claude 页 | 详情「余量查询」（:885）；首页余量卡「去配置」路由（home_quota_card.h:89→claude_module.cpp:872） | claude_ui_quota.cpp:71 | 供应商 dropdown+参数+测试连接 | 详情面板 |
| 22 | cc-switch 导入 `claude.dialog.ccswitch` | Claude 页 | 左栏「从 cc-switch 导入」（:589） | claude_ui_ccswitch.cpp:245 | 勾选导入+结果报告 | 左栏按钮 |
| 23 | 高级设置 JSON 全屏覆盖层 `claude.jsonfs`（z=1050） | #16 内 | 表单「全屏编辑」（:721） | claude_ui_form.cpp:1089 | 全屏模态编辑+格式化；Esc=放弃 | 表单内点「全屏编辑」 |
| 24 | 启动 splash | 主窗内全屏覆盖层 | 应用启动自动（app_sidebar.cpp:234） | ui_kit/splash.cpp:41 | 极光波 shader+logo+假进度条，2.0s+0.3s 淡出 | demo 首屏加载动画复刻 |

Toast 浮层族（轻量非模态）：`app.notice`（app_sidebar.cpp:26/286，跨页）；`claude.toast`（claude_module.cpp:504）；`md.toast` + 外部修改冲突 action-toast「文件已被外部修改/重载磁盘版本」（markdown_module.cpp:2498-2526，与 md.toast 互斥）——**参考清单所写"外部修改弹窗"实为此 action-toast，非弹窗**；`todo.toast`（todo_module.cpp:530）；`agent.detail.toast`「已复制」（agent_module.cpp:1959）；`settings.toast`（settings_module.cpp:512）。

## 2. 右键/样式菜单清单（6 处）

1. 托盘 styled 菜单——托盘右键（main.cpp:605→462-478），项「显示主窗口/退出 DevDesk」（core/tray_menu_spec.h:29-33）
2. `md.ctx` 文件行右键（markdown_module.cpp:705/816→3333）——「重命名/删除笔记」
3. `md.editor.ctx` 编辑器右键（:1029）——「插入表格→1-6 行×1-8 列」级联（:3375-3387）
4. `todo.ctx` 卡片右键（todo_ui_board.cpp:469）——「编辑/上移/下移/移动到某列…/删除」（todo_ui_dialogs.cpp:195-203）
5. `todo.ctx` 列头右键（:293）——「重命名列/左移/右移/改色(8 色子菜单)/删除列」（:206-215）
6. `claude.list.ctx` Profile 右键（claude_ui_list.cpp:513）——「编辑/建副本/复制配置/删除/设为当前」（:737-738）

## 3. 托盘/子窗/OS 原生

- 托盘菜单窗：托盘级独立窗——demo 做"托盘模拟区"（右下角图标点击展开同款菜单）
- **无 `app::openWindow` 产品内使用**（grep 零命中）；桌宠模块 2026-10-13 已整模块摘除（main.cpp:645 注释）——demo 无需子窗
- OS 原生（不进页面 demo，放静态示意/文案）：① 启动期 DB 损坏阻塞 msgbox 5 变体（main.cpp:79/90/100/112/170）②「选择工作目录」原生目录选择器（claude_module.cpp:632，详情「运行」触发）

## 4. 覆盖度声明

- **搜索词全集**：`[Dd]ialog`、`components::dialog(`、`frame\.id = "`、`compose\w*Dialog|draw\w*Dialog|drawNew|openDraft`、`backdrop|遮罩`、`[Pp]opup|浮层`、`scrim|popover|覆盖层`、`onContextMenu|contextMenu|showStyledMenu|styledMenu`、`toast|Toast`、`tooltip`、`datePicker|dropdown`、`openWindow|newWindow|createWindow`、`tray|托盘|桌宠|pet|splash`、`msgbox|message_box|pickFolder`、`modal`、各弹窗 open 置位族
- **已查范围**：src 全树（7 个 module_* 逐个 + core + platform + ui_kit + 根）。module_home、module_about 确认零弹窗/零菜单/零 toast。`frame.id` 全集 grep 与各模块 Esc 关链（markdown :355-366、todo Popup 枚举 8 项、claude :410-421、agent :2585）双向核对一致，无游离弹窗
- **口径确认**：dropdown open 态（settings 6 处、claude 表单/配额/调用记录、agent cron 模板、md 父目录）与 todo 截止日期滚轮（todo_ui_dialogs.cpp:386-410）、claude 环境变量 hover tooltip 均为**控件内浮层，不算弹窗**；右键菜单单列；toast 属浮层单列
- **与三份参考审计的差异**：多出 4 项（agent 配置浮层 #4、claude 片段删除确认 #19、JSON 全屏覆盖层 #23、启动 splash #24）、纠正 1 项（"外部修改"是 action-toast 非弹窗）、托盘菜单现为 2 项、桌宠已摘除
