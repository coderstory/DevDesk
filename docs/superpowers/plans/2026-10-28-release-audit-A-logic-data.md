# 上线模块审计 A：纯逻辑 / 数据访问 / ticker

> 审计日期：2026-10-28 13:39 ~ 13:55
> 工作目录：`D:/wt-rel-core`（分支 `wt-release-core`）
> 性质：独立只读审计，未修改任何源码；本文件是唯一写产物。
> 对照基准：`docs/superpowers/specs/2026-10-27-release-module-design.md`
> 说明：任务指定的 `docs/superpowers/specs/2026-10-27-release-visual-tokens.md` **不存在**；本次审计范围无视觉文件，未造成缺基准。

---

## 0. 审计基线（开始时记录）

### 0.1 `git status --porcelain`

审计范围内文件相对 HEAD 的状态（HEAD = `06107f70`，T01 地基提交）：

```
 M src/core/schema.h
 M src/module_release/release_logic.cpp
 M src/module_release/release_logic.h
 M src/module_release/release_store.cpp
 M src/module_release/release_store.h
 M src/module_release/release_types.h
?? src/core/ticker.cpp
?? src/core/ticker.h
?? tests/test_modules/test_release_logic.cpp
?? tests/test_modules/test_release_store.cpp
?? tests/test_modules/test_ticker.cpp
```

工作区另有大量模块 UI / 装配 / 设置 / 平台文件改动，均不在本次审计范围；审计期间 `tests/probes/module_toggle_sidebar_probe.cpp` 出现新的 `M`（范围外，未采信其内容）。

### 0.2 范围文件 mtime（审计开始 13:39 与结束 13:52 两次采样完全一致，审计期间未被改动）

| 文件 | mtime |
|------|-------|
| `src/module_release/release_types.h` | 2026-10-27 22:44:59 |
| `src/module_release/release_logic.h` | 2026-10-27 22:39:26 |
| `src/module_release/release_logic.cpp` | 2026-10-27 22:39:50 |
| `src/module_release/release_store.h` | 2026-10-28 11:06:33 |
| `src/module_release/release_store.cpp` | 2026-10-28 10:59:40 |
| `src/core/schema.h` | 2026-10-27 22:44:53 |
| `src/core/ticker.h` | 2026-10-28 10:45:25 |
| `src/core/ticker.cpp` | 2026-10-28 10:45:36 |
| `tests/test_modules/test_release_logic.cpp` | 2026-10-27 22:44:19 |
| `tests/test_modules/test_release_store.cpp` | 2026-10-28 11:05:57 |
| `tests/test_modules/test_ticker.cpp` | 2026-10-28 10:49:10 |
| `docs/superpowers/specs/2026-10-27-release-module-design.md` | 2026-10-28 09:32:13 |

结论：审计期间范围内文件零改动，以下结论**未失效**。

---

## 1. 六维结论

### 1.1 正确性（逐条核对 spec）

| spec 规则 | 结论 |
|-----------|------|
| 日期只放行周二/周四 | 已核：`isRegularReleaseDay` 严格解析 + 纯整数历法，非法日期/越界日期一律 false（`release_logic.cpp:96-105`） |
| 时间基准 = 未移除环境最早开始 | 已核：`taskBaseline` 跳过 removed，无有效环境返回 -1（`release_logic.cpp:107-119`） |
| 工单/提测/CAB = 基准−1h（橙） | 已核：`release_logic.cpp:138-157` |
| 公告 = 基准（黄） | 已核：`release_logic.cpp:159-162` |
| 部署 = 各环境开始（红） | **部分偏离**：到期时刻正确，但「未完成」用「环境仍有未移除服务」近似，不会在环境做完后停止报红 → A-01 |
| 合并分支不参与提醒 | 已核：`dueItems` 不消费 TaskStep，部署条目与 branch 无关 |
| Draft 过基准要报红「任务未提交」 | 已核：`release_logic.cpp:172-175`；Draft 未到基准整体静默（`:136`） |
| 提醒级别取最高 | 已核：`highestLevel` 红 > 橙 > 黄，空集黄（`release_logic.cpp:180-187`） |
| 校验只查分支与未跳过配置内容 | 已核：不查分支真伪/工单号/前置单；已移除服务与其配置不参与（`release_logic.cpp:189-212`）；但 owner 缺失的配置被静默跳过 → B-03 |
| 进度 = 3 固定步骤 + 未跳过配置 | 已核：固定 3 步缺行也计入 total；`kind=config` 聚合行不参与（`release_logic.cpp:214-249`）；但后端「固定 5 项配置」未在纯逻辑内体现 → A-02 |
| 环境完成 = 全部未移除服务走完 | 已核：`envCompleted` 为 `done == total`；服务全移除/环境移除 → 0/0 完成；任务完成 = 进度满（`release_logic.cpp:283-298`） |
| 作废不计当日唯一性、计入列表总数 | 已核：`findTaskByDate` 排除 voided；`countTasks`/`listTasksPage` 含 voided（`release_store.cpp:611-685`） |

