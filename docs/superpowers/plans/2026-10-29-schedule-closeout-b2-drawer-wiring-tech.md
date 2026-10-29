# 技术方案卡：调度线收口批次 2 —— 运行历史抽屉接线（TODO §B 第 2 条）

- 日期：2026-10-29
- 角色：developer
- 规格参照：调度交接文档 §4.5（运行记录与失败重试）、§9 验收 7（run 成功/投递失败可分辨）、验收 14（`--filter schedule` 全绿）；抽屉本体 = S7b 交付（commit 33e90e0a，`schedule_ui_history.*` + 4 条纯装配用例），本卡只做接线，不新造 F-ID。

## F-ID / 验证矩阵（本卡即批次 2 台账；QA 复验后更新结果列）

| F-ID | 流程（验证什么能力） | 验证方法与证据物 | 结果 |
|---|---|---|---|
| F-W1 | 页头「运行历史」入口点击打开 S7b 抽屉（替换 LOG 占位） | `--filter 历史抽屉` → 6/6/0；`schedule_list_history_drawer_open_close_wiring`：点击后 panel opacity 0→1、`schedule.history.row.0/1` 渲染、historyItems 装载 2 条（planned_at 倒序、渠道汇总 1 行） | **待测试**（developer 自测已绿，见下方证据原文） |
| F-W2 | 抽屉内「关闭」钮收起抽屉 | 同上用例后半段：`schedule.history.close.bg` onClick → open=false、下一帧 opacity 回 0 | **待测试**（同上） |
| F-W3 | 零历史空态：新库点入口不崩、无残行 | `schedule_list_history_drawer_empty_ok`：空 run 表开抽屉，opacity=1 且 `row.0` 不存在 | **待测试**（同上） |
| F-W4 | 全量回归不回退 | `bash scripts/build-test-run.sh --no-run` → `Total: 1934 Passed: 1928 Failed: 0`，exit 0（基线 1932 + 本批 2 条） | **待测试**（developer 自测已绿） |

## 方案与参照

- **挂载点**：`ScheduleListUi::compose` 末尾追加 `ui::drawHistoryDrawer(...)`（关闭态常驻构建，`components::dialog` 自带 `.open(st.open)` + `onOpenChange` 回写）。schedule_module.cpp 的 compose 本就转发 listUi，宿主文件零改动（该文件本批禁触）。
- **状态与宿主**：`ScheduleListUi` 增 `ui::HistoryDrawerState historyState_` + `ui::HistoryDrawerHost historyHost_` 成员；构造时接好 `close = [this]{ open=false }`；`retryRun` 留空 = 抽屉隐藏重试按钮（S7b 头文件既定契约，引擎重跑入口随宿主波接）。
- **数据源（全部既有接口，零新查询）**：`store_.listRules()`（含归档行）→ 逐规则 `listRunsByRule(ruleId, 50)` → 逐 run `listDeliveriesByRun(run.id)` → 一次性 `ui::assembleHistory(runs, deliveries, 50)`（复用 S7b 纯装配做倒序/归组/截断）。listRules 失败 → 页面 error_ 红字且不开抽屉；单规则/run 级失败 → 降级已载入部分 + error_，抽屉照开。
- **断言方式参照既有先例**：弹窗开合断言 = `panel->opacity` 1.0/0.0（`test_claude_ui.cpp:1155` 同款）；按钮点击模拟 = `frame.find(id)->onClick()`（`test_todo_ui.cpp` 同款）；button 组件 onClick 挂在 `id + ".bg"` 子节点。

## 涉及文件

| 文件 | 改动 | 另一平台影响 |
|---|---|---|
| `src/module_schedule/schedule_ui_list.h` | include history 头；+`openHistory()`/`historyOpen()`/`historyItems()`；+2 私有成员 | 无平台代码，Windows 同源 |
| `src/module_schedule/schedule_ui_list.cpp` | 构造接 close 回调；openHistory 实现；按钮 onClick 换 `openHistory()`；compose 末尾挂抽屉；resetForTest 收抽屉；**随批申报（2026-10-29 审查补记）**：`specStringArray` 补数字元素分支（编辑器写数字数组、引擎约定 0=周日，旧代码静默丢弃数字元素致 weekdays 摘要丢词，经 weekdaysSummary 数字→星期名映射闭环） | 同上 |
| `tests/test_modules/test_schedule_ui_list.cpp` | +2 条接线用例（开/收 + 空态），include schedule_ui_history.h | 同上 |
| `tests/test_modules/test_schedule_rule.cpp` | **随批申报（2026-10-29 审查补记，非 DST 域两处）**：(a) biweekly 用例 CHECK_MESSAGE 由流式 `<<` 改字符串拼接——旧写法在现行 doctest shim（`oss << (message)` 括号强制整体先求值）下是硬编译错，HEAD 该文件本就编不过，属编译修复；(b) interval_anchor 空窗断言 `[20000,30000]`→`[20000,22599]`——missedFires 闭区间语义下旧区间必含网格点 {22600,26200,29800}，旧断言恒假，属数学修复非削弱 | 同上 |

CMake：无新文件，模块库已含 `schedule_ui_history.cpp`（S7b 登记），零改动。

## POC 结论

无需 POC：挂载/开合/点击模拟三条路径均有仓库先例（claude 对话弹窗、todo 卡片点击），首跑即绿（仅 1 处测试侧节点 id 修正 `.close` → `.close.bg`）。

## 取舍

- **系统项运行历史不在 v1 抽屉内**：系统项 run 的 `rule_id` 为空，store 无 origin_key 维度查询（新造查询被禁）；抽屉 v1 聚合用户+归档规则历史，系统项历史随来源注册波（S8）接。
- **抽屉空态无占位文案**：S7b 本体未含空态行，接线批不加（最小改动）；零历史时抽屉只有标题与关闭钮，可接受。
- **手动重试按钮隐藏**：retryRun 回调需引擎入口，本批文件所有权不含 engine，留空走 S7b 既定「可为空」契约。
