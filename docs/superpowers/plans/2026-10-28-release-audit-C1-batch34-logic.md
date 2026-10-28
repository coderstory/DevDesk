# 上线模块审计 C1：批次 3+4 新增/改动的逻辑与数据层

> 审计日期：2026-10-28
> 工作目录：`D:/wt-rel-core`（分支 `wt-release-core`）
> 性质：独立只读审计，未修改任何源码；本文件是唯一写产物。
> 对照基准：spec `docs/superpowers/specs/2026-10-27-release-module-design.md`（§7 状态、§8 边界、§9 数据表）+ 审计 A（`2026-10-28-release-audit-A-logic-data.md`）已审过的批次 2 之前代码（不重复审）。
> 审计任务清单来自主会话五条（createTaskWithEnvs / dueItems 双签名 / ticker 保护 / schema 唯一索引 / reminder 数据面）。

## 0. 审计基线（开始时记录）

- 开始时 HEAD = `eb542240`，工作区 30+ 文件未提交（批次 3+4 改动）。
- **审计中途树漂移**（16:5x）：工作区改动被主会话一次性提交为 `6ca2174a feat(release): 上线模块全量实现…`，随后 `8c6b27b2` 分诊留痕。本报告所有 file:line 与结论均已用 `git show 6ca2174a:<path>` 逐项重锚（deriveStatus / 唯一索引 DDL / ticker catch / planDayNowUnix 辅助函数行号与内容逐一核对一致），**结论以 6ca2174a 为准，未失效**。
- 审计结束时工作区 clean（`git status --porcelain` 空）。

## 1. 六维结论

### 1.1 正确性

| 项 | 结论 |
|----|------|
| `createTaskWithEnvs` 原子性（嵌套事务） | **已核**。`release_store.cpp:715-742` 外层 `db_.transaction` 包 createTask + 逐环境 addTaskEnv；`sqlite_db.cpp:369-406` 的嵌套语义为 thread_local txDepth 深度计数——内层只跑 fn 不做 BEGIN/COMMIT，失败外传由最外层 ROLLBACK，同线程同步调用下整批零残留。等长校验在事务前（`release_store.cpp:718-723`），envs[i].taskId 一律覆盖为新建任务 id（`:733-734`）。触发器注入回滚测试（`test_release_store.cpp:706-768`）能真红。 |
| `deriveStatus` 与 spec §7 | **已核**。Done/Voided 终态透传、Draft 恒 Draft（提交才转 Pending）、Running 恒 Running（不回退）、Pending 按 `nowMinute < baseline` 边界（含恰好相等 → Running，`release_logic.cpp:152-177`）；无有效环境（baseline=-1）/日期非法保持库值。与 spec §7「状态只跟时间走」一致；spec 的「全部完成→已完成」由详情页 computeDisplayStatus/advanceTaskStatus 写库承担（列表只读展示），分工闭环。 |
| `planDateElapsedMinutes` 换算 | **已核**（跨零点/负数/非法日期）。`release_logic.cpp:133-150`：parseIsoDate 严格校验后 mktime 本地零点（tm_isdst=-1），floorDiv 向负无穷取整，跨零点可 >1440、计划日前为负；日期非法或 mktime 返回 -1 → 返回 false 且不写 outMinute，与头文件契约逐字一致。 |
| `expectedConfigs` 槽位分母 | **已核**。Backend 固定 5（nacos/db/cron/menu/gateway），PcWeb/MobileWeb 为 0，与库中行数无关；envProgress 缺行计未完成、跳过/完成计关闭、同 kind 多行按槽位去重（`release_logic.cpp:285-334`）。 |
| 唯一索引挡合法写？ | **已核不挡**。恢复前 UI 预检 `findTaskByDate`（`release_detail_ui.cpp:1082-1089`）；建单弹层预检（`release_dialogs.cpp:153`）；setStepDone UPDATE-first + 事务持连接锁（recursive_mutex 全程持有，`sqlite_db.cpp:369-372`）；配置 kind 是固定槽位不会改 kind 撞索引。同环境名跨任务复用不受 (task_id, env_name) 影响。 |
| reminder 步骤一次性拉取与分发 | **已核**。`release_reminder.cpp:285-290` 一次 `listTaskStepsByTask`（两级 JOIN，`release_store.cpp:1009-1032`）后按 taskServiceId 分发（`:311-313`），替代每服务一条 N+1。 |
| 窗看门狗 5 秒对账与代数守卫 | **已核**。开窗请求超 5s 无句柄 → 清 openRequestedAt 允许重试（`release_reminder.cpp:555-564`）；windowOpen 但句柄空超 5s → 重置开窗态 + 清指纹护栏（`:529-541`）；openGeneration 代数：迟到的旧请求窗口创建/销毁回调一律忽略并立即关闭，不碰当前窗状态（`:616, 660-680`）。宽限期 5s 与常量 kWindowOpenGraceSeconds 一致。 |