### 1.2 回归面

- schema **纯追加**：`git diff HEAD -- src/core/schema.h` = `+114 / -0`，release 九表整段插入在 `github_sync_state` 与既有索引之间，既有表 DDL、索引、种子 INSERT 逐字未动；迁移顺序未变（`applySchema` → `addMissingAgentColumns` → `migrateTodoFixedColumns` → `dropRemovedTables`，`dropRemovedTables` 不含 `release_*`）。
- ticker 为新增文件（untracked），全仓库既有调用方零处引用 `core::ticker`；唯一接入点是 `main.cpp:641` 新增一行 `pump`，纯追加。`src/core/CMakeLists.txt` 追加一行源文件。
- `Result` 错误码：`NotFound` / `AlreadyExists` / `ValidationError` / `Internal` 的使用与既有 `profile_repository` 的 UNIQUE→AlreadyExists 语义一致；唯一偏差是外键约束失败落到 `Internal` → B-02。
- 新增九表会自动进入 GitHub 同步的整库 SQL dump（`defaultSkipTables` 未排除）→ B-12（待拍板，非确定性缺陷）。

### 1.3 平台对称性

- 范围内文件零 `#ifdef`、零 `_WIN32/__APPLE__`、零 `platform/` 依赖；ticker 只用 `<functional>/<vector>`，logic 只用整数历法。
- 跨零点：`dueItems` 只比较 `startMinute`，`nowMinute` 可 > 1440，跨零点测试已覆盖（`test_release_logic.cpp:190-212`）。
- 时区/DST：范围内代码本身平台无关，但契约未定义 `nowMinute` 是「墙钟分钟数」还是「零点起实际流逝分钟数」，调用方 `mktime` 实现会在 DST 日产生 60 分钟偏差 → B-11。

### 1.4 静默失败面

- 全部 SQL 走 `prepare/bind/step/fetchAll` 并逐层 `Result` 上报，无裸 `sqlite3_exec`、无字符串拼接。
- `countTasks`：空行结果报 `Internal`、超 int 报 `Internal`；无静默截断。
- `listTasksPage`：负 offset/limit → `ValidationError`；limit=0 → 空页；分页在 SQL 侧完成。
- ticker：空 `std::function` 跳过、未知句柄幂等、回调内自注销按句柄复核，均不会崩/悬垂（`ticker.cpp:84-104`）。
- 回调抛异常未捕获，会中断本轮剩余订阅并抛回主循环；契约也未写「回调不得抛异常」→ B-05。
- 未发现「不崩但也不报」的 SQL 路径；两处静默是刻意的（空 fn、未知句柄）且已注释。另有 `validateTask` 静默跳过 orphan 配置（B-03）。

### 1.5 测试有效性

- **未发现恒真断言**：关键断言都会随功能回退变红（发布日正反例、基准-60/基准边界、跨零点 nowMinute>1440、作废/非作废双口径、分页翻页去重、ticker 三时间维度组合）。
- 两处冗余断言不构成缺陷：`test_release_store.cpp:241` 重复 `laterId.isOk()`；`test_release_logic.cpp:115` 重复 `taskBaseline`。
- 覆盖面缺口见 B-13；另有一处「注释声称验证、实际无断言」见 B-14。

### 1.6 文档口径

