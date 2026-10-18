# 智能体页 UX 全流程审计（2026-10-18）

> 方法：把自己当第一次用智能体页的真实用户，对每个操作步骤问四个问题——知道发生了什么吗（反馈）、出错知道为什么和怎么办吗（错误态）、能少点一次吗（摩擦）、空/加载/成功态长什么样（状态完整性）。
> 只读审计，零代码改动。所有行号基于本文件落盘时的 `dev`（merge `6e1319c`）。
> 对照物：`docs/superpowers/specs/2026-10-18-agent-detail-dialog-redesign.md`（下称「重设计 spec」，P1–P5 分级）；`docs/EUI-NEO-CAPABILITIES.md`（下称「能力表」，修法只落 ✅ 能力）。

## 0. 与重设计 spec 的边界（已覆盖点，本文不再立项）

| 重设计已覆盖 | 阶段 |
|---|---|
| 详情内最近一次/下一次/近 7 天摘要三卡 | P1/P2 |
| 详情时间线 20 条按天分组、失败一行摘要 | P2 |
| 详情内运行中耗时「进行中 · N秒」、错误全文滚动、计划对照、token 明细 | P3 |
| 配置浮层分段字段（替代 12px 灰长串 `.info`）、消息默认折叠、复制 toast、工具 chip 去 emoji | P4 |
| `runRowText` 的「耗时 45s」原始秒口径（退役，统一 `formatRunDurationText`） | P2（spec §11） |
| 状态色 token（§9.1 success/error/warning/info） | 已定义，本文直接复用 |

重设计 spec 明确的非目标 = 本文主战场：**任务列表、创建/编辑表单、通知中心、run 的取消/删除动作、跨页一致性**。

---

## 1. 任务列表（`agent_module.cpp:1616-1759`）

### A1（低）空态只有一行灰字，无行动入口
现状：`agent.empty` 单行 tertiary 文案「暂无任务——点右上角…」（1633-1641），用户要去左上角找按钮。
定性：状态完整性 + 摩擦。空态是转化率最高的一屏，现在只给路标不给门。
修法：空态改居中块：fa-robot 图标（能力表 §6 图标 ✅）+ 主文案 + 40 高「创建第一个任务」主按钮直调 `openNewTaskDialog()`。断言：空任务 compose 后存在 `agent.empty.cta` 且 onClick 后 `taskDialogOpen_==true`。规模 S。

### A2（中）状态列无色无点，五种状态一个颜色
现状：`.status` 12px `textSecondary` 纯文本（1707-1713），「运行中」「失败」「成功」视觉权重完全相同；扫一屏无法回答「哪个任务出问题了」。
定性：缺反馈，列表核心职责失守。
修法：状态文字前加 8px 圆点（`rect.size(8,8).radius(8)`，能力表 §10 状态指示点 ✅），色映射直接复用重设计 spec §9.1 表（completed=success、failed=error、limited=warning、running=info、cancelled=tertiary）；点 + 中文文字并存，不只靠色。断言：failed 行 `.status.dot` 色值 == `t.colors.error`；文案仍为「失败」（可访问性双通道）。规模 S。

### A3（中）「下一次什么时候跑」在列表不可见
现状：列表行只有状态列；schedule/once/daily 任务最关心的下次触发时间任何地方都没有（详情卡是 P2 范围且要开弹窗才知道）。
定性：缺状态。定时任务的价值全在「它会自己跑」，用户却看不到它还会不会跑。
修法：新增「下次触发」列（放状态右侧，tertiary 12px）：schedule=`nextCronMatch`（`cron.cpp` 已有）格式化 `MM-DD HH:MM`；once=`once_at` 直读；daily=语义文案「今天打开后」（不许造假时间，同 spec D10 契约）；manual/event=「手动 / 事件」。**缓存纪律照抄重设计 spec §8.5/A9**：每分钟 + 配置变化才算一次，禁止 compose 逐帧回扫 cron（最坏 366×1440 次 localTime）。断言：cron `0 9 * * 1-5` 的任务在列表行可见含「09:00」的文本；把假时钟拨过触发点后文本变化（注入 `deps_.schedulerNow`）。规模 M。

