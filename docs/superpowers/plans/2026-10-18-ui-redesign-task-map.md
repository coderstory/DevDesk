# 智能体详情弹窗重设计 — 任务卡地图（只读分析产物）

> 日期：2026-10-18 ｜ 性质：派单前地图，不含任何代码改动
> 依据：`docs/superpowers/specs/2026-10-18-agent-detail-dialog-redesign.md`（739 行，下称 spec）
> 现状基线：dev `6e1319c`。**所有 agent_module.cpp 行号均为该基线的行号**，实施分支快进到更新的 dev 后行号会漂移，以符号名为锚、行号作起点。
> spec 自身分 P1–P5 五阶段（§13）；本文按派单口径合并为三张卡：P1=布局壳（spec P1）、P2=时间线+run 详情（spec P2+P3）、P3=配置折叠+消息（spec P4）。spec P5（新用例补全/探针/全量回归/Windows 构建）不设独立卡，作为各卡验证口径的收尾由主会话跑全量。

---

## 1. 现状对照表（spec 改造点 vs 现状代码）

### 1.1 布局壳

| spec 改造点 | 现状 | 位置 |
|---|---|---|
| D1 按实际钳制尺寸排版（`actualW/H = min(760/660, screen-48)`） | 内容几何全部用**请求宽**算：`w = dialogWidthPx(Form)`（metrics.h:35-36 = 760），`innerW = w - kDialogBodyInsetX`（metrics.h:41 = 56） | agent_module.cpp:2117-2120 |
| D1 高度口径 | compose 侧钳 `screen.height - 40`；EUI-NEO dialog 实际钳 `min(h, screen - metrics.spacing.overlay)` | agent_module.cpp:2118；EUI-NEO components/dialog.h:81-82 |
| D2 宽/窄分支（`wide = actualW>=680 && actualH>=520`） | 无任何分支；单列绝对 y 排版 | agent_module.cpp:2131-2392 |
| D15 `.info` 退休 | 单条 12px 灰字拼串（目标/触发/cron/次数/槽位/迭代/破坏性/异常原因） | 组装 :2148-2180，渲染 :2181-2190（y=60，h36） |
| D15 `.summary` 文本改容器 | all-time 汇总单行文本「共 N 次 · 成功率…」，有 run 或查询失败才渲染 | :2197-2226（y=100，h18）；文案拼装 formatTaskRunSummaryText :117-125 |
| D15 `.runs.title` → `.runlist.title` | 「运行记录（最近 N 次）」标题，y 随汇总行跳 104/122 | :2200，:2227-2237 |
| D5/D8 详情任务查 20 条 | `kRunListLimit = 5`，所有任务统一查 5 条 | 常量 :290；查询 :1115 |
| §8.7 新状态成员（messagesOpen_/configOpen_/detailTab_/toast_ 等） | 不存在；成员区在 | agent_module.h:244-254、:313-316 |
| §7.3 页级最后画 toast | compose 尾部只画两张弹窗（任务、详情） | agent_module.cpp:1763-1768 |
| Esc 关最上层 | 详情优先，已实现（保留项） | :2396-2408 |

### 1.2 时间线（现状 = 无时间线）