- `release_logic.h` / `release_store.h` / `schema.h` / `ticker.h` 的主体注释与实际行为一致。
- 三处陈旧/不完整口径：`release_logic.h:29-31`（「全部做完后任务状态会转 Done」只对整单成立，已并入 A-01）、`release_logic.h:32`（工单适用性漏写 live env/service 过滤，B-04）、`release_types.h:40` + `schema.h:349`（action 实际还有 `submit` / `reorder`，B-10）。

---

## 2. Findings

### Blocker（0）

无。

### Major（2）

#### A-01 环境已全部做完后，「部署未完成」红条不会消失

- **位置**：`src/module_release/release_logic.cpp:164-170`；相关注释 `src/module_release/release_logic.h:29-31`
- **问题**：`dueItems` 判定环境红条的条件是「已到 env.startMinute 且该环境仍有未移除服务」（`envHasLiveService`），并没有消费 `TaskStep`/`envCompleted` 来判定部署是否真的完成。只要服务行还在（永远不会物理删），该环境从开始时刻起每一轮都产出红条，直到**整单** `task.status == Done` 才整体静默。多环境任务里「EM 全部验证完成、EU 还没开始」是常态，此时 EM 会每 5 分钟被报一次「部署未完成」，与实际进度矛盾。
- **影响**：提醒引擎的核心语义（「已经到期但还没完成」才弹）被破坏；用户会对红条脱敏，真正未完成的环境反而被噪声淹没。头文件用「全部做完后任务状态会转 Done」为该近似辩护，但 `Done` 需要所有环境 + 任务级前置全部完成，该论断对「单个环境完成」不成立，注释本身也是错的。
- **建议**：给 `dueItems` 增加 `const std::vector<TaskStep>& steps`（或由调用方传入已完成环境 id 集），环境条目改为「未 `envCompleted` 才报」；若短期不改签名，至少在 `release_reminder.cpp` 聚合处按 `envCompleted` 过滤已完成环境，并同步修正 `release_logic.h` 注释与新增用例。

#### A-02 后端「固定 5 项配置」没有落在纯逻辑里，进度/校验依赖调用方补合成行

- **位置**：`src/module_release/release_logic.cpp:198-210`（校验）、`:242-246`（进度）；契约 `src/module_release/release_logic.h:49-56`
- **问题**：spec §3.1 规定后端服务的配置项由类型固定为 5 项，不随数据行是否存在而变。当前 `envProgress` / `validateTask` 只遍历调用方传入的 `TaskConfig`：没有配置行 = 0 项。详情页靠 `appendSyntheticConfigs`（`release_detail_ui.cpp:331-350`）在内存里补 5 个 id=0 占位行来补正，而任务列表页直接读库、**没有补**（`release_list_ui.cpp:829`）。实测数据路径：新建任务时弹层只写 env/service 快照、不写 config 行（`release_dialogs.cpp:197-224`），因此一个刚建的后端任务在列表卡片上 total = 3×服务数（而非 8×服务数），做完合并/部署/验证 + 提测/CAB/公告就显示 100%，而执行页仍是 6/11。打开执行页也不会持久化占位行，列表口径会一直错到用户第一次填/跳过某项配置。
- **影响**：列表页进度条与「N/M」数字系统性偏低，可能把未完成任务显示成 100%；任何绕过详情页合成逻辑的新调用方（导出、概览、后续自动化）做 `validateTask`/`taskCompleted` 时会把 5 个空配置当成「不存在」而放行。同一函数内「固定 3 步可以缺行计入」与「固定 5 配置缺行不计入」的双标，后续维护者极易踩。
- **建议**：二选一并在契约里写死：(a) `envProgress` / `validateTask` 按 `TaskService.kind == Backend` 补齐 5 种 `ConfigKind` 的期望槽位，缺失行按未完成计入 total；或 (b) 建任务时在数据层一次性落 5 行配置（弹层或 store 的 `addTaskEnv`），详情页删除合成兜底。无论哪种，都要给列表/详情加一条「同一任务两页 progress 必须相等」的用例。

### Minor（14）

#### B-01 spec 的数据层唯一性没有任何 SQL 约束兜底