### 1.2 回归面

- 旧 5 参 `dueItems` 重载：**该留且仅剩旧测试调用**（grep 全 src：详情页 `release_detail_ui.cpp:1777` 与提醒 `release_reminder.cpp:317` 均已走 6 参；5 参只剩 `test_release_logic.cpp` 旧用例），与头文件声明一致。空 steps 语义经推演与旧契约逐点一致（有存活服务必红、全移除 0/0 不红）。
- `deriveStatus` 唯一调用方是列表（`release_list_ui.cpp:836`）。详情页状态口径**未被遗漏**：详情页用独立三件套（computeDisplayStatus/advanceTaskStatus/restoredStatus，`release_detail_ui.cpp:1010-1055`）并把推进写回库；完成动作只发生在详情页，Done 同帧写回，列表与详情无可见分叉。
- schema 唯一索引为纯追加（`git show --stat 6ca2174a -- src/core/schema.h` = +126），既有表/索引/种子未动；ticker 新增文件，唯一接入 main.cpp 一行 pump。

### 1.3 平台对称性

- `planDateElapsedMinutes` 用 mktime：Windows UCRT 与 macOS libc 对 tm_isdst=-1 的本地零点/DST 判定语义一致，生产代码两平台对称。DST 切换日墙钟 vs 流逝分钟固有 ±60 分钟偏差属 A-13 已接受口径（中国无 DST）。
- **测试辅助函数两平台一致地错**（见 F-1）：`planDayNowUnix` 用纯民用 UTC 天数，与实现的本地零点口径脱钩，非 UTC 时区（含本机 UTC+8）必红。

### 1.4 静默失败面

- 唯一索引冲突的可辨识性**只做了一半**：upsertService/upsertEnv 有 isUniqueViolation→AlreadyExists 映射；createTask（含 createTaskWithEnvs）/upsertTaskConfig/setStepDone/appendLog 撞索引或外键违约全部以 `Internal` + 原始 SQLite 文本冒出（`sqlite_db.cpp:13-21` mapErrCode 未区分 SQLITE_CONSTRAINT）→ F-3。
- ticker try/catch 吞掉后无状态残留：everFired/lastFired 在调用前记账是注释明示的刻意设计（防回调自注销后再访问 entry）；name/fn/user 均先拷贝防悬垂（`ticker.cpp:100-111`）。无残留。
- reminder 读库失败：整体放弃本 tick 保持现窗 + 节流 LOG_WARN + 恢复 LOG_INFO（`release_reminder.cpp:722-726`），不静默。

### 1.5 测试有效性