| spec 改造点 | 现状 | 位置 |
|---|---|---|
| 按天分组 `.runlist.day.<k>` | 无分组概念 | — |
| `.runlist.<i>` 容器 + `.bg/.dot/.time/.status/.duration` 子节点 | 每行是**单个 text 节点**，onClick 直挂文本上，内容 = `runRowText` 长串「状态 · 耗时 Ns · 迭代 N · token N」 | 行渲染 :2262-2284（onClick :2276-2283）；runRowText :139-154 |
| `.runlist.scroll` 独立滚动 | 无滚动容器，行数多了直接撑高（弹窗内静态排） | :2257-2309 |
| 状态圆点/颜色 token | 无圆点；选中仅文字变 primary 色 | :2266-2275 |
| `.runlist.note` 20 条提示 | 不存在 | — |
| 行高 52/68/84（fs13 派生） | 行 18px（fs12）、err 行 16px（fs11） | :2269、:2287、:2289 |
| `agent_time_text` helper（§11） | 分散在 module 匿名/具名函数：runStatusText :130-137、formatRunDurationText :91-107、formatRunResultTimeText :109-115（YYYY-MM-DD HH:MM，无秒） | 待迁移/退役 |
| 失败原因 `utf8Truncate(...,48)` 一行摘要 | `.err` 全文单行 wrap(false) fs11；`.err.hint` 仅 LLM 配置类 | :2285-2305；needsClaudePageHint :60-63 |
| 空态「从未执行」 | 「（还没有运行记录）」 | :2310-2318 |

### 1.3 run 详情（现状 = 无右栏）

| spec 改造点 | 现状 | 位置 |
|---|---|---|
| `.detail.status/.meta/.schedule/.error` 右栏 | **不存在**；耗时/迭代/token 混在时间线行文本里 | runRowText :139-154 |
| 计划 vs 实际（previousCronMatch） | cron 只有 `nextCronMatch`；预览行已示范「只在变化点重算」模式 | cron.h:32；同款节流注释 agent_module.cpp:679-681 |
| 消息默认折叠（D6 messagesOpen_） | 消息**默认全部渲染**；仅块级折叠——最终 assistant 文本块被 `applyDefaultMessageExpansion` 无条件展开 | :951-971；装载点 :1245 |
| `.msgs.toggle` 区级开合 | 不存在（消息区常开） | — |
| `.msg.<i>.time` 消息时间 | 无时间显示；`AgentMessageRow` 无 createdAt 字段 | agent_store.h:32-36 |
| 工具 chip 文案「工具 · name」（去 emoji） | `"🔧 " + toolName` | :1367 |
| 消息字号 13/20 | badge fs11、正文/预览/块 fs12、行高 18 | :1278、:1292-1300、:1321-1331、:1350-1395 |
| `.msgscroll` 高度 ≥96 降级 + contentKey | 已有 `.msgscroll`（min96/max520 钳制）+ contentKey（version+n）；无 fs 维度 | 常量 :293-294；scroll :2357-2390（contentKey :2381-2383） |
| `estimateMessageHeight` 接字号 | 现签名：`estimateTextBlockHeight(text,w,fs,lh)` 已有双参；`estimateMessageHeight(msg,expanded,w,lh)` 与 `estimateMessagesAreaHeight` 只有 lineHeight 无 fontSize | message_blocks.h:53-64；调用 :1278、:2360-2362 |

### 1.4 配置折叠（现状 = 无折叠）

| spec 改造点 | 现状 | 位置 |
|---|---|---|
| `.config.header` 40 高常驻 + 浮层 | 不存在；配置全部挤在 `.info` 单行 | 见 1.1 |
| `.config.scrim` z19 / `.config.popover` z20 | 无浮层；EUI-NEO `.zIndex(int)` 可用 | 能力文档 :147 |
| 分段字段 `.config.body.<field>.label/.value` | 无 | — |
| 复制 toast（D13） | 复制只 LOG 留痕，无可见反馈；`ui_kit::drawToast/ToastState` 已存在可直接用 | :1310-1316、:2347-2353；src/ui_kit/toast.h:16,48 |
| `openTaskDetail` 清零清单（§8.7） | 现只清 selectedRunId_/消息/展开态/版本号 | :928-937 |
| `closeTaskDetail` 不清缓存 | 已符合 | :940-948 |

### 1.5 数据层