- **位置**：`src/core/schema.h:285-347`（`release_task` 无 plan_date 部分唯一索引；`release_task_env` 无 `(task_id, env_name)`；`release_task_config` 无 `(task_service_id, kind)`；`release_task_step` 无 `(task_service_id, kind)`）；写入点 `src/module_release/release_store.cpp:687-712, 769-811, 913-953, 979-1006`
- **问题**：spec 的「一天一个（作废不占名额）」「一个任务里同一环境只出现一次」「每个服务每类配置/步骤只有一项」目前全部由 UI 单点保证（弹层查 `findTaskByDate`、选择去重；`setStepDone` 靠事务内 get-or-insert）。`createTask`/`addTaskEnv`/`upsertTaskConfig(id=0)` 均可直接造出重复数据，库层不会拒绝。
- **影响**：任何新入口（导入、恢复、脚本、后续同步）都能破坏业务不变量；一旦重复行落库，进度会重复计数、提醒按同名环境合并分组、`findTaskByDate` 只返回最新一条。
- **建议**：加部分唯一索引 `CREATE UNIQUE INDEX ... ON release_task(plan_date) WHERE status <> 'voided'` 及 `(task_id, env_name)`、`(task_service_id, kind)` 两类唯一索引（config/step 各一条）；或在对应写方法内先查后写并显式返回 `AlreadyExists`。schema 注释已解释 plan_date 不能直接 UNIQUE，部分索引正好满足。

#### B-02 外键约束失败被上报为 `Internal`，不是 `ValidationError` / `NotFound`

- **位置**：`src/module_release/release_store.cpp:118-143`（`fetchAny`/`fetchId`/`execDone` 原样转发错误码）；根因 `src/core/sqlite_db.cpp:13-21` 的 `mapErrCode` 把 `SQLITE_CONSTRAINT` 归入 default → `Internal`
- **问题**：`addTaskEnv`（不存在的 taskId）、`upsertTaskConfig`（不存在的 taskServiceId）、`setStepDone`、`appendLog` 的父 id 无效时，SQLite 的 `FOREIGN KEY constraint failed` 会以 `Internal` 冒到 UI，语义上应是输入非法/引用不存在。
- **影响**：调用方无法区分「数据库坏了」和「传了个过期 id」；UI 只能显示「内部错误」，排障成本高，错误码语义与仓库既有约定不一致。
- **建议**：在 store 的写入口先做父行存在性检查（或按错误文本含 `FOREIGN KEY` 映射为 `NotFound`/`ValidationError`）；补一条「父 id 不存在」的用例。

#### B-03 `validateTask` 静默跳过 owner 缺失的配置，且签名无法排除已移除环境

- **位置**：`src/module_release/release_logic.cpp:198-211`；契约 `src/module_release/release_logic.h:43-45`
- **问题**：`owner == nullptr`（config 指向不存在的服务）时直接 `continue`，缺数据不报错；函数没有 env 入参，若调用方把「已移除环境下的服务」原样传入，这些服务的空分支会被当成缺失项误报——目前详情页靠 `loadDetail` 提前过滤 removed env（`release_detail_ui.cpp:2879`）才没触发。
- **影响**：前者会把损坏数据当「校验通过」；后者让纯函数的正确性依赖调用方隐式约定，后续提醒/导出/自动化复用时会误拦。
- **建议**：orphan 配置改为报告「配置项归属异常」；`validateTask` 增加 `envs` 入参或明确文档 + 断言要求入参只含有效环境的服务。

#### B-04 工单适用性的注释漏写 live 环境/服务过滤

- **位置**：`src/module_release/release_logic.h:32`；实现 `src/module_release/release_logic.cpp:51-73`
- **问题**：注释写「任务内存在该 kind 的未跳过且内容非空的配置」，实现还要求该配置属于未移除服务、且服务属于未移除环境。
- **影响**：注释与实现不一致，容易被后续维护者按注释「简化」实现而误催已移除服务的工单。
- **建议**：注释补齐两条过滤条件（实现行为本身正确，测试已覆盖 removed service 场景）。

#### B-05 ticker 回调抛异常无任何保护，契约也未声明 no-throw

