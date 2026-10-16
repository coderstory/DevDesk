# 审计：9378c42 pollRunEnd 冷启动盲区兜底（三守卫全假 → 15s 轻查询 EXISTS）

- 对象：`9378c42`（分支 `worktree-agent-pollrunend-fallback`，基于 dev `9751ce2`，5 文件 +144/−2）
- 审计者：独立只读 agent（非实现者），2026-10-16
- 手段：全 diff 精读 + 基/新双版本对照（三守卫逐值演进推演）+ 关联源码核对（sqlite_db 锁口径 / Statement::step / schedulerNow 缝既有用法 / app_sidebar 帧调用点 / schema 索引 / 既有 pollRunEnd 用例）+ worktree 复跑 + sqlite 查询计划实测
- 复跑证据：`DEVDESK_TEST_TMPDIR=/tmp/devdesk-audit-9378c42 bash scripts/build-test-run.sh --no-run --filter agent` → `Total: 92 / Passed: 92 / Failed: 0`（上一批审计同 filter 为 90/90，+2 即本次两条新用例）；定点 `./build/tests/devdesk_tests 兜底 --log` → 两条新用例各 `[PASS]`（3ms/2ms，无 SKIP），且命中行 `[DEBUG] Agent: 兜底轮询发现库内 running run…`（agent_module.cpp:864）实测可见

## Findings

| # | 位置 | severity | 问题 | 建议 |
|---|---|---|---|---|
| 1 | `agent_store.cpp:464-475`；`schema.h:316-317` | info | `WHERE status='running'` 无索引可走（现有 `idx_agent_runs_task(task_id, started_at DESC)` 不覆盖 status）→ `EXPLAIN QUERY PLAN` = `SCAN agent_runs` 全表扫；空命中（空闲常态）必须扫完全表，200k 行实测 ≈5–10ms/次，且全程持 `db_.mutex()` | 当前表量级小、可接受；下次 schema 批次补 `CREATE INDEX … ON agent_runs(status)`（或 `WHERE status='running'` 部分索引）即闭合 |
| 2 | `agent_module.cpp:849`（注释）、`:838-841` | info | 「兜底自动停摆」只在 run 存活期成立：命中 → prevAlive 接管（兜底不被调用）→ run 终结后无 alive → `uiPolling_=false`（:1032-1034）→ 下一帧三守卫复全假 → **兜底即恢复**。稳态 = 每 15s 一条 EXISTS 常驻（原「无 run 在身时零 DB 查询」变为空闲 4 条轻查询/分钟） | 设计上必然（外部直写无事件可订阅），**不要**加「静默 N 周期停」——停了就再次漏外部 running run；仅口径层面写实即可（:826「零常规查询」措辞已够） |
| 3 | `agent_module.cpp:850-866` | info | 起止都落在两个检查点之间的短命外部 run（寿命 <15s 且对齐不利）仍零通知：EXISTS(running) 是快照，两次都看不到它 → refreshData 永不跑 → 无迁移通知 | 轮询设计固有盲区；commit/注释只写了「最大延迟 15s」，未写此洞，建议注释补一句如实说明。补洞（按 `started_at > 已观测最大启动时刻` 二次判据）成本大于收益，不建议本批做 |

无 blocker、无 major。

## 核查点逐条

