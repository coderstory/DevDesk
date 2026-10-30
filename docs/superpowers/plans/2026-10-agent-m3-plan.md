# 智能体 M3（可用性收口）实现计划

> **2026-10-30 销账**：Task 2/3/4 已由其他线交付并双重复核（码农核查 + QA 独立复跑 dev@c6155a81：agent 定向 133/133、agent_ui 73/73、dialog probe 绿、隔离守卫绿），M3 全部完成，本计划不再有剩余开发量。下文复选框未勾属销账滞后，以本注记为准。

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** agent M3 = 决策记录（`docs/superpowers/specs/2026-10-agent-roadmap-decisions.md` §2）四项全做：① run 完成桌面通知（completed 可静默、failed/limited 必响）② cron 常用模板下拉 + 下次触发时间预览 ③ run 详情一键复制最终回复全文 ④ 代理未启/槽位解析失败时错误行带「去 Claude 页」直达引导。零 core 改动、零 migration（M3 不动 core，见决策 §1）。

**Architecture:** M3 全部收敛在 `module_agent` 层。现状锚点（写计划时实测核对）：
- 详情弹窗 `AgentModule::composeTaskDetailDialog`（agent_module.cpp:1366-1525）：run 错误行在 runlist 内逐 run 渲染（agent_module.cpp:1482-1491，id `agent.dialog.detail.runlist.<i>.err`）；消息流里最终 assistant 文本块默认展开（agent_module.cpp:1506 注释），已有**逐条消息**复制 chip（agent_module.cpp:765，actionChip `mid + ".copy"`）——但「最终回复全文一键复制」按钮不存在，是 ③ 的新增面。
- 页内错误行 `agent.page.error`（agent_module.cpp:918-928）：弹窗之外失败的唯一可见出口——④ 的第二落点。
- 「代理未启」「槽位解析失败」两类错误的原文产自 `LlmClient`：llm_client.cpp:56 `"代理未启，请先在 Claude 页启动代理"`、llm_client.cpp:63 `"无法解析模型槽位「" + model + "」（无激活 profile 或槽位未配置）"`——它们流入 run.error（agent_loop.cpp keep/收尾路径）再进两处错误行。④ 按**错误文本前缀匹配**判断是否渲染引导，不改 LlmClient 的错误文案（文案是测试锚点）。
- cron 表单：任务弹窗 `isSchedule` 分支内一个裸 input（agent_module.cpp:1261-1277，id `agent.dialog.task.cron`，`onChange` 只写 `editCron_`）；保存时 `parseCron` 校验（agent_module.cpp:520-527，非法 → `editError_` 不关窗）。**无模板下拉、无下次触发预览**——② 的全部是新增。
- cron 纯函数层 `cron.cpp`：只有 `parseCron`（cron.cpp:108）与 `cronMatches`（cron.cpp:131），**没有「下次触发时间」计算**——② 需新增纯函数 `nextCronMatch`（本地时间、从 now+60s 起逐分钟前探，硬上限防死循环）。
- run 终态感知：`AgentLoop` 收尾 `store_.finishRun`（agent_loop.cpp:328）；UI 侧 `refreshData` 轮询（agent_module.cpp:635 起）经 `uiPolling_` 刷新 `latestRuns_`/`taskRuns_`。① 的通知触发点选 **UI 轮询侧检测 running→终态迁移**（模块缓存已在手、不碰线程体），非 loop 回调。
- run 状态语义：`completed` / `failed` / `limited` / `cancelled` / `running`（agent_store.h:23，agent_module.cpp:54-61 `runStatusText`）。
- migration 现状：最新版本 = **v4**（migrations.cpp:223-233 `kMigrations[]`，最大 version 4）。M3 不加 migration；M4 的 context note 新列届时顺延为 v5（本计划仅作记录，不在 M3 实施）。

**Tech Stack:** C++20 / CMake / Ninja；自制迷你 doctest（REGISTER_TEST + DOCTEST_CHECK/REQUIRE，fail-fast）；EUI-NEO 组件（dropdown / input / toast / clipboard——**toast 是否可用见 Task 1 核实项**）；构建一律 `bash scripts/build-test-run.sh`。

**Spec:** 决策记录 `docs/superpowers/specs/2026-10-agent-roadmap-decisions.md`（§2 M3 范围 = 本计划唯一需求源）；设计 spec `docs/superpowers/specs/2026-09-30-agent-design.md`（§5.1 cron 语义「错过的触发不补跑」、§5.2 状态语义）。

