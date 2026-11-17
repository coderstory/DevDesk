# UI/UX 走查·第一路：生产力域（首页/笔记/Git/待办/定时）+ 全局壳

- 日期：2026-11-18｜角色：ui-ux-designer｜池条目：need 284｜性质：只读走查清单（不代修、无 demo、无截图）
- 判偏差标尺：`docs/design/2026-11-07-全站卡片设计基准.md`（#195）、`2026-11-07-列表统一规范.md`（#219）、`2026-11-07-dialog-standard-design.md`（#156）、`2026-11-06-macos-custom-titlebar-design.md`、`2026-11-03-全局参数卡.md`、`2026-11-01-UI整改批次IV`；手册卡壳点引自 `docs/research/2026-11-18-app-polish-audit/manual/00~05`。
- 视觉留档：本阶段无截图证据，凡需目检确认的条目标「需目检」，后续 QA 实操截图补证。

## 0. 总览计数

| 级 | 数 |
|---|---|
| P0 | 1 |
| P1 | 3 |
| P2 | 12 |
| P3 | 7 |
| 合计 | 23 |

**Top 5 最严重**：
1. [P0] 定时——编辑已有 cron 规则时表达式不回显且被清空，改任意字段后保存即损坏规则。
2. [P1] Git——Tab 为自绘下划线式且 active 字重 500，与全站块面 Tab、两档制双违。
3. [P1] 定时——三个弹窗（编辑/历史抽屉/删除确认）仍裸 components::dialog 自绘，未迁标准壳。
4. [P2] 全局——ui_kit::button Secondary 三态同色，全站空态引导按钮无悬停反馈。
5. [P2] 跨模块——页头动作控件形态/高度四页四样（40/36/32/裸文字）。

**正面确认（非发现）**：两档字重已在五模块落地（grep 无 500 残留，仅 git Tab 一处违例）；schedule 行卡字重全合规（schedule_ui_list.cpp:841/876/927/989）；标准弹窗壳 drawDialogShell 已覆盖 markdown/todo/claude/settings/release 等 9 文件；todo 优先级染色基色已走 cardSurface（todo_ui_board.cpp:74-75，#219-C9 已落）；home 统计卡已走 cardSurface（home_module.cpp:573-648，#219-C2 已落）；todo 卡间距 8、schedule 列表卡距 8 符合 #219 §4.3。

---

## 1. 跨模块一致性主题（同类控件双方锚点对照）

### T1 弹窗壳双轨残留（P1）
- 现象：标准壳 `ui_kit::drawDialogShell`（dialog_shell.cpp:95-160，含标题分隔线 y=52 / 底区细线 y=h−73 / 按钮右锚 min96 自适应 / 最小尺寸 420×240，常量 metrics.h kDialogHeaderH 69 / kDialogFooterH 73）已覆盖 9 文件，但 schedule 三弹窗仍裸 `components::dialog` 手拼。
- 用户所见：从「笔记→待办」一路弹窗都有标题分隔线与右下主按钮，切到「定时」页弹窗突然没有线、按钮变固定 120×36 左下多出一颗裸文字「删除」。
- 锚点：schedule_ui_edit.cpp:471（编辑）、schedule_ui_history.cpp:461（历史抽屉）、schedule_ui_list.cpp:1247（删除确认）vs dialog_shell.cpp:95-160、metrics.h（kDialogHeaderH 等）。
- 基准出处：#156 §3 四段式结构、§2.1（schedule 三弹窗标「自绘 P1 P2 P4 P6」未迁）。
- 建议方向：三弹窗迁 drawDialogShell；删除钮用 DialogShellSpec 危险变体（40 高、min 宽、带 hover 档）。

### T2 Git Tab 形态与字重双违（P1）
- 现象：Git 左栏「全部文件/仅修改」用自绘下划线式 Tab，active 字重 500；全站 Tab 组件 wtab（块面式、active 600、页级 40 高 15 字）wtab.h 注释明写「淘汰下划线式」。
- 用户所见：Git 页的模式切换条是一条 2px 绿下划线，其它页（笔记/定时）是圆角块面选中底；且激活字 500 是全站列表域唯一第三档。
- 锚点：git_views.cpp:163-189（underlineTab，:174 fontWeight(active?500:400)）vs wtab.h:14-40（S2.2 块面式 +「淘汰下划线式」注释）、wtab.cpp:62。
- 基准出处：#219 §2.1（列表域禁 500）+ 全局参数卡 §F2（TAB 归一 40/15 档）。
- 建议方向：换 ui_kit::wtab（页级档 40/15），或至少改 600；块面/下划线二选一须过会定形态。