| spec 改造点 | 现状 | 位置 |
|---|---|---|
| `AgentTaskRunSummary` + 7d 三字段 + `listTaskRunSummaries(windowStartSec)` | 只有 all-time 四字段；签名无窗口参 | agent_store.h:38-45、:85 |
| `AgentMessageRow.createdAt` + SELECT 加 created_at | 字段不存在；appendMessage 已写列（无需迁移） | agent_store.h:32-36 |
| 汇总缓存键 = writeVersion + windowStartSec | 只有 writeVersion 单键 | agent_module.cpp:1061-1079（:1063-1064） |
| `refreshData`：列表查 1 条、详情任务查 20 条 | 全部查 `kRunListLimit`(5) 条进 `taskRuns_` | :1114-1133 |
| 通知/判停链依赖 latestRuns_ | detectRunEndNotifications :1181、hasLiveRun :1262、pageErrorRunIds_ 剪枝 :1140-1156——limit 改 1 后语义仍成立（只用 front），是回归敏感点 | :1140-1209、:1249-1266 |

---

## 2. 任务卡

### 卡 P1 — 布局壳（spec §13 P1；依赖：无，但受 T7b 排序约束见 §4）

- **改动文件**：`src/module_agent/agent_module.h`（§8.7 成员）、`src/module_agent/agent_module.cpp`（重写 composeTaskDetailDialog 骨架 :2113-2393）、`tests/test_modules/test_agent_ui.cpp`（几何断言替换 §12.2）。**禁碰任务弹窗区**（:1772-2111，T7b 领地）。
- **内容**：actualW/H 复算（含钳制口径裁决，见风险 R-A）、wide 判据、`.summary` 改三卡容器、`.config.header` 常驻头（只画头，浮层在 P3）、`.runlist.title/.runlist.scroll` 骨架 + `.detail` 骨架、窄屏 `.nav`（components::segmented）、`.info`/`.runs.title` 退休、openTaskDetail/closeTaskDetail 清零清单。toast 成员与绘制**建议挪 P3**（复制 toast 的触发点在 P3 才存在；这样 P1 不碰 compose() 尾部，规避 T7b 冲突）。
- **规模**：M
- **断言风险点（动了必红）**：
  - 绝对 y 断言：`agent_ui_detail_dialog_run_summary`（test_agent_ui.cpp:3224）——`panel.y+122`（:3283）、`panel.y+144`（:3287）、无汇总 104/126；spec §12.2 要求改相对关系。
  - `.info` 存在断言：:1037、:1133-1134（连同 `agent_ui_once_detail_shows_alarm_label_and_time` :731 的闹钟摘要文案路径，闹钟信息迁入 `.config.body`）。
  - `.runs.title`：:1855-1861（`agent_ui_detail_dialog_geometry_and_empty_state` :1800 内）。
  - 消息区底 `panel-19.5`：:1684、:1851（spec 改为 -24 口径）。
  - 文件头 ID 契约注释 :13-17（含 `.info`/`.msgscroll` 旧契约）。
- **验证口径**：`bash scripts/build-test-run.sh --filter agent_ui_detail`；probe 必跑：`./build/tests/agent_dialog_probe`（A/B 流程须仍绿，B 流程在 tests/probes/agent_dialog_probe.cpp:297-361）；新增三尺寸无头用例（760×660 / 700×560 / 900×600，spec §12.3 最后四条依赖 P1 骨架先行）。

### 卡 P2 — 时间线 + run 详情（spec §13 P2+P3；依赖：P1 合入）