## Global Constraints

- **M3 不动 core / platform / migrations**：四项全部落 `src/module_agent/`（含 ui_kit 可另议——若通知需 ui_kit 新组件，走 EUI-NEO 优先政策评估，见 Task 1）。核心铁律：core 不依赖 EUI-NEO、module_* 之间不互相依赖；改完跑 `bash scripts/check-module-isolation.sh` 必须绿。
- **不抛异常**：错误一律 `core::Result<T>`；线程体异常不逃逸（scheduler.cpp 既有 catch 模式）。
- **通知形态未定（实现期核实项，不是已定结论）**：**EUI-NEO 无系统通知 API**（`docs/EUI-NEO-CAPABILITIES.md` 盘点确认 ❌ 项；现支持 toast 但为单实例、无队列）。Task 1 开工先查 `../EUI-NEO` 能力清单与 `docs/EUI-NEO-CAPABILITIES.md` 决定形态（**toast vs 推动上游补系统通知**），按 EUI-NEO 上游优先政策：若走上游，先修 `../EUI-NEO` 再回来接线；若用 toast，接受单实例限制并写明。
- **cron 语义不破坏**：本地时间、5 字段、vixie 日/周或语义（cron.cpp:139-147）；`nextCronMatch` 是纯函数（无 IO 无时钟注入 `now` 参数），错过的分钟不补跑的既有语义与 M3 无关（预览只读）。
- **TDD**：每任务先失败测试后实现；测试名/断言按本计划，不为绿灯弱化断言。
- **构建**：一律 `bash scripts/build-test-run.sh`（禁手敲 cmake）；定向 `./build/tests/devdesk_tests <子串>`（如 `agent_cron` / `agent_ui`）；**用例总数以运行输出 `Total:` 为准，不在文档写死**；涉及可见 UI 的必须有 probe 或等价无头几何断言覆盖。
- **worktree + 提交纪律**：在 worktree 隔离分支实施（worktree 首建直接跑 `scripts/build-test-run.sh`，脚本自动定位 `../EUI-NEO` 与平台参数）；原子提交、显式 pathspec、禁 `git add -A`；完成合 `dev`（串行合并）。
- **本任务纯新增文档先行**：M3 计划本身已入库；执行时若发现锚点行号漂移（dev 在动），以符号名为准重新定位，不改计划文件里的历史行号注记。

## Review Focus（四类最易咬人的点）

1. **通知误报/漏报**：轮询检测 running→终态迁移时，用户开着详情弹窗（`taskRuns_` 刷新中）与关着弹窗两路都要覆盖；同一 run 只通知一次（迁移检测要防重复触发）；app 处于前台且用户正看着详情弹窗时 completed 静默的判定不能误杀 failed/limited 的必响。
2. **nextCronMatch 死循环/时区**：非法但 parseCron 通过的边界（如 `2月30日`）；前探步进必须设硬上限（如 366 天 × 1440 分钟），到顶返回「不可计算」而非死循环；用注入 now 测试，不依赖真时钟。
3. **复制全文 ≠ 最后一条消息**：最终回复可能是多 block（text + tool_use 混合，message_blocks.h）；「最终回复全文」= 最后一条 assistant 消息的**全部 text block 拼接**，不是最后一条消息原文 JSON、也不是逐条复制。
4. **错误行引导误匹配**：前缀匹配要钉住两类错误原文（llm_client.cpp:56 / :63），普通失败（如网络错误）不得渲染「去 Claude 页」；错误行有 detail 弹窗与 page 两处，漏一处就不算完成。

---

### Task 1: run 完成桌面通知（completed 可静默、failed/limited 必响）

**Files:**
- Modify: `src/module_agent/agent_module.h`（通知态成员：已通知 runId 集合等）+ `src/module_agent/agent_module.cpp`（refreshData 迁移检测 + 通知调用）
- 可能 Modify: `../EUI-NEO/`（上游补齐系统通知 API——若核实结论走这条路）
- Test: `tests/test_modules/test_agent_ui.cpp` 追加（迁移检测纯逻辑）+ probe（通知可见性，若形态为 UI 可见元素）