- **位置**：`src/core/ticker.cpp:99-104`；契约 `src/core/ticker.h:19-20`
- **问题**：`fn(now, user)` 直接调用。回调一旦抛异常，本轮剩余订阅不再触发，异常抛穿 `pump` 进入主循环；没有日志、没有隔离。空 fn 有跳过、自注销有复核，唯独异常路径未定义。
- **影响**：一个模块的回调 bug 会连带其他模块的每帧任务停摆，且现象是「整帧异常」而非可定位的模块错误；契约读者也无法预期。
- **建议**：至少把「回调不得抛异常」写进头文件；更稳妥的是 `try/catch(...)` 包住单次调用（捕获后 LOG_ERROR + 继续本轮），与仓库「不抛 C++ 异常」的总体约定一致。

#### B-06 `hasSubscribers` 把空回调订阅也算作「有订阅」

- **位置**：`src/core/ticker.h:53-55`；`src/core/ticker.cpp:107`
- **问题**：只判断注册表非空，`fn` 为空的条目永远计数为 true；主循环若据此跳过空转优化，会持续做无意义 pump。
- **影响**：空转判断语义与头文件「空转判断：注册表为空时…」的暗示不完全一致；影响很小，但属于可预期的误用。
- **建议**：`hasSubscribers` 过滤 `!fn` 条目，或在注释里明确「含空 fn 条目」。

#### B-07 `setStepDone(..., doneAt = 0)` 会为从未完成过的步骤插入一行 0 记录

- **位置**：`src/module_release/release_store.cpp:985-1005`
- **问题**：先 UPDATE，未命中则无条件 INSERT。对「从未置位过」的步骤执行撤回，会留下一条 `done_at=0` 的历史行，与「撤回 = 把已有行置 0，不删行」的注释口径不符。
- **影响**：日志/步骤表会出现从未发生的记录；当前 UI 只在已有步骤上撤回（`release_detail_ui.cpp:1162`），暂无可见后果，但契约与实现不一致。
- **建议**：`doneAt == 0` 且 UPDATE 未命中时直接返回 ok；或注释明确「撤回会补 0 行」。

#### B-08 数据层不校验日期格式、分钟范围与 action 取值

- **位置**：`src/module_release/release_store.cpp:687-712`（planDate 原样入库）、`:497-567`（start/endMinute 无 0..1439 检查）、`:1010-1023`（action 任意文本）
- **问题**：空串/非 ISO 的 `plan_date` 会被接受，`ORDER BY plan_date DESC` 退化为文本排序，且提醒侧 `nowMinuteForPlanDate` 解析失败会静默跳过该任务（永远不提醒）；分钟越界同理让到期比较失真。
- **影响**：数据入口一旦绕过 UI（导入、脚本、后续 API），会产生「不崩但永远不提醒」的任务。
- **建议**：store 写入口做最小校验（ISO 日期、0..1439、action ∈ 已知集合）并返回 `ValidationError`；或在 schema 加 CHECK。

#### B-09 `listEnvs` 每个环境一条关联查询（N+1）

- **位置**：`src/module_release/release_store.cpp:473-492`
- **问题**：先读全部 env，再循环 `prepare("SELECT service_id FROM release_env_service ...")`。
- **影响**：基础配置页/新建任务弹层每次刷新都放大查询次数；当前数据量小，不构成故障，但属于后续加环境时的线性放大点，且与「列表分页在 SQL 侧完成」的性能纪律不一致。
- **建议**：一条 `SELECT env_id, service_id ... ORDER BY env_id, service_id` 后内存归组，或直接 JOIN。

#### B-10 日志 action 注释沿用旧口径（只写 confirm / revoke）

- **位置**：`src/module_release/release_types.h:40-41`；`src/core/schema.h:349`
- **问题**：实际写入的 action 还有 `submit`（提交，`release_detail_ui.cpp:806`）与 `reorder`（拖拽排序，`:855`）。
- **影响**：按注释实现的读取方/审计脚本会漏掉两类记录；后续按 action 过滤时口径不清。
- **建议**：注释改为 `confirm / revoke / submit / reorder`，或在 store 层为 action 定义枚举/常量集合。

#### B-11 `nowMinute` 契约未区分「墙钟分钟」与「零点起流逝分钟」，DST 日调用方偏差 60 分钟