1. **兜底链路 — 已核，通过**。三守卫全假 → `pollRunEndFallback()`（:839）→ 节流（:853-857；`lastRunFallbackSec_==0` 首帧必查）→ `store_->hasRunningRun()`（EXISTS：`SELECT 1 … LIMIT 1` + 单次 `step()`）→ 命中 `refreshData()`（:865）→ 该 run 进 `prevRunStatuses_` → 后续帧 prevAlive=true 走常规 refreshData → 终态迁移产生通知。**无循环自激**：兜底仅由「三守卫全假」进入，命中后改由 prevAlive 分支承载，`lastRunFallbackSec_` 只在兜底内推进且只增不减；兜底不写库、与查询同锁互斥。**停摆口径**见 finding 2（仅 run 存活期停）。
2. **单 sqlite 连接纪律 — 已核，通过**。`hasRunningRun()` 首行 `std::lock_guard<std::recursive_mutex> lock(db_.mutex())`，与文件内其余 14 处逐字同款（全文件 `db_.mutex()` 的非标准锁用法仅文件头注释 1 处，grep 全量核实，无绕过）；Statement 自身亦持同锁（同线程递归重入安全）；声明序 lock 先 / st 后 → 语句析构先于锁释放。
3. **节流时钟 — 已核，通过**。缝 = `Deps::schedulerNow`（`Scheduler::NowFn = std::function<int64_t()>`，agent_module.h:100），与既有用法同款（同文件 `refreshCronPreview` :608、Scheduler 构造 :469）；测试可冻结声明**属实**——throttle 用例注入常量 lambda 并在 `5000→5016`（跨 15s 窗口）后实测计数 1→2，非恒真。缺省 `core::time::nowUnix()`（time_t→int64_t，两平台均 64 位无损）。
4. **静默失败面 — 已核，通过**。查询失败 → `LOG_WARN` + return，不吞不置位、下周期自然重试（:858-861）；命中行 LOG_DEBUG 实测可见（测试侧 `--log` = Debug 级 + 控制台；生产侧需设置页日志级别切 DEBUG，默认档不显示——但命中路径本身有 toast 兜底可见，DEBUG 仅诊断用，可接受）。
5. **测试有效性 — 已核，通过（附一条复现前提）**。断言均内容级：cold_start = notice 存在 + 标题含「失败」+ 正文含任务名（删兜底 → 无 notice → REQUIRE 红）；throttle = 计数 1/1/2 精确钉窗口语义（去节流 → 红；窗口改 0/过大 → 红），另断言无迁移不得产生通知（防兜底查询自激假通知）。**修前红**：cold_start 的语义红在 base 上逐守卫核实为结构必然（base 早退 → prevRunStatuses_ 恒空 → 通知恒空 → REQUIRE_MESSAGE 红）；但「字面复现」有一前提——新测试引用新 public seam `runFallbackChecksForTest()`（agent_module.h:121），整份 test hunk 直贴 base 会先撞**编译错**，需只保留 cold_start 用例再跑才得 `Failed 1`。我未实测（只读复跑限允许命令，未改被审树）。
6. **回归面 — 已核，通过**。早退路径改动 = 老两行 `if (…) return; refreshData();` 等价重排（−2 即这两行；非兜底分支语义逐字不变）。调用点仅 `app_sidebar.cpp:246`（每帧，主界面后）+ 页内 compose；既有用例仅 `agent_run_end_notice_take_semantics` 两处调用（test_agent_ui.cpp:1789/1795，两次都走非兜底分支：首帧 tasksDirty_ 真，次帧 prevAlive 真），`--filter agent` 全量 92/92 实测绿。行为差异只落在「三守卫全假 + 库内 running run」这一此前无覆盖的组合（既有用例两次调用分别由 tasksDirty_ / prevAlive 放行，从未走到兜底分支）。
7. **行数比 — 合理**。+144 = 测试 86（两条用例，长注释与文件风格一致）+ 实现 58（module 30 含契约注释 / header 12 / store 13+3）。实现 = 一次 EXISTS + 一个节流时间戳 + 一个计数测试缝，无多余抽象、无配置项膨胀。

## 六维结论

1. **正确性 — 已核，通过**（核查点 1；对照原 bug「外部直写库 running run 的终态迁移零通知」闭环，含命中后 prevAlive 接管的交接）。
2. **回归面 — 已核，通过**（核查点 6；92/92 实测绿，早退分支语义等价）。
3. **平台对称性 — 已核，通过**。纯逻辑改动：零平台分支、零新 include、零 CMake 触碰（int64_t/time 全走既有 core::time）；本机 macOS 编译验证，Windows 未本地验证（走 CI，如实登记）。
4. **静默失败面 — 已核，通过**（核查点 4）。
5. **测试有效性 — 已核，通过**（核查点 5；修前红的复现前提已写明，非恒真断言）。
6. **文档口径 — 已核，通过（两处措辞可更严）**。注释 / commit 与实现一致；「兜底自动停摆」（finding 2）与「短命 run 盲区未写明」（finding 3）为 info 级措辞问题，不改行为、不阻断。

## 行数/功能比

+144 中 86 行（60%）是测试；产品代码 58 行 = 30 行 module（含 ~10 行契约/反重构注释）+ 13 行 store 查询 + 15 行头文件（常量 / 测试缝 / 两个状态位）——与「EXISTS + 15s 节流 + 两条测试」一一对应，无超需求实现，符合「避免代码膨胀」口径。

## 裁决

**放行** — 兜底链路逐环核实成立且无自激，单连接锁口径与 store 既有 15 处逐字一致，节流时钟缝用法与既有模式同款且测试冻结实测有效，失败面 WARN 不吞 + DEBUG 实测可见，两条新用例为内容级断言（throttle 计数精确钉窗口）、92/92 实测全绿，行数比合理。findings 三条全 info 级（status 索引 / 「停摆」措辞 / 短命 run 盲区），无需回修；前两条建议随下次 schema 或注释批次顺带处理。