- **改动文件**：新增 `src/module_agent/agent_time_text.h/.cpp` + 进 `src/module_agent/CMakeLists.txt` 显式源列表；`src/module_agent/agent_store.h/.cpp`（7d 字段、窗口参数、createdAt SELECT）；`src/module_agent/cron.h/.cpp`（previousCronMatch）；`src/module_agent/agent_module.h/.cpp`；`tests/CMakeLists.txt` + 新增 `tests/test_modules/test_agent_time_text.cpp`；改 `test_agent_store.cpp`、`test_agent_ui.cpp`、`test_message_blocks.cpp`。
- **内容**：时间线行容器化（bg/dot/time/status/duration 子节点，onClick 落 `.bg`——文件头「chip 手搓两层」契约同款）、按天分组、20 条 + `.runlist.note`、7d 统计查询 + 缓存双键、describeNextRun/describeRunPlan、`.detail.meta/.schedule/.error` 右栏、formatRunResultTimeText/formatTaskRunSummaryText/runRowText 退役。
- **规模**：L
- **断言风险点**：
  - **1970 年时间夹具**：`agent_ui_detail_dialog_run_summary` 用 `UPDATE agent_runs SET started_at=1000/1100/1200`（test_agent_ui.cpp:3250-3254）——7d 窗口下全部落空，spec §12.5 要求改 `now-1h/1d/8d`。这是 P2 最隐蔽的红。
  - runRowText 文案断言：:1143-1152（runlist.0 含「成功」「token —」、runlist.1 含「205000」）——改 `.runlist.<i>.status/.duration`，token 断言迁 `.detail.meta`。
  - `agent_format_task_run_summary_text` :3205（被退役函数的直测，:3213/:3217 文案）——整测删除或迁移。
  - formatRunDurationText 直测 :3180-3188——include 从 agent_module.h 迁 agent_time_text.h，文案契约（含「0秒」回拨钳）不变。
  - `test_agent_store.cpp:246` 聚合用例——listTaskRunSummaries 加参后同步。
  - 通知链回归：limit 5→1 后跑 run 结束通知用例（Task 1 / takeRunEndNotice 路径）。
- **验证口径**：`--filter agent_ui_detail agent_time_text agent_store cron`（新用例名见 spec §12.3 前 9 条）；probe B 增加 `.runlist.0.time` 格式断言（spec §12.4）。

### 卡 P3 — 配置折叠 + 消息 + toast（spec §13 P4；依赖：P1（浮层几何）、P2（时间线定稿后同函数收尾））

- **改动文件**：`src/module_agent/agent_module.h/.cpp`；`src/module_agent/message_blocks.h/.cpp`（estimate 系接 fontSize）；`tests/test_modules/test_message_blocks.cpp`、`test_agent_ui.cpp`；`tests/probes/agent_dialog_probe.cpp`。
- **内容**：`.config.scrim/.popover/.popover.scroll` + 分段字段；消息默认折叠 messagesOpen_（applyDefaultMessageExpansion 降级为「展开态内规则」）；`.msg.<i>.time`；工具 chip 文案；复制 toast（ui_kit::drawToast + `toast_` 成员，页级最后绘制——此卡才碰 compose() 尾部）。
- **规模**：L
- **断言风险点**：
  - **默认展开断言全面翻改**：`.msg.1.blk.0` 默认可见 :1063-1065、:1163、:1407、:1427（`agent_ui_detail_dialog_close_and_reopen` :1364 的重开断言）——按 spec §12.2 改「先点 `.msgs.toggle`」，并新增「默认无 `.msgs.body`/`.msgscroll`」反向断言。
  - `.msgs.copyfinal` 语义保持（折叠态可见、无回复不渲染——现状条件 :2337 与 spec 12.1 一致，勿「顺手」改渲染条件）：:1323、probe :353。
  - `agent_ui_detail_message_toggle_and_copy` :1178 的 msg.2 高度比较——先展开消息区再比。
  - estimate 系签名变化：test_message_blocks.cpp:230-277（12/18 → 13/20）、:340-352。
  - probe B 稳定帧构图（agent_dialog_probe.cpp:361）——消息折叠后 B 帧元素集变化，截图断言同步。
- **验证口径**：`--filter agent_ui_detail message_blocks`；probe 全流程 + `--shot-detail` 宽屏截图 + `--window-size 700x560` 窄屏断言（spec §12.4）；收尾主会话跑全量 + Windows 构建（spec P5）。

---

## 3. 风险清单

### 3.1 spec 与现状矛盾 / 口径差