### A4（中）已停用任务与启用任务长得一样
现状：禁用后唯一线索是操作 chip 文案从「禁用」变「启用」（1724-1729）；标题、状态、徽章全无变化。用户扫列表无法区分「这个任务在岗吗」。
定性：缺状态。
修法：`enabled==false` 时 title `withOpacity(0.45)` + 行尾加「已停用」tertiary 徽章（复用 `.warn` 徽章的几何槽，`kTaskWarnW`）。断言：禁用任务行的 title opacity < 1 且存在 `.off` 徽章节点；启用后两者消失。规模 S。

### A5（中）运行中再点「启动」是死按钮
现状：`.run` 的 disabled 条件只有 `loop_ == nullptr`（1748）；任务已在跑时按钮照常可点，`startTask` 撞并发闸后只 `LOG_WARN` 静默返回（586-589）。用户点了没任何反应，会反复点。
定性：摩擦 + 缺反馈（按钮状态与真实可用性脱节）。
修法：disabled 条件加 `latestRuns_.count(id) && latestRuns_[id].status=="running"`，label 切「运行中…」。断言：种子一条 running run 后 compose，`.run` disabled（复用 `agent_ui_run_button_opens_detail_dialog` 的断言手法）；run 结束后按钮恢复可点。规模 S。

### A6（低）行无 hover 反馈
现状：行 `.interactive(true).onClick`（1670-1677）但无背景 hover 态；与详情时间线选中态（重设计 §9.1 hover token）不成体系。
定性：不一致。修法：行底 rect `.states(n, t.colors.hover, n)`。断言可并入 A2 批次的 probe。规模 S。

---

## 2. 创建/编辑任务表单（`composeTaskDialog:1772-2111`）

### B1（中）六种触发方式零解释，「每天」的语义是隐藏规则
现状：下拉只有六个词（252-256）；「每天」实际语义 =「当天首次打开应用后的第一个调度周期跑一次」（249-250 注释，用户不可见），与直觉「每天固定时刻」相悖；「常驻」「事件」对新用户是黑话。schedule/once 分支各有预览行，manual/resident/event/daily 选中后**什么提示都没有**——用户不知道自己刚选了什么。
定性：缺反馈，新手第一分钟就会选错。
修法：y=212 的现成提示槽（`skipIfEmpty`，schedule/once 已占用，1937-1942/1963-1968）扩展到全部六类：新增 `kTriggerHints[]` 与 `kTriggerLabels` 同构，onChange 时刷新（`refreshCronPreview`/`refreshOncePreview` 同一机制）。daily 的 hint 必须写明「需应用保持运行，打开后首个调度周期执行」。几何契约不动（槽位已存在，弹窗高度不变）。断言：切到「每天」后 `.hint` 文本含「应用」；切到「手动」后文本非空。规模 S。

### B2（中）「迭代上限」裸数字输入，跑满后果不可知
现状：无说明的数字输入（1996-2006）；新用户不知道「迭代」是什么、默认 10 意味着什么、跑满后 run 以「受限」终态结束（limited 的来源对用户完全不可见，直到列表突然出现一个「受限」）。
定性：缺反馈；受限状态在 A2 修好后仍需要因果解释。
修法：hint 槽加「跑满上限将以『受限』结束，不会继续重试」；input 换 `components::stepper`（能力表 ✅，min=1 max=50 与 saveTaskDialog 的 clamp 886 对齐），省掉手输非法中间态（`parseIterInput` 的容错只保数据不保体验）。断言：stepper 加号一次后 `.iter` 值 +1；hint 文本含「受限」。规模 S。

### B3（低）「允许破坏性操作」开关无解释
现状：开关文案即全部信息（2020-2028），用户无从判断「破坏性」指什么、勾了会发生什么。
定性：危险开关缺说明。修法：tooltip（能力表 ✅）或并入 B1 hint 槽：「允许任务调用删除文件等高危工具；不勾则此类工具调用被拒」。断言：hint 文本含「高危」。规模 S。