### T3 页头动作控件形态不一（P2）
- 现象：同为页头右侧动作，五页五种规格。
- 用户所见：首页「刷新」=描边按钮 100×40、待办「新建任务」=绿填充 32 高 12px 小字、笔记「新建笔记」=绿填充 136×40 14px、定时「新建」=绿填充 40 高、「运行历史」=裸绿字无底无 hover、Git 空态主按钮=36 高。
- 锚点：home_module.cpp:489-502（刷新）、todo_ui_board.cpp:200-209（新建 32 高 fontSizeSmall）、markdown_module.cpp:451-459（新建 136×40）、schedule_ui_list.cpp:1100-1115（裸文字 history + 新建 kButtonHeight）、git_views.cpp:252-255（36 高）vs button.h:41（kButtonHeight=40）。
- 基准出处：#195 §4.1（页头右端主按钮 40px）；全局参数卡 §F2（页头归一 40 档）。
- 建议方向：页头主按钮统一 40 高 14 字；「运行历史」升为 Outline 按钮或次级按钮（裸文字不可点面积只有字宽）。需目检确认观感。

### T4 空态组件覆盖不一（P2）
- 现象：全站统一空态组件 emptyState（图标+主句+引导+行动钮）只有 todo/mcp/agent/memory 在用；git 左中栏与 schedule 空态为裸 12px 灰字。
- 用户所见：待办空列有图标引导，Git 左栏空是孤零零一行灰字「工作区无变更」，定时无规则页只有一句文字。
- 锚点：todo_ui_board.cpp:251-259（EmptyStateOptions）vs git_views.cpp:598-609（裸文字）、schedule_ui_list.cpp:1111 注释「空态纯文字提示」。
- 基准出处：#195 §5（统一空态组件：图标+主句+引导+行动钮）。
- 建议方向：git/schedule 空态换 emptyState（git 左栏窄盒用无图标档）。

### T5 果冻按压覆盖面不齐（P2）
- 现象：jellyCard 仅 schedule 行卡/agent/侧栏消费；todo 任务卡、git 树行、md 树行、home 统计卡无按压形变。
- 用户所见：定时页卡片按下去有回弹，待办卡片按下去是死按（仅变色）。
- 锚点：jelly.h:53-66 + 消费面 grep（schedule_ui_list.cpp、agent_module.cpp、app_sidebar.cpp）vs todo_ui_board.cpp:890-957（真卡无 jelly）、git_views.cpp:633-694（行无 jelly）。
- 基准出处：批次IV §1.4（L2 业务卡统一 jellyCard）+ #195 §6（按压果冻基线）。
- 建议方向：todo 任务卡与 home 统计卡接 jellyCard；git/md 树行属裸行可豁免，写明豁免口径即可。

### T6 Secondary 按钮无悬停反馈（P2）
- 现象：ui_kit::button 的 Secondary 档 normal/hover/pressed 三态全部 = t.colors.hover，视觉零变化；Danger 档 hover=pressed 同为 darkenRed。
- 用户所见：各页空态里的引导按钮（emptyState action 恒 Secondary）悬停毫无反应，像不可点。
- 锚点：button.cpp:43-49（Secondary）、:58-63（Danger）+ 消费方 empty_state.cpp:117-124；对照 dialog_shell.cpp:73-92 副按钮（surfaceElevated/hover/active 三态分明）。
- 基准出处：#156 §4.3（副按钮 hover 底=hover、按下=active）。
- 建议方向：Secondary normal 改 surfaceElevated；与 dialog 副按钮规格对齐。

### T7 键盘路径不一致（P2）
- 现象：待办新建弹窗标题框「回车直接创建」；定时编辑弹窗所有输入无 Enter 提交、保存只靠右下按钮；git 树/变更列表无键盘导航；todo 折叠列反而有完整 Enter/Space+focus 环。
- 用户所见：同为高频创建动作，待办可以一路键盘打完，定时必须鼠标点保存；键盘用户在 git 页只能靠 Tab 碰运气。
- 锚点：todo_ui_dialogs.cpp（新建弹窗回车创建，manual 04 流程 2）vs schedule_ui_edit.cpp:883-932（仅按钮 onClick，无 onEnter）；对照 todo_ui_board.cpp:511-530（折叠列键盘+focus 完整实现，可作范式）。
- 基准出处：#156 §9（键盘焦点/取消路径统一）。
- 建议方向：表单类弹窗统一「主输入框 Enter=提交、Esc=取消」；至少补 schedule 编辑弹窗。