- **R-A（最高优先，P1 开工前必须裁决）高度钳制三口径不一致**：compose 侧 `screen.height - 40`（agent_module.cpp:2118）≠ spec D1 `screen - 48` ≠ EUI-NEO 实际钳 `screen - metrics.spacing.overlay`（EUI-NEO components/dialog.h:81-82）。spec 的意图（A2）是「按实际钳制后排版」，但 DevDesk **拿不到 dialog 实际尺寸**（drawDialogFrame 内部直接 `.size(frame.widthPx(), frame.height)`，src/ui_kit/dialog_form.cpp:14-15 无回读）。P1 只能在 DevDesk 侧复算同一公式——`48` 须与 EUI-NEO `metrics.spacing.overlay` 对齐，存在上游改值漂移风险。
- **R-B（EUI-NEO 能力缺口，唯一一个）dialog 实际尺寸 / overlay token 不暴露**。spec §1.6「必须按 dialog 实际钳制后的尺寸排版」依赖它。替代方案（二选一）：① DevDesk 侧复算 `min(760, screen-48)` 纯函数 + 注释钉住 48 的出处（本批推荐，零上游依赖）；② 走 EUI-NEO 上游 PR 暴露实际尺寸或 overlay token（政策允许但周期长，本批不等）。
- **R-C spec §12.2 对 estimate 系的描述与现状半符**：spec 写「补 fontSize/lineHeight 参数」，实际 `estimateTextBlockHeight` 已有双参（message_blocks.h:53-54），缺的是 `estimateMessageHeight/estimateMessagesAreaHeight` 的 fontSize（现只有 lineHeight，:57-64，fs 12 在调用点写死）。实施按「AreaHeight 补 fontSize + 全部调用点 12/18→13/20」理解，不要重造签名。
- **R-D `.summary` 渲染条件差异**：现状「无 run 且无查询失败则整个 summary 不渲染」（:2198-2199）；spec D9/§10 摘要三卡**常驻**（空态显示「近 7 天暂无执行」）。`agent_ui_detail_dialog_run_summary` 的「无 run 几何回到 104/126」分支整支作废。
- **R-E 通知链隐性耦合**：`latestRuns_` limit 5→1（§8.6）后，run 结束通知（:1175-1209）、判停（:1249-1266）、pageError 剪枝（:1140-1156）语义不变但全部失去冗余——store 排序契约（started_at DESC, id DESC）从「纵深防御」（:1121-1128 注释）变成「唯一依赖」。测试要覆盖这条。
- **R-F 任务弹窗几何契约冻结**：T7b 明言弹窗 500 高有 3 处常量断言 + 2 处 `+224` 断言（2026-10-18-agent-progress-snapshot.md:144）；agent_module.cpp:1936-1937、:1971 注释「不许动」。P1–P3 全程禁碰 `composeTaskDialog`（:1772-2111）。

### 3.2 EUI-NEO 能力对照（spec 用到的组件逐项核对 docs/EUI-NEO-CAPABILITIES.md）

| spec 依赖 | 结论 | 证据 |
|---|---|---|
| `.nav` 用 `components::segmented`，生成 `.hit.<i>/.label.<i>` | ✅ 在清单（:18/:86），且 ID 生成与 spec §7.2 逐字一致 | EUI-NEO components/segmented.h:99,113 |
| `.config.popover` z20 / `.scrim` z19 | ✅ `.zIndex(int)` 在能力表 | 能力文档 :147 |
| `.runlist.scroll/.msgscroll/.detail.scroll` | ✅ scrollView + contentKey（现状已用） | 能力文档 :116；agent_module.cpp:2381 |
| 复制 toast（D13 `ui_kit::drawToast`） | ✅ DevDesk ui_kit 本地已有，无需动 EUI-NEO；EUI-NEO toast 单实例无队列（:100）与本批单 toast 用法无冲突 | src/ui_kit/toast.h:16,48 |
| 状态色 success/warning/info/primarySoftBg/hover/surfaceElevated/borderDefault/textTertiary | ✅ 全部在主题 token | src/core/theme.h:20-41 |
| ❌ → R-B：dialog 实际钳制尺寸/overlay 常量 | **不在 ✅ 清单，无现成 API** | 见 3.1 R-B |
| wrap(true)/utf8Truncate/clipboard void | ✅ 现状已用（:2187、:1564、:1312） | — |
| fontScale 派生行高（§9.4） | ✅ `core::theme::setFontScale` 契约沿用 | spec §9.4 自证 |