**Interfaces:**
- Consumes: `refreshData` 轮询路径（agent_module.cpp:635 起）与 `latestRuns_` / `taskRuns_` 缓存；`runStatusText`（agent_module.cpp:54-61）。
- Produces: 模块内私有「run 状态迁移检测」——本次轮询与上次轮询对比，`running → completed/failed/limited` 的 runId 集合；`completed` 按静默规则跳过，`failed`/`limited` 必发。
- **实现期核实项（开工第一件事，先核实再写实现）**：EUI-NEO 无系统通知 API。查 `../EUI-NEO` 能力清单与 `docs/EUI-NEO-CAPABILITIES.md`，在以下两种形态中定一个并留痕（写进本 Task 的提交说明）：
  1. **toast 方案**：用 EUI-NEO 既有 toast（单实例限制：连续多条通知会互相顶替，接受并注明）；零上游改动。
  2. **上游补齐方案**：按 EUI-NEO 上游优先政策（CLAUDE.md「EUI-NEO 上游贡献政策」），先在 `../EUI-NEO` 加系统通知 API（Windows `ToastNotificationManager` / macOS `NSUserNotification` 系），提交流程照政策（example + 文档 + DevDesk 验证），DevDesk 侧再接线。
  - 决策依据：系统通知的优势是「窗口最小化/不在前台也能看见」——这是 ① 的核心诉求；toast 只有窗口可见时才有意义。**若上游补齐成本可控，优先系统通知**；但这是实现期判断，不是本计划的既定结论。
  - **结论（2026-10-18 回写）：走形态 1 —— toast，且落地为「应用级」toast，不推上游补系统通知 API。** 落地面：`AgentModule::pendingNotices_` 队列（`agent_module.h:140`）由装配层取走后发 `app::showAppNotice`（`app_sidebar.cpp:56`，跨页可见）；**页内直发 toast 已在 fix round 裁决删除** —— 通知与页面无关（用户在别的页、开着别的弹窗时也须可见），且页内 toast 与页面自身 toast 共单实例会互顶。不推上游的理由：桌面通知要 macOS（`UNUserNotificationCenter` 系，含授权流程）与 Windows（`ToastNotificationManager`，含 AUMID/开始菜单快捷方式）两套原生桥，成本 ≫ 收益 —— run 只在 app 进程活着时产生通知，而 app 活着时窗口/任务栏已是可见面，系统级通知的增量只剩「窗口最小化且任务栏被遮挡」这一窄场景；上游无具体消费需求时先行造 API 也违背「有真实能力缺口才动上游」的前提（CLAUDE.md §EUI-NEO 上游贡献政策）。同 runId 只通知一次：`notifiedRunIds_` + `pruneNotifiedRunIds`（防集合随会话无界增长）。判据：`test_agent_ui.cpp` 迁移检测用例（completed 静默 / failed 必响 / 不重复）+ `pruneNotifiedRunIds` 用例；通知不经页内元素渲染，故无「通知元素存在性 probe」。
  - **残留边界（如实登记）**：app 未启动时不存在 run，故没有「退出后也要响」的形态问题；外部直写库插入 run 的冷启动盲区是另一条 backlog 条目（pollRunEnd 守卫三条件），与形态无关。

**验收:**
- [ ] failed/limited 的 run 结束后，用户不看智能体页也能感知（核实结论为系统通知时：窗口最小化可见；为 toast 时：窗口前台可见）。
- [ ] completed 不打扰（静默或不发，按核实的形态实现「可静默」）。
- [ ] 同一 runId 不重复通知。
- [ ] 测试：迁移检测纯逻辑用例（completed 静默 / failed 必响 / 不重复）绿；probe 或无头断言覆盖通知元素存在性。

- [x] **Step 1: 核实通知形态**（结论见上「结论」块：应用级 toast，不推上游）
- [x] **Step 2: 写失败测试**（迁移检测：上轮 running 本轮 completed → 不通知；上轮 running 本轮 failed → 通知一次；同 runId 二次迁移 → 不再通知）
- [x] **Step 3: 红 → 实现 → 绿**
- [x] **Step 4: 通知本体接线**（按 Step 1 结论：应用级 toast；零上游改动，EUI-NEO 侧未动）

---

### Task 2: cron 常用模板下拉 + 下次触发时间预览

**Files:**
- Modify: `src/module_agent/cron.h` + `src/module_agent/cron.cpp`（新增纯函数 `nextCronMatch`）
- Modify: `src/module_agent/agent_module.h`（模板表常量 / 预览文案成员）+ `src/module_agent/agent_module.cpp`（任务弹窗 `isSchedule` 分支扩展，agent_module.cpp:1261-1277 锚点）
- Test: `tests/test_modules/test_agent_cron.cpp` 追加（nextCronMatch 纯函数）+ `tests/test_modules/test_agent_ui.cpp` 追加（表单组合：下拉选中回填 / 预览文案）