### B4（中）四条关闭路径全部静默丢草稿
现状：Esc（onCancel 1831 等）、✕（1809）、取消（2098）、遮罩（`onOpenChange` 1789-1791）都直调 `closeTaskDialog()`；`openNewTaskDialog` 每次清空全部草稿（631-653）。写了 200 字 goal 误触 Esc = 全部重来。
定性：数据丢失风险（高频路径：Esc 是肌肉记忆）。
修法（两全解，选 a 不做确认弹窗）：`openNewTaskDialog` 不清 `editGoal_`/`editContextNote_`（其余字段保留清空），重开时回填并在错误行位置显示「已恢复上次未保存的草稿」（tertiary，输入即消）。避免在弹窗上叠确认层（drawDialogFrame 嵌套虽可行但交互重）。断言：输入 goal → 关闭 → 重开新弹窗 → `<id>.hit` InputState 文本 == 原文（复用 `agent_ui_dialog_create_task_persists_fields` 的读回手法）。规模 S。

### B5（低）保存成功零反馈
现状：保存成功静默关窗（909-910），列表刷新是唯一暗示。
定性：轻；但「创建的东西在哪」对首用用户不确定。修法：页级 toast「已创建任务 / 已保存」（`ui_kit::ToastState` ✅，机制与重设计 D13 同款，注意单实例节流位）。断言：save 成功后 `toast_.open==true` 且 title 含「已」。规模 S。

### B6（低）表单错误行 18px 固定高不换行
现状：`agent.dialog.task.error` 固定 height 18（2081-2084）；cron 解析错误等长文案会截断（截断处恰是「怎么办」的部分）。
定性：错误态不完整。修法：按 `estimateTextBlockHeight` 给高 + `wrap(true).maxWidth(innerW)`（message_blocks 已有估算函数）。断言：注入 60 字错误后错误节点高度 > 18。规模 S。

---

## 3. 运行中（run 进行时）

### C1（高）无法停止一个正在烧 token 的 run
现状：UI 全文无任何 cancel 入口；`ActiveRun::cancel` 原子位与循环检查**都已存在**（`agent_loop.cpp:201` 迭代边界查 token → `status="cancelled"` → `finishRun`:350，用户取消优先于超限判定 :206），但只有 `onStop`/析构在用（373/449）。用户启动错任务后唯一选择是等它跑完 N 轮迭代。
定性：能力缺口 + 成本损伤（每次误启动都在烧 API 钱）。重设计 spec 本批不做取消（§2 非目标），但它是当前页面的头号硬伤。
修法：详情弹窗标题行（运行中时）加「停止」chip → 找到 `runs_` 中该 taskId 的 ActiveRun 置 `cancel=true`；文案注明「当前轮结束后生效」（in-flight LLM 请求不中断，协作式取消的诚实口径）。断言：启动后点停止 → 轮询至 run 终态 → `latestRuns_[id].status=="cancelled"` 且列表状态列「已取消」；对已结束 run 点停止不崩不产生新 run。规模 S/M。

### C2（中）运行中列表零动态
现状：运行中任务在列表里只是状态列文字「运行中」；无耗时、无点、无任何变化（详情内的进行中耗时是 P3 范围）。
定性：缺反馈（`uiPolling_` 已保证每帧刷数据，但列表没消费它）。
修法：运行中行的状态列文案带已耗时「运行中 · 45秒」（`now - startedAt`，直接复用 `formatRunDurationText`）；依赖 A2 的点。断言：运行中行状态文本含「·」且随时间增长（假时钟 +2s 后文本变化）。规模 S。

### C3（低→缓）跨页无运行中感知
现状：run 进行时切到其它页面无任何全局指示；失败 toast 4.5s 即逝（见 D2）。
定性：缺反馈；但修它要动 app 侧边栏层（跨模块），页内已有详情自动弹出兜底。
修法：缓，规模 L（侧边栏 agent 图标运行中圆点，`app_sidebar.cpp` 改动 + 跨模块状态源）。本轮不做，防范围膨胀。

---

## 4. run 结束