- **位置**：契约 `src/module_release/release_logic.h:24-25`；调用方证据（范围外）`src/module_release/release_reminder.cpp:96-118`
- **问题**：`startMinute` 是墙钟 0..1439，而 `nowMinute` 由 `mktime` 零点 + `(now - midnight)/60` 得到，是实际流逝分钟。夏令时切换日两者相差 1 小时：春季 23:00 的环境会在午夜才提醒，秋季则提前 1 小时。范围内纯逻辑本身平台无关，但它没有把口径写死，调用方因此有歧义空间。
- **影响**：DST 时区用户的提醒/步骤解锁（详情页也用同一换算）在每年两天偏移 1 小时；中国时区无 DST，属海外场景缺陷。
- **建议**：契约明确写「`nowMinute` = 计划日当地墙钟分钟（= 当地时刻的 hour*60+minute，跨零点累加 1440）」，调用方改用 `localtime` 取墙钟分钟；或反过来把 `startMinute` 定义为「零点起流逝分钟」。范围内无需改代码，但契约必须二选一。

#### B-12 新增九表自动进入 GitHub 同步的整库 SQL dump（需拍板）

- **位置**：`src/core/schema.h:247-359`；`src/core/sql_dump.cpp:22-28`（`defaultSkipTables` 未含 `release_*`）
- **问题**：GitHub 同步导出整库（排除 `claude_call_logs` / `claude_run_settings` / `github_sync_state`），release 九表会随笔记同步上传；其中 `release_task_config.content` 可含数据库脚本/nacos 配置等敏感内容，与 spec「本地 sqlite、单机使用」的定位可能冲突。
- **影响**：用户开启笔记同步即把上线域的配置内容推到远端仓库；是否可接受应由用户拍板，而不是由「没有排除」默许。
- **建议**：明确决策：要么把 release 表加入 `defaultSkipTables`（并在同步卡片「不上传」清单自动呈现），要么在 spec/设置页写明「上线数据随库同步上传」。

#### B-13 测试覆盖面缺口（规则已有、用例没有）

- **位置**：`tests/test_modules/test_release_logic.cpp` / `test_release_store.cpp` / `test_ticker.cpp`
- **问题**：
  - 逻辑：A-01（环境完成后仍报红）无对应用例——这正是该缺陷未被发现的原因；`taskBaseline` 最小开始分钟为 0 的「found 哨兵」边界未覆盖；`taskProgress` 零环境场景未覆盖；`validateTask` owner 缺失、skipped=true 但内容非空的配置、removed env 下服务的工单过滤未覆盖；多环境 dueItems 的顺序未断言。
  - 存储：`upsertService` / `upsertEnv` 更新不存在 id 的 NotFound 分支未覆盖；非法枚举文本读回失败（契约明写）未覆盖；外键失败（`setStepDone`/`appendLog`/`addTaskEnv` 父 id 不存在）未覆盖；`createTask` 同日两个有效任务未覆盖；`Cron/Menu/Gateway` 配置 kind、`Config/Verify` 步骤 kind、`Done` 状态的文本回读未覆盖。
  - ticker：空 `std::function` 跳过（头文件明写）未覆盖；A 回调注销 B（互注销）未覆盖；回调内 `subscribe` 新订阅未覆盖；异常回调行为未覆盖。
- **影响**：上述规则一旦被回退，套件仍全绿；A-01/A-02 就是现成的例子。
- **建议**：按上列清单逐条补场景用例，每条用例描述里标注所防事故（与仓库既有测试风格一致）。

#### B-14 测试注释声称「任务快照不受影响」，但用例未建快照、无对应断言

- **位置**：`tests/test_modules/test_release_store.cpp:162-167`
- **问题**：`release_store_env_service_links` 在删除基础配置服务前没有创建任何任务快照，注释里的「任务快照不受影响」没有断言支撑。
- **影响**：`deleteService` 若未来误加对 `release_task_service` 的级联/更新，该用例不会变红，与注释给人的安全感相反。
- **建议**：在该用例中先 `createTask` + `addTaskEnv` 建快照，再删基础服务，断言快照行仍在且字段不变。

---

## 3. 计数

| 级别 | 数量 |
|------|------|
| blocker | 0 |
| major | 2 |
| minor | 14 |