**Interfaces:**
- Consumes: `parseCron`（cron.cpp:108）/ `cronMatches`（cron.cpp:131）；`core::time::localTime`（`src/core/time.h`，scheduler.cpp:175 先例）；任务弹窗既有布局契约（agent_module.cpp:173「cron 与触发同行、仅『定时』渲染、弹窗高度不变」）。
- Produces:
  - `core::Result<std::tm> nextCronMatch(const CronExpr& c, std::time_t fromSec)`——从 `fromSec + 60s` 起逐分钟前探（对齐「分钟翻转即触发」粒度，scheduler.cpp:125），命中即返回该分钟的本地 `std::tm`；**硬上限 366 天**（`366 * 1440` 次步进），到顶 `fail(ErrorCode::ValidationError, "一年内无触发时间")`。纯函数：不读真时钟（`fromSec` 全由调用方注入），可测。
  - 常用模板表（模块内常量数组）：「工作日 9 点 `0 9 * * 1-5`」「每天 9 点 `0 9 * * *`」「每小时 `0 * * * *`」「每周一 9 点 `0 9 * * 1`」（至少 4 条，文案与表达式成对）。
  - UI：`isSchedule` 分支内 cron input 旁加模板 dropdown（选中 → 回填 `editCron_`，复用 `setEditCronDraft`，agent_module.cpp:474）；input 下方一行「下次触发：<yyyy-MM-dd HH:mm>」预览——`editCron_` 每变一次：先 `parseCron`（非法 → 显示校验错误文案，替代/前置于保存期报错）；合法 → `nextCronMatch(now)` 显示；不可计算 → 显示「一年内无触发时间」。
- 布局契约：模板 dropdown 与预览行不得改变弹窗总高（`kDetailDialogH` 同族的表单高度常量与 `kPageGapCount` 减项联动处——agent_module.cpp:173/883 的同步注释规则）。若空间不足，预览行放 cron 行下方原 gap 内并同步修正子项计数。

**验收:**
- [ ] 下拉选模板 → cron 表达式回填 → 预览立即更新。
- [ ] 手改非法表达式 → 预览位显示校验错误（不等到保存才报）。
- [ ] `0 9 * * 1-5` 在周五注入 now → 预览为下周一 09:00（本地时间）。
- [ ] 无匹配表达式（如 `0 9 31 2 *`，2 月 31 日永不命中）→ 「一年内无触发时间」，不死循环不崩溃。
- [ ] 测试：nextCronMatch 纯函数 ≥4 用例（基本命中 / 跨日 / 星期语义 / 无匹配到顶）绿；表单组合用例绿。

- [ ] **Step 1: 写失败测试**（cron 纯函数四用例 + UI 表单两用例）
- [ ] **Step 2: 红**
- [ ] **Step 3: 实现 nextCronMatch → 绿**
- [ ] **Step 4: 实现模板下拉 + 预览接线 → 定向测试 + probe/无头几何断言绿**

---

### Task 3: run 详情一键复制最终回复全文

**Files:**
- Modify: `src/module_agent/message_blocks.h` / `message_blocks.cpp`（新增「提取 assistant 消息全部 text block 拼接」纯函数，若既有则复用）
- Modify: `src/module_agent/agent_module.h` + `agent_module.cpp`（详情弹窗消息流标题行加「复制回复」chip，agent_module.cpp:1508-1516 锚点）
- Test: `tests/test_modules/test_agent_ui.cpp` 追加（提取函数纯逻辑 + 弹窗组合）

**Interfaces:**
- Consumes: `detailMessages_`（`AgentMessageRow` 列表，agent_store.h:30-34）与消息流渲染路径（agent_module.cpp:1506 起）；既有逐条复制 chip 先例（agent_module.cpp:765 `actionChip(inner, mid + ".copy", "复制", …)` → clipboard）。
- Produces:
  - 纯函数 `std::string finalAssistantText(const std::vector<AgentMessageRow>& msgs)`——取**最后一条 role=assistant** 的消息，解析其 contentJson，按序拼接全部 `type=text` block 的 text 字段；无 assistant 消息或全无 text block → 空串。
  - 详情弹窗「消息（N）」标题行（agent_module.cpp:1508-1516）右侧加「复制回复」actionChip：非空才可点；点击 → `platform`/EUI-NEO clipboard 写入 `finalAssistantText(detailMessages_)` + LOG_INFO（照 :770 先例）。
