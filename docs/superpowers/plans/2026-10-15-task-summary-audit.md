# 审计：9e60969 任务级 run 执行汇总（详情弹窗汇总行）

- 对象：`9e60969`（分支 `worktree-agent-task-summary`，基于 dev `1bb36c0`，6 文件 +426/−4）
- 审计者：独立只读 agent（非实现者），2026-10-16
- 手段：全 diff 精读 + 关联源码核对（sqlite_db / home 余量卡先例 / runRowText / 状态域）+ worktree 复跑
- 复跑证据：`DEVDESK_TEST_TMPDIR=/tmp/agent_audit_tmp2 bash scripts/build-test-run.sh --no-run --filter agent` → `Total: 90 / Passed: 90 / Failed: 0`

## Findings

| # | 位置 | severity | 问题 | 建议 |
|---|---|---|---|---|
| 1 | `agent_store.cpp:249-271` | info | 5 个重复的 `rowInt` 错误返回块（约 20 行模板） | 与文件既有映射风格一致，接受；后续若加聚合列可抽局部 lambda |
| 2 | `agent_module.cpp:1829-1960`（100/122/144/104/126/22/30） | info | 汇总行几何是魔法数，且与 `test_agent_ui.cpp` 断言值双份维护 | 项目政策要求的几何钉死（数值断言硬门禁），失败信息含实测偏差可定位；接受 |
| 3 | `agent_module.cpp:858-879` | info | 查询失败后 `runSummariesError_` 持续到下一次版本 bump 才有机会恢复；瞬态失败（理论上的 BUSY/进程外写）期间错误文案常驻 | 与 home 余量卡同口径（home_module.cpp:820-853 同模式），单连接同进程内写不走 BUSY 路径，接受 |
| 4 | `agent_store.h:40-41` | info | 成功率分母含 running/cancelled/limited，运行中任务成功率暂时偏低 | 结构体注释已明示口径（分母=全部 run，分子=仅 completed），有意的口径选择；接受 |

无 blocker、无 major。

## 六维结论

1. **SQL 正确性 — 已核，通过**。`SUM(status='completed')`：SQLite 比较结果为整型 0/1，组内 `COUNT(*)≥1` 故 SUM 恒非 NULL（agent_store.cpp:231）；running（`ended_at IS NULL`）在时长 CASE 显式计 0（:232-234）；回拨 run 负差钳 0，`ended_at` 仍参与 MAX（测试钉死 `lastEndedAt=1090`，语义=「最近一次有结果的结束时间」正确）；`MAX(ended_at)` 忽略 NULL，全未结束 → `row.isNull` → nullopt（:262-271）；分母口径见 finding 4。
2. **缓存竞态 — 已核，通过**。版本快照取在查询前（agent_module.cpp:864 先于 :866）：查询期间新写落库则记录的 ver 已陈旧 → 下一帧必重查，丢更新最多延迟一帧、不会永久丢失；同连接 recursive_mutex 使查询与写互斥、无撕裂读。与 home 余量卡先例逐行同款（home_module.cpp:827-853：:833 读版本 / :834 命中早退 / 失败显形 + LOG_ERROR）。`writeVersion()` = `atomic` acquire load，不触 sqlite3* 句柄（sqlite_db.h:122-124），无锁读符合单连接纪律；onStop 置 `runSummariesSeenVersionValid_=false` 防新连接版本从 0 起的陈旧命中（对应 home_module.cpp:244 的同款防御）。
3. **零 core 声明 — 已核，通过**。diff 6 文件全部在 `src/module_agent/` + `tests/test_modules/`，无 core/platform 触碰、无 CMakeLists 改动（即无链接面变化）。
4. **模块隔离 — 已核，通过**。新增 include 仅 `<ctime>`（agent_module.h / test_agent_ui.cpp 各一处）；无兄弟模块 include；`agent_module.h:24` 本就含 `agent_store.h`（`AgentTaskRunSummary` 无新依赖边）；脚本自带模块隔离守卫随复跑通过。
5. **测试有效性 — 已核，通过**。store 用例变异敏感（去掉负差钳 → totalSeconds 30 红；去掉 MAX 忽略 NULL → 红；口径改 → totalRuns/okRuns 红）；时间戳经直改 SQL 钉死绕开 finishRun 墙钟；DROP TABLE 用例为真：`SqliteDb::exec` 成功即 bump 版本（sqlite_db.cpp:353「裸 SQL 写入口一律置位」）→ 下次 refreshData 必重查必失败，若删错误显形分支则 summary 为 nullptr 断言红；几何断言（+122/+144/+104）符合项目「几何改动必须数值断言」政策，两分支算术自洽（runsTitleY+22=runlist 位、+30+runsH=msgsTitleY，新旧值换算逐项相等）；时区无关声明属实——UI 用例对时间只断言 `最近结果 ` 前缀，精确值只在 store 用例钉裸 int，纯函数用例传固定 `tm` 不调 localtime。
6. **静默失败面 — 已核，通过**。失败 → `runSummariesError_` + LOG_ERROR，汇总行以 error 色显形「运行汇总读取失败：<原因>」（agent_module.cpp:1841-1844）；`hasSummary` 把错误计入（:97-99），无 run 任务失败也显形；成功后 `runSummariesError_.clear()` 恢复；onStop 双清。残留见 finding 3（接受）。

## 行数/功能比

+426 中 239 行（56%）是测试；产品代码 ~187 行 = 3 个纯函数格式化器 + 缓存体 + 一条 GROUP BY 与逐列映射 + 弹窗接线 + onStop 清理，与 backlog 四指标（总数/成功率/总耗时/最近结果时间）一一对应，无超需求实现；`formatRunDurationText` 与既有 `runRowText` 的「Ns 裸秒」是不同展示面、非重复。

## 裁决

**放行** — 四项 backlog 指标全部落地且有变异敏感断言，缓存语义与 home 先例同款且竞态分析成立，零 core 与模块隔离属实，静默失败面已显形闭环；findings 全部 info 级，无需回修。