---

## 4. 依赖序与 T7b 交叠

### 4.1 在飞态势（2026-10-18 实测）

- 快照文档排队（2026-10-18-agent-progress-snapshot.md §五）：**T7a 收尾 → T3c2 → T7b → UI P1 → UI P2/P3**——T7b 被显式排在 UI P1 **之前**。
- 三个在飞 locked worktree（`agent-a2bbfe…` 停 28bb89c、`agent-aa1aa7…`/`agent-aae4ff…` 停 6e1319c，均无领先 dev 的提交）；worktree 隔离无法窥探其未提交改动，T7b 交叠按快照文档口径分析。
- T7b 范围：**任务弹窗**（`composeTaskDialog` :1772-2111）标题行「AI 生成草稿」chip + 子弹窗输入 + 异步生成 + 预填。动 `agent_module.cpp`（任务弹窗函数 + openNewTaskDialog/edit 草稿成员）、`agent_module.h`（草稿/异步成员）、`compose()` 尾部（:1763-1768 邻接处加第三张子弹窗绘制）、`test_agent_ui.cpp` 任务弹窗段。

### 4.2 交叠判定：P1 是否碰 agent_module.cpp 同区域？

**同文件、不同函数——函数级不相交，但有三个共享缝合点**：

| 缝合点 | T7b 写 | P1 写 | 冲突度 |
|---|---|---|---|
| `agent_module.h` 成员声明区（:244-254 邻域） | 草稿/异步生成成员 | §8.7 详情状态成员 | 低——追加式，git 可自动合并；语义无交叠 |
| `compose()` 尾部弹窗绘制序列（:1763-1768） | +1 行子弹窗绘制 | 若 P1 接 toast 绘制则同 5 行区域 | **中——唯一真冲突点** |
| `test_agent_ui.cpp` | 任务弹窗段（新增 chip/子弹窗用例） | 详情弹窗段（几何断言替换） | 低——同文件不同段 |

### 4.3 排序建议（避开单写者冲突）

1. **维持快照队列：T7b 先合 dev，P1 后动**。这是硬约束的首选满足方式。
2. **若 T7b 与 P1 必须并行**：P1 卡**不得接 toast 绘制**（挪到 P3——复制 toast 的触发点本就在 P3），并把这条写进 P1 派单 prompt。如此 P1 对 agent_module.cpp 的写入收敛在 `composeTaskDetailDialog`（:2113-2393）+ `openTaskDetail/closeTaskDetail`（:928-948）+ 常量区（:287-294），与 T7b 的任务弹窗区零重叠；agent_module.h 的成员区冲突靠「各追加各的」自动合并。
3. **P1 开工前必须 `git merge --ff-only dev` 快进**（把 T7b 已合入部分收进来），收工前再快进一次解决 h 成员区可能的文本冲突。
4. **P2 与 P3 不可并行**：两者都大改 `composeTaskDetailDialog` 函数体（同函数两写者必撞）；且 P3 的浮层几何依赖 P1 的 configY/mainH、消息折叠依赖 P2 定稿的右栏高度分配。顺序 P1 → P2 → P3 串行。
5. 在飞期间本地图文档的合并照常走 dev 串行 merge（本 commit 只有这一个新文件，与 T7b 的 pathspec 天然不相交）。