### T8 列表域字重/材质遗留零散项（P3）
- git 状态徽标字母 10px/700（git_views.cpp:676-677）——两档制无 700 档（状态词 600）；徽标近图标属性，建议随 T2 一并归 600 或记豁免。
- dialog 标题 700（dialog_form.cpp:41/58、dialog_shell 经 drawDialogTitle 继承）——#219 §2.3 附录挂起项，全站弹窗标题与页头 600 不同档；单独过会。
- button.h:56 注释仍写「kButtonHeight（34px）」，实值 40（button.h:41）——注释漂移。
- schedule 历史抽屉卡距 10（schedule_ui_history.cpp:50）vs 列表卡距 8（schedule_ui_list.cpp:55，#219 §4.3 统一档）。
- todo 详情弹窗「删除」chip 用 t.colors.background 作按钮底（todo_ui_dialogs.cpp:879）——background 是窗口底 token，语义误用，应换 surfaceElevated/hover 档。

---

## 2. 逐模块清单

### 2.1 定时 schedule（4 条）

- [P0] 定时 编辑已有「表达式」规则时 cron 不回显且被清空 | 用户所见：双击一条 cron 规则只想改名字/启停，弹窗里表达式字段是空的；直接点「保存」后该规则的表达式被清掉，规则不再触发（手册 05 明文 v1 限制） | 代码锚点 schedule_ui_edit.cpp:393-395（manual 05 头注引） | 基准出处 #156 §9（表单回填/取消路径） | 建议方向：按 kind 回填表达式到表单；短期至少「保存时表达式为空且原规则是 cron → 阻止并红字提示」。
- [P1] 定时 三弹窗未迁标准壳 | 用户所见：编辑弹窗无标题分隔线与按钮区细线，保存/取消为固定 120×36 右下角，「删除」是左下 48×20 裸红字（热区小、无 hover）；与全站弹窗语言脱节（同 T1） | 代码锚点 schedule_ui_edit.cpp:870-932（删除裸文字 :873-880、按钮 120×36 :884-925）、:471 | 基准出处 #156 §3.1/§3.3 | 建议方向：迁 drawDialogShell + Danger 按钮变体。
- [P2] 定时 编辑弹窗保存无禁用态，校验错误全靠点击后红字 | 用户所见：必填没填也能点保存，点了才在预览区位置冒红字；#156 规定「主按钮置灰直至必填齐」 | 代码锚点 schedule_ui_edit.cpp:883-921（onClick 内 validate，无 enabled 态） | 基准出处 #156 §6（表单校验：主按钮禁用态直至必填齐） | 建议方向：validateAndParse 每帧算，失败置灰保存钮+就近红字。
- [P2] 定时「全屏提醒」渠道可勾选但实际不可触发 | 用户所见：新建规则的通知渠道里能勾「全屏提醒」，勾了永远没反应——总开关默认关且设置页无入口 | 代码锚点 channels/fullscreen_channel.cpp:42-46（manual 05 卡壳点 5） | 基准出处 基线四色语义（渠道应能力自洽） | 建议方向：设置页补开关，或渠道项置灰加「需在设置开启」说明。需目检渠道列表实际呈现。

### 2.2 待办 todo（4 条）

- [P2] 待办 归档无显式入口，只能拖拽进「已归档」列 | 用户所见：详情弹窗没有「归档」按钮；不知道拖拽就永远找不到归档功能；归档列内还不支持手动移出排序口径（manual 04 卡壳点 4） | 代码锚点 todo_ui_dialogs.cpp:862-924（详情头部仅 删除/标记完成 两 chip）+ todo_module.cpp:818（只读详情） | 基准出处 #219 §2.2 判据（高频操作应就近可达） | 建议方向：详情弹窗补「归档」动作 chip；或右键菜单补「归档」。
- [P2] 待办 任务卡无果冻按压，与定时/agent 卡不一致 | 用户所见：按住待办卡片拖拽前一刻，卡片无回弹形变（同 T5） | 代码锚点 todo_ui_board.cpp:890-957（真卡无 jelly 引用） | 基准出处 批次IV §1.4 | 建议方向：接 jellyCard（保持拖拽命中面不变）。需目检。
- [P3] 待办 页头「新建任务」按钮 32 高 12px 小字，主操作视觉弱于其它页 40 档（同 T3） | 用户所见：待办页头右上角的绿色主按钮明显比笔记/定时页的矮半截、字小一档 | 代码锚点 todo_ui_board.cpp:200-209 | 基准出处 #195 §4.1（页头主按钮 40px）+ 全局参数卡 §F2 | 建议方向：40 高 14 字归一。
- [P3] 待办 详情弹窗偏离 #156 三段式（UAT5 重设计在先） | 用户所见：详情弹窗头部右上有「删除/标记完成」两颗 chip，标题下没有分隔线；底部只有 取消|保存 钉底——好用但与其它弹窗长得不一样（同 T1 附注） | 代码锚点 todo_ui_dialogs.cpp:848-924（无 title.line、头部 chips :868-924） | 基准出处 #156 §3.1（头区=标题+分隔线，不加其他元素） | 建议方向：保留头部动作 chip 设计亦可，但补分隔线与底区细线归一视觉语言；或走会改 #156 头区定义。需目检。