### D1（中）成功 = 完全静默，且被测试钉死
现状：`detectRunEndNotifications` 只认 failed/limited（65-78，注释 1171「completed/cancelled 静默」）；`agent_ui_run_completed_silent_no_toast`（test_agent_ui.cpp:2157）把「成功不通知」钉成契约。手动跑一个任务，跑完了用户不知道，只能自己刷列表。
定性：缺反馈。失败响成功哑的对比让「没消息」无法区分「没跑」和「跑好了」。
修法：completed 也入 `pendingNotices_`（title「智能体任务完成」）；**有意的行为变更，需同步改写上述测试断言**（把钉死用例反转为 completed 必入队）。cancelled 保持静默（用户自己停的，不需要告知）。断言：running→completed 迁移后 `takeRunEndNotice()` 返回 title 含「完成」；cancelled 迁移不产生通知。规模 S。

### D2（高）失败通知只有应用内 toast，离开应用即永久错过；OS 通道闲置
现状：失败/受限 → `pendingNotices_` → `showAppNotice`（app_sidebar.cpp:56）应用内 toast 4.5s；不点开应用就看不到。而 `platform::notify` 的**系统通知后端已完整可用**（macOS UNUserNotificationCenter / Windows 托盘 NIF_INFO，notification.h），授权也在 `requestNotificationAuthorizationOnce`（993-997）请求了——但目前只有 LLM 主动调 app.notify 工具才会发系统通知，模块自己的 run 终态从不走它。
定性：缺反馈 + 通道错配（定时任务失败恰恰发生在用户不在应用前的场景）。
修法：failed/limited 迁移时在入队应用内 toast 的同时调 `platform::notify::backend().send({id:"agent-run-<runId>", title:"智能体任务失败", body:"「任务名」…"})`；稳定 id 复用系统替换语义。send 契约「受理≠展示」不阻塞、失败只 LOG。断言：注入 mock Backend（`setBackendForTest` 已有）→ 迁移失败 → `send` 恰被调一次且 `Notification.id` 前缀 `agent-run-`、body 含任务名；同一 runId 不重复发。规模 S/M。

### D3（中）最终回复是 12px 纯文本，markdown 全部裸奔
现状：assistant 回复按纯文本块渲染（1348-1362）；LLM 回复常态是 markdown（标题/列表/代码块），用户看到的是 `## 标题`、`- ` 原文。重设计 P4 覆盖折叠/复制/工具文案，不覆盖渲染。
定性：结果可读性（这是任务的交付物，不是日志）。
修法：详情消息区对最终 assistant 消息加「渲染」开关（默认开）：text 块走 `components::markdown`（能力表 ✅），切换保留原文视图供复制（复制语义不变，走既有 `messagePlainText`）。断言：含 `# x` 的消息渲染态不出现字面 `#`（节点为 markdown 组件子树）；切原文后文本 == 原文；复制内容两态一致。规模 M。

---

## 5. 任务编辑 / 删除 / 启停

### E1（高）任务根本无法删除——存储层就没有这个能力
现状：`AgentStore` 全表核对无 `deleteTask`/`removeTask`（agent_store.h:46-94）；UI 只有详情/编辑/启停/启动。建错的任务（或跑完的一次性任务）永远留在列表里，唯一处置是「禁用」——列表只增不减，一次性任务跑完变成一排僵尸。
定性：能力缺口，第一天就该有的操作。
修法：三层薄切片——① store `deleteTask(id)`：单事务删 `agent_tasks` + 级联 `agent_runs` + `agent_messages`（按 `task_id`/`run_id` 关联删除，无 schema 变更）；② 行内「删除」chip（`.edit` 旁，error 色 12px）；③ 点击先弹确认（`drawDialogFrame` 二层，红字「将删除该任务与全部运行记录，不可恢复」，确认钮 error 底）。正在运行的任务禁用删除入口（与 A5 同一 disabled 条件）。断言：建任务+2 run+若干消息 → 删除确认 → `listTasks` 不含该 id、runs/messages 关联行数为 0；点取消不删任何表；对 running 任务 `.delete` disabled。规模 M。

---

## 6. 通知与提醒（汇总视图）

现状全链：run 终态 → `detectRunEndNotifications`（只 failed/limited）→ `pendingNotices_` → 装配层 `takeRunEndNotice` → `showAppNotice` 应用内 toast 4.5s。成功静默（D1）、系统通道闲置（D2）、无通知历史（EUI-NEO toast 单实例无队列，能力表 ❌；自管队列成本高不做）。
「错过 toast」的兜底不靠通知中心，靠**列表常驻状态**：A2 的彩色点 + 状态文字就是持久化的通知（failed 点 error 色一直在列表上，直到下次运行覆盖）。此口径下不立独立 finding，D1+D2+A2 三件套即闭环。