- **原子化 2 条非恒真**：`test_release_store.cpp:623-703`（等长预检 + 三层落库逐字段断言 + 事务前拒绝不产生写入）、`:706-768`（RAISE(ABORT) 触发器注入第二个环境 → 三层零残留 + 撤触发器后同日重试成功；若内层自 BEGIN/COMMIT，重试断言必红）。
- **deriveStatus 7 块断言非恒真但 2 条在非 UTC 时区必红**（见 F-1）：终态透传/Draft 恒 Draft/Running 不回退/无基准/非法日期 5 块与时间换算无关恒绿；pending 边界与跨零点 2 块在 UTC+8 机器实测口径错位 480 分钟。
- 覆盖缺口三条：ticker 异常回调无用例（F-4）、listTaskStepsByTask 无直接用例（F-5）、setStepDone 幻影行守卫无用例（F-6）——审计 A B-13 列出的同名缺口在修复批仍空白。

### 1.6 文档口径

- 已核一致：dueItems 6/5 参契约（含条目顺序、环境完成判定同源 envCompleted）、deriveStatus 契约（终态/Draft/Running/Pending/无依据不猜，与实现逐行对齐）、planDateElapsedMinutes（floorDiv 向负无穷/不写 outMinute）、expectedConfigs（Backend 5/前端 0/缺行计未完成）、ticker no-throw 契约（捕获+LOG_ERROR+继续本轮）、createTaskWithEnvs（taskId 忽略/等长校验/事务原子）、listTaskStepsByTask（两级 JOIN 一次性拉全）。
- 审计 A 的 B-04（工单注释漏 live 过滤）在批次 3/4 的头文件重写中实际已订正（`release_logic.h:58-59` 已含「仅未移除环境下的未移除服务」），与分诊清单 A-06「不修」的留痕不一致——属记账口径差异，非缺陷。
- 一处契约欠完整：`release_store.h:80-82` setStepDone 未声明「从未置位过的步骤撤回不补行」（实现有守卫，见 F-7）。

---

## 2. Findings

### Blocker（0）

无。

### Major（2）

#### F-1 测试时钟口径与实现不一致：deriveStatus 用例在非 UTC 时区必红，且「恰好等于基准」边界从未真正覆盖

- **位置**：`tests/test_modules/test_release_logic.cpp:75-79`（辅助函数 `planDayNowUnix` = `core::time::daysFromCivil(2026,10,27)*86400 + minute*60`，纯民用 UTC 天数，注释自述「与 release_logic 内部 nowMinute 同口径」）；实现 `src/module_release/release_logic.cpp:133-150`（`planDateElapsedMinutes` 用 `mktime` 本地零点）。
- **问题**：辅助函数产出的 epoch 是 **UTC 零点** 起算，实现换算的锚点是 **本地零点**。本机（UTC+8）实测：本地零点 − UTC 零点 = −480 分钟。于是 `planDayNowUnix(599)` 经 `planDateElapsedMinutes` 得 1079（≥ 基准 600）→ `release_status_pending_transitions_at_baseline` 断言 Pending → **红**；`planDayNowUnix(1379)` 得 1859（≥ 基准 1380）→ `release_status_cross_midnight_after_baseline` 断言 Pending → **红**。只有 TZ=UTC 的机器全绿；而在 UTC 机器上 `planDayNowUnix(600)` 恰好命中相等边界纯属巧合，非 UTC 机器上「恰好等于基准那一刻 → Running」这条断言从未真正测到边界（实测值 1080 ≠ 600）。
- **影响**：开发机（中国时区）跑测试套件必挂 2 条断言，全绿门槛在真实环境过不去；测试注释「同口径」误导后续维护者。与「测试按场景写、每条注明防什么事故」的交付纪律冲突——这两条用例防的事故（边界误判）在非 UTC 时区反而测不到。
- **建议**：`planDayNowUnix` 改为与 `planDateElapsedMinutes` 同构——先 `mktime` 本地零点（tm_isdst=-1）再 `+ minute*60`；或在测试进程入口显式设 `TZ=UTC`（两平台 CRT 均认，MinGW UCRT 与 macOS libc 都支持），并把该假设写进测试文件头部注释。