### 2.3 Git（5 条）

- [P1] Git Tab 下划线式 + active 500 双违（同 T2） | 用户所见：Git 页模式切换条与全站任何一页都不同，激活字重是全站独一份的 500 | 代码锚点 git_views.cpp:163-189（:174） | 基准出处 wtab.h S2.2 + #219 §2.1 | 建议方向：换 wtab 或改 600。
- [P2] Git 空态页整页铺不透明 background、主界面顶栏铺不透明 surface | 用户所见：磨砂/半透窗口效果档下，Git 空态整页把窗材质挡死；选中仓库后顶栏也是一块不透明色带——其它页（首页等）都是透明透出材质 | 代码锚点 git_views.cpp:196（empty bg）、:272（top bg surface） | 基准出处 「页面不自铺底色」口径（home_module.cpp:384 注释、manual 00 §设置项联动） | 建议方向：空态页去底色、顶栏改 glassPanelFill 档；改后需三档材质目检。需目检。
- [P2] Git 左/中栏空态为裸灰字，无统一空态组件（同 T4） | 用户所见：左栏「工作区无变更」、中栏「工作区无变更」都是一行 12px 灰字，与待办/首页空态形态断层 | 代码锚点 git_views.cpp:598-609 | 基准出处 #195 §5 | 建议方向：窄栏用 emptyState 无图标档。
- [P3] Git 顶栏高 48 异于全站页头 40 档 | 用户所见：Git 页顶栏比其它页页头高 8px，切页时页头节奏跳一拍 | 代码锚点 git_views.cpp:28（kTopBarH=48）vs metrics.h:12（kHeaderH=40） | 基准出处 全局参数卡 §F2（页头归一 40） | 建议方向：顶栏 40、搜索框 32 居中不变；若 48 是为搜索框呼吸位，改 40+内边距。需目检。
- [P3] Git 空态主按钮 36 高、图标+文字按钮规格与全站 40 档不一（同 T3） | 用户所见：Git 空态页的「选择仓库目录」比标准主按钮矮 | 代码锚点 git_views.cpp:252-255 | 基准出处 button.h:41（kButtonHeight=40） | 建议方向：归一 40。

### 2.4 笔记 markdown（4 条）

- [P2] 根级新建文件夹入口缺失 | 用户所见：想先建目录结构，树里没有任何文件夹时右键无处可点（「新建子目录」只挂在文件夹右键上）；只能借「新建笔记」用 `/` 隐式建目录（manual 02 卡壳点 1） | 代码锚点 md_module_menu_poll.cpp:403（文件夹右键菜单） | 基准出处 基线四原则（高频操作可发现性） | 建议方向：「根目录」组行支持右键「新建子目录」，或树顶加显式新建文件夹入口。
- [P2] 退出应用时保存失败无任何用户可见提示 | 用户所见：磁盘满/只读时退出，编辑直接丢失，只有日志有记录（manual 02 异常表「未处理」） | 代码锚点 markdown_module.cpp:152 | 基准出处 #195 §5（失败态不冒充其它态） | 建议方向：退出路径遇 saveFailed_ 弹确认「有未保存修改，仍要退出？」。
- [P3] 状态栏「● 未保存」用 errorText 红 | 用户所见：正常打字过程中的未保存态也是红的，与「✕ 保存失败」同色——用户容易把常态误读为出错 | 代码锚点 md_module_editor.cpp:276-279 | 基准出处 #195 §4.3 四色语义（红=仅告警） | 建议方向：未保存改 textSecondary/warning，红只留保存失败。需目检。
- [P3] 文件名过长/Windows 保留名无预检 | 用户所见：CON/NUL 或超长名在保存/重命名时才收到 OS 级「重命名失败」toast，无前置拦截 | 代码锚点 md_module_menu_poll.cpp:633-642（仅绝对路径/盘符/../ 保留名外拦截）、:649-651 | 基准出处 #156 §6（错误就近、前置） | 建议方向：名称校验补保留名与长度上限（MCP 写侧已有 64 字先例 notes_mcp_tools.cpp:25）。