- 边界：最终回复含多个 text block（中间夹 tool_use）时全部拼接，**顺序保持**；空串（run 刚起 / 全 tool_use）→ chip 禁用或不渲染（存在性测试判据）。

**验收:**
- [ ] 单 text block 回复：点击后剪贴板 = 该 block 全文。
- [ ] 多 block（text + tool_use + text）：拼接顺序正确、tool_use 不混入。
- [ ] 无 assistant 消息：chip 不可用。
- [ ] 既有逐条复制 chip 行为零回归。
- [ ] 测试：finalAssistantText ≥3 用例 + 弹窗组合用例绿。

- [ ] **Step 1: 写失败测试**（提取函数三用例 + 组合用例）
- [ ] **Step 2: 红 → 实现 finalAssistantText → 绿**
- [ ] **Step 3: 弹窗 chip 接线 → 定向测试绿**

---

### Task 4: 错误行「去 Claude 页」直达引导

**Files:**
- Modify: `src/module_agent/agent_module.cpp`（两处错误行：详情弹窗 runlist 错误行 agent_module.cpp:1482-1491 + 页内错误行 agent_module.cpp:918-928）+ `agent_module.h`（判定辅助）
- Test: `tests/test_modules/test_agent_ui.cpp` 追加

**Interfaces:**
- Consumes: 错误原文（llm_client.cpp:56 `"代理未启，请先在 Claude 页启动代理"`、llm_client.cpp:63 `"无法解析模型槽位「…」（无激活 profile 或槽位未配置）"`）；页面跳转通道——查既有跨页跳转先例（CLAUDE.md 记 Home 快捷按钮跳 Markdown 的 Router 通道；实际入口以 `core/router` 与既有跳转调用点为准，实现时 grep `Router` 跳转 API 落位）。
- Produces: 判定函数 `bool needsClaudePageHint(const std::string& errorText)`——按两处原文**前缀**匹配（`"代理未启"` / `"无法解析模型槽位"`），其余错误一律 false；两处错误行后追加「去 Claude 页」可点元素（复用既有可点 chip 契约：id 带 text 与 onClick，agent_module.cpp:2-4 文件头契约），点击 → Router 切到 Claude 页。
- 契约同步：详情弹窗 runlist 的 childCount/childrenH 计算（agent_module.cpp:1442-1453，error 行也是子元素）——引导元素若作为 error 行内文本而非新增子行，则不动计数；若新增子行，计数必须同步（注释 :1442 已警告）。

**验收:**
- [ ] run.error 为「代理未启…」→ 详情弹窗该错误行下出现「去 Claude 页」，点击跳转 Claude 页。
- [ ] run.error 为「无法解析模型槽位…」→ 同上。
- [ ] 页内错误行同两类错误 → 同样引导（弹窗外路径）。
- [ ] 其它错误（网络失败 / 落库失败等）→ 不渲染引导。
- [ ] 测试：判定函数正负用例 + 两处错误行渲染组合用例绿。

- [ ] **Step 1: 写失败测试**（判定正负 + 两处渲染）
- [ ] **Step 2: 红 → 实现判定函数 → 绿**
- [ ] **Step 3: 两处错误行接线 + 跳转通道 → 定向测试 + 无头几何断言绿**

---

### Task 5: 收尾（全量回归 + 交付）

- [ ] `bash scripts/build-test-run.sh` 全量（零回归，Total/Passed/Failed 与开工基线对比）
- [ ] `bash scripts/check-module-isolation.sh` 绿
- [ ] UI probe（`ctest -L probe`）绿或按三态门禁判定
- [ ] 截屏留痕（`--screenshot`）：任务弹窗含模板下拉 + 预览、详情弹窗含复制 chip 与错误行引导
- [ ] 原子提交（显式 pathspec）→ 审计 → 合 `dev`

---

## 备注：口径外事项（2026-10-15 三方对齐核查登记）

- **M4 前置（不阻塞 M3）**：migration 版本号两套口径冲突——spec §6 称 v12 / 本计划称 kMigrations 最大 v4（migrations.cpp:231）。M4 加 context note 列前必须核实 migrations.h 与 migrations.cpp 的真实版本链。已登记 `2026-09-22-backlog.md` §四点七。
- **执行汇总视图**：任务级 run 总数/成功率/耗时汇总不在 M3 范围（M3 四项不动）；数据已在 agent_runs 表，M3 后作为小增量候选评估。已登记 backlog §四点七。
- **Task 1 通知形态**：本计划唯一开放决策（toast vs EUI-NEO 上游补系统通知 API），实现期核实后定夺。