#### F-2 唯一索引对存量脏库是「启动即硬崩」，无迁移守卫；schema 注释「防历史脏数据」与实际行为相反

- **位置**：`src/core/schema.h:361-370`（四条 `CREATE UNIQUE INDEX IF NOT EXISTS` 直接建在既有表上，无去重守卫）；失败链 `src/core/sqlite_db.cpp:408-421`（applySchema 整脚本单事务，任一语句失败整批回滚）→ `src/core/database.cpp:42-46`（migrate 失败）→ `src/main.cpp:140-148`（open 失败且 quick_check 判非损坏 → `LOG_FATAL` + `std::abort()`，连还原对话框都不走）。
- **问题**：若既有库在索引加入前已有重复行（如同日两个非作废任务——早批次恢复动作无 findTaskByDate 预检时可能造出；同服务同 kind 配置重复），`CREATE UNIQUE INDEX` 必然失败 → 应用**硬崩溃**且无任何可恢复路径。schema 注释写「库层防并发写穿透与**历史脏数据**」，实际行为是把历史脏数据变成启动崩溃——注释与行为相反。
- **影响**：本机实证：真实库 `C:\Users\e-Yunfei.Qian\DevDesk\devdesk.db`（2026-10-27 版）尚无 release 九表（模块未随已发布版本出海），`.demo-data/devdesk.db` 九表俱全、四类重复计数全 0——**当前两个库都安全**。风险限定在「早批次工作树产物库 + 恰有脏行」组合：概率低、后果高（数据进不去、应用起不来、用户无对话框可操作）。
- **建议**：建索引前先查重并清理（重复行保留最小 id、LOG_WARN 留痕）再 CREATE INDEX；或把 migrate 失败从 abort 降级为「备份 + 弹窗 + 可重试」路径。至少把「存量库须先验重」写进 schema 注释，替代「防历史脏数据」的错误表述。

### Minor（7）

#### F-3 A-04/A-10 在数据层未见落地：外键违约与非法输入仍以 Internal 冒出

- **位置**：`src/module_release/release_store.cpp` 写入口（addTaskEnv `:799-841`、upsertTaskConfig `:943-983`、setStepDone `:1034-1065`、appendLog `:1069-1082` 无父行存在性预检；createTask/updateTask/upsertEnv/appendLog 无日期格式/分钟范围/action 取值校验）；根因 `src/core/sqlite_db.cpp:13-21` mapErrCode 对 SQLITE_CONSTRAINT 归 default → Internal。
- **问题**：分诊清单 A-04（父 id 无效返回对应错误码）、A-10（写入口最小校验）在数据层均无对应代码；UI 层有兜底（parseTimeText、findTaskByDate 预检），但 store 契约「错误一律 Result 返回」对写入路径只覆盖了读回非法枚举、未覆盖写入非法值。
- **影响**：绕过 UI 的入口（导入、脚本、后续自动化）产生「不崩但永远不提醒」的脏任务，或父 id 失效时报「内部错误」无法定位。可能是后续批次安排，本批报以留痕。
- **建议**：写入口最小校验（ISO 日期、0..1439、action 白名单）+ 父行存在性预检，或在 mapErrCode 区分 SQLITE_CONSTRAINT_UNIQUE / FOREIGNKEY。

#### F-4 A-07 的 ticker 异常保护无任何测试

- **位置**：`tests/test_modules/test_ticker.cpp`（7 个 REGISTER_TEST 全为正常路径；grep throw/异常零命中）；实现 `src/core/ticker.cpp:106-111`。
- **问题**：审计 A B-13 明确列出「异常回调行为未覆盖」，修复批加了 try/catch 却未补用例——移除保护套件仍全绿。
- **影响**：回归保护为零；「一个坏回调拖垮整帧」的事故会原样复发。
- **建议**：加一条「回调抛异常 → 其余订阅照常触发 + LOG_ERROR」用例（注册 A 抛异常、B 计数，断言 B 收到回调）。