---

## 7. 跨页一致性（对照 spec §6.5 token）

### G1（中）同页两套字号体系：11/12px 字面量 vs 13/15px token
现状：本页 11/12px 字面量遍布——状态列 12（1711）、错误行 11（2289）、字段标签 12（fieldCaption 210）、工具卡描述 11（1567）、消息正文 12（1354）；token 口径是 `fontSizeSmall=13` / `fontSizeBody=15`（theme.h:63-64，spec §6.5 正文 15）。重设计 D14 在详情弹窗内禁了 11/12——改造落地后**同一个功能里弹窗 13/15、列表和表单 11/12**，视觉语言撕裂。
定性：不一致，且是重设计自己制造的（修复它反而必要）。
修法：两批各 S——①列表与错误行：12→13、11→13（错误行 13 + `utf8Truncate` 控宽）；②任务弹窗字段标签/提示 12→13。断言：compose 后 `.status` 字号 == 13.0f；grep 守卫 `agent_module.cpp` 内 `fontSize(11` / `fontSize(12` 在列表/表单段命中归零（详情段由 D14 守）。

### G2（低）chip 圆角不合 spec pill 口径
现状：`actionChip` 用 `radius.overlay`（173），spec §6.5 chip = 999px pill。修法：`radius(999.0f)` 一行。断言：probe 量 `.bg` 圆角 > 16。规模 S。

### G3（低）页内错误行 16px 固定高不换行
现状：`agent.page.error` height 16 无 wrap（1469-1476），长错误硬截断；与全仓 Ellipsis 加固批（`8df78dc`，13 处接省略号）口径不齐——本行漏网。修法：`wrap(false)` + Ellipsis 省略号接法（同批口径），或双行高。断言：超宽错误文本不再被硬裁到无省略号（复用 8df78dc 批的断言手法）。规模 S。

### 豁免注记（防后人误改）
- 行内「启动」主按钮 32 高（1745）低于主按钮 40-44 口径：44px 行高约束下的有意取舍，40 放不进 `kTaskRowH=44` 的行。不算违规。
- `agent_ui_run_completed_silent_no_toast`、`agent_ui_run_failed_shows_toast` 是现行为契约：D1/D2 实施时**必须**同步改写这两个用例，不许删断言了事。

---

## 优先级排序（用户感知收益 ÷ 实现成本）

Top 5 = 今晚可派发粒度（每条一个独立 worktree 子任务，含测试）：

| # | 批次 | 含 findings | 规模 | 一句话理由 |
|---|---|---|---|---|
| 1 | **列表状态视觉批** | A2+A4+A5+A6 | S | 一屏读懂哪个任务在跑/坏了/停了；全是 compose 层改动，无 store 变更，收益/成本比最高 |
| 2 | **任务删除** | E1 | M | 建错只能禁用是死路；store+UI+确认三层薄切片，第一天就该有 |
| 3 | **停止按钮** | C1 | S/M | 误启动在烧钱，止血阀；cancel 原子位与 cancelled 状态机已存在，UI 是最后一块 |
| 4 | **失败系统通知** | D2 | S/M | 定时任务失败恰恰发生在人不在时；OS 后端与授权全部现成，只差一行 send |
| 5 | **表单语义提示批** | B1+B2+B3 | S | y=212 现成 hint 槽扩展到全类型+迭代说明；新手第一分钟从「瞎选」变「被告诉」 |

次优先（下一批）：A1 空态 CTA（S）→ A3 下次触发列（M）→ D1 成功通知（S，注意改钉死测试）→ B4 草稿恢复（S）→ G1 字号统一两批（S×2）。
再后：D3 markdown 渲染（M）→ B5 保存 toast（S）→ B6/G2/G3 打磨（S）。
不做：C3 侧边栏运行中点（L，跨模块，等真实痛点）；通知历史中心（toast 单实例 ❌，列表状态即持久通知）。