### 2.5 首页 home（1 条）

- [P2] 底部动作行三种按钮级别同排（填充/描边/幽灵），层级语义未对齐 #156 | 用户所见：「+ 新建笔记」绿填充、「+ 新建任务」白底描边、「切换 Claude Profile」纯文字绿字——三颗一字排开视觉重量三级跳，幽灵钮可点击面积感知弱 | 代码锚点 home_module.cpp:944-969（三档 colors） | 基准出处 #156 §4.3（同视口主按钮唯一为弹窗场景定；页面级动作行建议两档封顶） | 建议方向：动作行收敛为 主(填充)+次(描边) 两档，Profile 切换并入描边钮或移入卡内。需目检。

### 2.6 全局壳（3 条）

- [P2] 侧边栏快捷键 Ctrl+B 在 Markdown 编辑器被「加粗」占用，其余页无占用说明；无逐页枚举 | 用户所见：焦点在编辑器里按 Ctrl+B 不折叠侧栏（预期内）；但焦点在待办搜索框、定时输入框等处按 Ctrl+B 是否折叠无统一契约（manual 00 卡壳点 4） | 代码锚点 main.cpp:1074 | 基准出处 #156 §9（取消/快捷键路径统一） | 建议方向：定契约「单行输入框处放行折叠」并逐页验证。
- [P3] 数据库迁移失败仅 LOG_FATAL，无专门用户弹窗文案 | 用户所见：迁移失败时应用直接终止，无「数据库升级失败」说明窗（manual 00 异常表「半处理」） | 代码锚点 main.cpp:210-212 | 基准出处 #195 §5（错误态必须显形） | 建议方向：还原失败弹窗同款 message_box，文案含数据目录路径与恢复指引。
- [P3] 托盘「模块关闭提示」toast 完整文案未定稿（拼「『短名』」片段） | 用户所见：关闭某模块开关后 toast 文案是否通顺需实机确认（manual 00 卡壳点 2） | 代码锚点 app_assembly.cpp:326,359 | 基准出处 手册 00 | 建议方向：实机核一次并定稿文案。需目检。

---

## 3. 先验验证清单（后续 QA 实操按格子补证）

| 格子 | 走查条目 | 验证方式 | 状态 |
|---|---|---|---|
| G1 | P0 cron 不回显 | 新建 cron 规则→编辑→改名字→保存→查规则是否仍触发 | 待 QA |
| G2 | T1/T2.3 弹窗壳 | 定时三弹窗亮暗两主题截图对照 dialog-standard | 待 QA |
| G3 | T2 Git Tab | Git 页与笔记页 Tab 同屏截图对照 | 待 QA |
| G4 | T6 Secondary hover | 空态引导按钮 hover 前后截图 | 待 QA |
| G5 | Git 材质 | 窗口效果三档 × 亮暗，Git 空态页与顶栏截图 | 待 QA |
| G6 | T3 页头按钮 | 五页页头并排截图 | 待 QA |
| G7 | T5 果冻 | 待办卡/定时卡按压录屏对照 | 待 QA |
| G8 | md 未保存红 | 打字停顿 200ms 截状态栏颜色 | 待 QA |
| G9 | home 动作行 | 亮暗两档截图看三档按钮层级 | 待 QA |
| G10 | T7 键盘 | 定时编辑弹窗内按 Enter/Esc 实测 | 待 QA |
| G11 | md 根目录右键 | 空树右键行为实测 | 待 QA |
| G12 | 托盘 toast | 开关模块实机触发截图 | 待 QA |

## 4. 走查方法与边界说明

- 代码走查采用 file:line 定向分段读（±60~150 行/段），未整文件通读；字重矩阵由全量 grep（fontWeight）建立，硬编码字号 grep 显示五模块无脱离 token 的裸字号。
- 不含记忆/壁纸/MCP/终端/上线/设置/关于（走查第二路）；不收录 notepad。
- 手册已证事实（各模块流程/文案/异常表）直接引用不重查；引用格式见各条「代码锚点」。