#### F-5 listTaskStepsByTask（两级 JOIN）无任何直接测试

- **位置**：`src/module_release/release_store.cpp:1009-1032`；`tests/` 下 grep `listTaskStepsByTask` 零命中。
- **问题**：批次新增的一步拉全 SQL 无直接用例；JOIN 写错（漏 env 级联 → 别任务的步骤漏进本任务、ORDER BY 丢失 → 分发顺序不稳）现有套件测不到。
- **建议**：补一条「两任务各有步骤 → 按 taskId 拉取互不串」用例，与提醒引擎分发逻辑对应。

#### F-6 setStepDone 幻影行守卫无测试

- **位置**：`src/module_release/release_store.cpp:1052-1054`（doneAt==0 且未命中不插行）；测试 `tests/test_modules/test_release_store.cpp:412-446` 只覆盖「已有行撤回」路径。
- **问题**：审计 A B-07 的修复（幻影行守卫）无对应用例——移除守卫后现有断言仍全绿（撤回用例走 UPDATE 命中分支）。
- **建议**：补「从未置位过的 kind 直接撤回 → 行数不变」断言。

#### F-7 transaction() 的 txDepth 非 RAII：fn 抛异常后线程永久失去事务语义

- **位置**：`src/core/sqlite_db.cpp:374-390`（`++txDepth; auto result = fn(); --txDepth;` 无守卫对象）；依赖方 `src/module_release/release_store.cpp:715-742`（createTaskWithEnvs 原子性承诺建立在该机制上）。
- **问题**：fn 若抛异常（非 Result 失败，如 std::bad_alloc），txDepth 不回退：此后该线程所有「事务」静默退化为无 BEGIN/COMMIT（逐条 autocommit，原子性静默丢失），且上一个 BEGIN 永不提交。仓库约定不抛异常、store fn 内无回调，概率低；但批次 3/4 的核心交付物（原子建单）正依赖此机制，值得披露。
- **建议**：RAII 深度守卫（局部对象析构回退）；或至少在 transaction() 注释写明「fn 抛异常即线程事务语义损坏」。
- **注**：该机制批次前已存在（sqlite_db.cpp 本批未改），本条属依赖面披露，非本批引入。

#### F-8 createTask/upsertTaskConfig 路径缺唯一冲突映射（与 F-3 同源，单列）

- **位置**：`src/module_release/release_store.cpp:688-713`（createTask INSERT 无 isUniqueViolation→AlreadyExists）、`:943-962`（upsertTaskConfig INSERT 无映射）、`:744-775`（updateTask 改 plan_date 撞索引无映射）；对比 upsertService/upsertEnv 已有映射（`:406-411, 536-541`）。
- **问题**：同日重复建单/同服务同 kind 重复配置的竞态或新入口场景，用户看到的是「step: UNIQUE constraint failed: idx_release_task_plan_date_active」的 Internal，而非可辨识的「当天已有任务」。
- **影响**：UI 预检兜住了正常路径，但错误码语义不一致（同一类约束三种表现）。
- **建议**：把 isUniqueViolation 检查补进上述三个写路径，返回 AlreadyExists/ValidationError。

#### F-9 release_store.h setStepDone 契约未声明「从未置位过的步骤撤回不补行」

- **位置**：`src/module_release/release_store.h:80-82`；实现守卫在 `src/module_release/release_store.cpp:1052-1054`。
- **问题**：契约只写「已有行置 0，不删行」，未写「无行且 doneAt==0 直接成功不插行」；按头文件契约实现会复现 B-07 幻影行（并污染「缺行按未完成计」口径）。
- **建议**：头文件补一句「从未置位过的步骤撤回不产生行」。

---

## 3. 计数

| 级别 | 数量 |
|------|------|
| blocker | 0 |
| major | 2 |
| minor | 7 |

（F-9 为 F-6 的契约侧补充；F-8 与 F-3 同源分列。）
