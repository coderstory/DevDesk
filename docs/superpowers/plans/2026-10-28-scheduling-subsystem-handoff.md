# 定时调度子系统交接文档（2026-10-28）
> 面向没有本次对话上下文的接手会话。仓库根以 `D:/wt-rel-core`（DevDesk worktree）为准；其它路径均写绝对路径。
> 已吸收调研文档 `D:/wt-rel-core/docs/superpowers/research/2026-10-28-scheduling-module-research.md` 的选型、精度/限频与未决问题结论。
## 0. 基线现状（开工前第一件事）

| 项 | 事实 |
|---|---|
| 底座位置 | `D:/wt-rel-core/src/core/ticker.h`、`src/core/ticker.cpp`；分支 `wt-release-core`，HEAD `06107f70` |
| 提交状态 | **未提交**：`git status` 为 `?? src/core/ticker.h`、`?? src/core/ticker.cpp`、`?? tests/test_modules/test_ticker.cpp`；`src/core/CMakeLists.txt` 与 `tests/CMakeLists.txt` 为已修改（M，注册了 ticker） |
| 其它工作树/集成 | `D:/project/gui/DevDesk`（`feature/20261027-release-module`）**不含** ticker 与 `src/module_release/`；集成到 `dev`（`master` 仅发布） |
| 上线模块现状 | `src/module_release/` 仅在 `wt-release-core` 存在；`release_module.cpp:94-105` 已订阅 ticker（`intervalSec=1`）并驱动全屏提醒 |

**开工动作**：先把三件未跟踪文件 + 两处 CMake 注册原子提交（显式 pathspec，禁 `git add -A`）并合入 `dev`，再从 `dev` 切新 worktree。否则新 worktree 从提交切出，树上没有 ticker，编译必失败。
## 1. 一句话目标与为什么现在做

**目标**：在 `core::ticker` 之上建立「规则 → 下次时刻 → 动作 → 通知渠道」四层调度子系统，支持用户自定义定时、系统只读定时、运行记录与补跑、企业微信群机器人推送。

**为什么现在做**：上线模块（`module_release`）已把 ticker 用作 1 秒帧泵并跑通全屏提醒；下一步刚需是企业微信群通知与用户自定义定时。智能体现有六种触发（`src/module_agent/task_draft_job.h:44-47`）只服务「跑 agent 任务」，没有规则模型、通知渠道、补跑与运行历史，不能直接复用。
## 2. 已有底座：`core::ticker`（不要重写）
### 2.1 接口逐字段（`src/core/ticker.h`）

| 符号 | 类型/签名 | 语义 |
|---|---|---|
| `Handle` | `std::uint64_t` | 订阅句柄；0 为无效，`unsubscribe(0)` 幂等 |
| `Now` | `std::int64_t` | unix 秒 |
| `TickFn` | `std::function<void(Now now, void* user)>` | 回调；空函数会被跳过 |
| `Subscription::name` | `std::string` | 仅排障用，不参与调度 |
| `Subscription::fn` | `TickFn` | 回调体 |
| `Subscription::user` | `void*` | 注册方上下文，原样回传 |
| `Subscription::firstAt` | `Now` | 首次触发绝对时刻；0 = 立即（下一次 pump） |
| `Subscription::intervalSec` | `int` | 首次之后距离**上次触发**的间隔秒数；0 = 每次 pump 都触发 |
| `Subscription::expireAt` | `Now` | 失效绝对时刻；0 = 永不过期；`now >= expireAt` 时本轮移除 |
| `subscribe(Subscription)` | `Handle` | 注册；name 不参与调度 |
| `unsubscribe(Handle)` | `void` | 注销，未知句柄静默返回 |
| `pump(Now)` | `void` | 主循环每帧调用，按下面判定顺序推进 |
| `hasSubscribers()` | `bool` | 空表判断 |
| `resetForTest()` | `void` | 仅测试缝，生产不调用 |

**判定顺序（`src/core/ticker.cpp:64-103`，先命中先跳过）**：① 过期（`expireAt != 0 && now >= expireAt`）→ 本轮移除；② 未到（`now < firstAt`）；③ 节流（`intervalSec > 0 && everFired && now-lastFired < intervalSec`）；④ 触发（先记 `lastFired=now`，再调回调）。
**回调内可 `subscribe`/`unsubscribe`（含注销自己）**：pump 先取句柄快照，每次调用前复核活性。
**线程契约**：只在主线程使用，内部无锁；后台线程要触发回调必须先派回主线程。注册表进程级、永不析构（退出钩子 UAF 防护，见 `ticker.cpp` 文件头）。
### 2.2 主循环接入点

`src/main.cpp:641`：`app::dslAppConfig()` 内每帧调用 `::core::ticker::pump(::core::time::nowUnix());`。这是全仓唯一 pump 点；不需要新增接入。
### 2.3 已有测试（`tests/test_modules/test_ticker.cpp`，7 条，已注册 `tests/CMakeLists.txt:125`）

覆盖：注册/注销/user 透传；空表空转；`firstAt` 门控；`expireAt` 停止并清理；`intervalSec` 节流；回调内自注销；三时间维度组合顺序。**这些用例是契约，不得改写或删除。**
### 2.4 反重构警告

- 不要用「每条规则重注册一个 ticker 订阅」的方案：ticker 的 `intervalSec` 从上次触发起算、不按日历对齐，且 `expireAt == firstAt` 会在首触发前被删；逐规则重注册会放大遗漏与忘记注销风险。推荐**全局只挂一条** `{name="schedule.engine", firstAt=0, intervalSec=1, expireAt=0}`，引擎内部自己算到期；秒级分辨率足够（cron/日历均分钟级）。
- 一次性规则不能用 `expireAt` 收尾（会被提前移除），必须由调度引擎在触发后归档/停用。
## 3. 四层架构与职责边界

| 层 | 目标落点 | 职责 | 禁止 |
|---|---|---|---|
| ticker | `src/core/ticker.{h,cpp}`（已有） | 帧泵，只负责「每秒叫一次」 | 不写任何规则、下次时刻、补跑逻辑 |
| scheduler | `src/core/schedule_rule.{h,cpp}`（新建） | 规则模型、校验、`nextFire`、`missedFires`、到期队列；纯函数优先 | 不读系统时钟（由调用方注入 now）、不碰 IO/DB/UI、不依赖 platform/EUI-NEO |
| action | `src/module_schedule/action_runner.{h,cpp}`（新建） | 提醒 / 提醒并跳转 / 静默执行内部任务 / 仅投递 | 不在动作代码里写 `if (wecom)`；渠道选择走 `channel_ids[]` |
| channel | `src/module_schedule/channel_registry.{h,cpp}` + `channels/*.cpp`（新建） | 全屏 / toast / OS 通知 / 静默 / 企业微信 webhook，统一 `deliver()` | 渠道不得反向依赖具体动作 |

宿主：`src/module_schedule/`（新建模块，注册到 `src/app_registry.h` 的 `kModuleRegistry` 与 `src/core/module_interface.h` 的 `PageId`），持有 DB、列表/编辑 UI、运行历史，并在 `onStart` 订阅唯一 ticker 泵、`onStop` 注销。
装配：`src/app_assembly.cpp` 用 `core::ServiceLocator`（先例 `src/core/service_locator.h`）注册系统定时 provider 与内部任务回调；模块之间零 include。

建议接口签名：
```cpp
// core::schedule
struct FireContext { int64_t nowUnix; std::tm localNow; std::string tzId; int64_t lastEvalAt; };
Result<RuleSpec> parseRuleSpec(RuleKind kind, const std::string& specJson);
Result<std::optional<int64_t>> nextFire(const RuleSpec&, int64_t afterUnix, const FireContext&);
Result<std::vector<int64_t>> missedFires(const RuleSpec&, int64_t fromUnix, int64_t toUnix, int cap, const FireContext&);
// module_schedule
struct Delivery { std::string title, body; int level; std::optional<std::string> deeplink; int64_t runId; std::string dedupeKey; };
class Channel { virtual Result<Receipt> deliver(const Delivery&) = 0; };
class ActionRunner { Result<RunOutcome> run(const ActionSpec&, const RunContext&); };
class ScheduleProvider { virtual std::vector<ScheduleItemView> items() const = 0; };
```
## 4. 功能范围清单（用户视角、可验收）
### 4.1 触发模型全集

统一规则 `kind + spec_json`；日历字段按**本地墙钟**存，`next_fire_at` 另存 Unix 秒缓存。

| kind | spec_json 关键字段 | 算法 | 覆盖 |
|---|---|---|---|
| once | `at`（Unix 秒）或本地 `at_local` | 绝对时刻 | 闹钟、单次提醒 |
| interval | `every_sec`、`anchor`、可选 `window` | `anchor + ceil((after-anchor)/N)*N`；休眠/停机只算一次到期 | 每 30 分钟巡检 |
| calendar | `unit`(day/week/month/year)、`interval`、`weekdays[]`、`month_day` / `nth_weekday`、`time`、`anchor_date`、`workdays_only` | 本地日历推进；周规则以 `anchor_date` 所在周为第 0 周，`week_index % interval == 0`；月规则按第 N 天 / 第 N 个周几 / 最后工作日；不存在日期跳过 | 每两周周二 09:00、每月 15 日、每月最后一个工作日 |
| cron | `expr`（5 段） | 复用 `nextCronMatch` | 高级用户兼容 |
| event | `events[]`、`cooldown_sec` | 无下次时刻，事件到达即判定 | 待办/笔记事件 |
| resident | 无 | 仅手动/内部入队 | 值守类动作 |

- 「每两周周二 09:00」= `{"unit":"week","interval":2,"weekdays":[2],"time":"09:00","anchor_date":"2026-10-27"}`；`anchor_date` 必须是周二，否则校验失败。
- 明确不支持用 cron 表达「每两周」（5 段无周奇偶）；不引入 libical/libcron，理由见调研 §3/§15。
### 4.2 系统定时 vs 用户定时

- **用户定时**：可增删改、可启停、可暂停，落库。
- **系统定时**：只读派生，不落库；各来源模块实现 `ScheduleProvider`，列表刷新时与 DB 规则合并展示。视图项：`{source, key, title, summary, next_fire_at, enabled, source_route, executable}`；`origin_key` 用稳定合成键（如 `release:task:<id>:precheck`）。
- 系统项默认**只读展示、执行归来源模块**（避免上线提醒双弹）；来源注册 `executable=true` 才由新引擎执行。
### 4.3 动作类型

| 动作 | 行为 |
|---|---|
| `remind` | 仅提醒（全屏/toast/OS/静默按渠道） |
| `remind_open` | 提醒并跳转（deeplink 指向页面/模块；跳转由 app 层执行） |
| `run_task` | 静默执行内部任务，如 `agent:<task_id>`、`release:<check>`；经 `InternalTaskRegistry`，动作层只认 key |
| `webhook_only` | 只投递企业微信，不弹本地提醒 |
### 4.4 生命周期

| 状态/动作 | 语义 |
|---|---|
| enabled | 正常参与调度 |
| paused | `paused_until` 非空，期间到期点不补（可配） |
| disabled | 保留配置与历史 |
| 编辑 | 事务内重算 `next_fire_at`；正在跑的 run 不打断 |
| 一次性执行后 | 自动归档 `archived_at=now`，不删除，列表默认折叠 |
| 删除 | 软删除 `deleted_at`，历史保留 90 天；提供「彻底删除」二次确认 |
### 4.5 运行记录与失败重试

每次触发（含被跳过/抑制/错过）写一条 `schedule_run`：计划时刻、实际开始/结束、耗时、结果摘要、错误、状态。
状态：`pending | running | success | failed | skipped | missed | suppressed`。
动作失败按类型重试：agent 任务不自动重跑（交给原任务语义）；内部检查任务可配 1 次。渠道失败单独记 `schedule_delivery`，**不改变 run 的 success**（run 成功、投递失败必须能分辨）。
### 4.6 错过与补跑

每条规则独立 `missed_policy`：

| 场景 | 默认 | 行为 |
|---|---|---|
| 用户提醒 | `once` | 启动/唤醒后补一次，记录标注「原定 X 时间」 |
| 静默执行 | `skip` | 不补，写 `missed` 运行记录 |
| 一次性闹钟 | 用户可选 `skip`/`once` | `once` 需限制迟到窗口（建议 24h） |
| 事件触发 | 无补跑 | 应用关闭期间事件不落库即丢 |

`all` 策略最多补 100 次，超出合并为一条「遗漏 N 次」提醒；`(origin, origin_key, planned_at)` 唯一索引保证重启不重复补。同规则同计划时刻只产生一条 run。
### 4.7 冲突与串行

- 同规则不并发：run 状态 `running` 唯一（部分索引），第二条到期点入队等待。
- 同一时刻多条：优先级 **系统定时 > 用户提醒 > 静默动作**；同优先级按 rule id。
- 并发度：重动作全局 1；HTTP 渠道每渠道 2；本地提醒不限（主线程只入队）。
- 队列满：`skip` 并记 `suppressed`，不静默丢弃。
### 4.8 时区、夏令时、跨零点

- 存储：日历/cron 规则存本地墙钟字段，不存 UTC 偏移；`next_fire_at` 为缓存，换时区/改规则时重算；一次性按绝对 epoch 存。
- v1 只支持系统本地时区；规则表预留 `tz_id TEXT DEFAULT ''`，不引入 IANA tzdb。
- DST 春季跳变（02:30 不存在）：在最早存在的时刻 03:00 触发。
- DST 秋季回拨（01:30 出现两次）：只在第一次触发；以「本地日期 + 当地时分」为去重键。
- 跨零点：用 `mktime(tm_isdst=-1)` + `core::time::daysFromCivil` 推进日历日，禁止 `+86400`（DST 日会偏 ±1h）。
- 时钟回拨/NTP：`now < last_eval_at` 时只更新 `last_eval_at`，不重放；之后按新时间重算。
- 休眠跨过触发点：等同「错过」，由 `missed_policy` 决定，不依赖 ticker 语义。
### 4.9 通知渠道注册表

统一接口 `Channel::deliver(Delivery) -> Result<Receipt>`；内置渠道：

| 渠道 | 能力 | 约束 |
|---|---|---|
| 全屏提醒 | 聚合开独立全屏窗 | 关闭后冷却 5 分钟；重启首 tick 补弹（沿用 `release_reminder.h` 契约） |
| 应用内 toast | 跨页轻提示，自动消失 | 单实例队列；适合失败降级 |
| OS 通知 | 托盘 `NIF_INFO` | 标题 63 / 正文 255 UTF-16；去重 2s；限速 3 突发/1 每秒；**无点击动作** |
| 静默 | 空投递 | 只写运行/投递记录 |
| 企业微信 webhook | 群机器人 | 见下表 |

**企业微信要点**（官方文档 2025-08-07 版，调研 §10.2 已核实）：

| 项 | 结论 |
|---|---|
| 端点 | `POST https://qyapi.weixin.qq.com/cgi-bin/webhook/send?key=<key>`，JSON body |
| 长度/限频 | `text.content` ≤ **2048 字节**；`markdown`/`markdown_v2` ≤ **4096 字节**（v2 不支持 @）；每机器人 **20 条/分钟**，实现用令牌桶目标 18 条/分钟 |
| @ 人 | text 用 `mentioned_list` / `mentioned_mobile_list`（userid/手机号/`@all`）；markdown 用正文 `<@userid>`；默认不 @all |
| 错误判定 | 必须解析 body 的 `errcode`，不能只看 HTTP 状态 |
| 重试分类 | 网络错误 / 5xx / `45009`（等 60s）/ `45033`（指数退避）可重试；`93000`（非法/被移除）/`93001`（群不允许）/`93004`（已停用）/`93017`（空请求）判永久失败，停用渠道并本地告警 |
| 重试/幂等 | 3 次：0s / 30s / 300s，每次写 `schedule_delivery`；无服务端幂等键，靠 `(rule, planned_at)` 去重，重试不得改正文 |
| 失败降级 | 最终失败降级本地 toast **一次** + 运行历史标红；不每次重试都弹 |
| 凭据 | key 存 `platform::keychain`：service `devdesk.schedule`，account `wecom.<channel_id>`；**不写 sqlite、不进日志、UI 只显示 `****`** |
| 代理 | `platform::HttpClient` 当前 `Request` 无 proxy 字段（libcurl 默认吃环境代理并排除 localhost）；需补 `proxy_mode`（跟随系统/不使用/自定义），渠道级覆盖优先于全局 |
## 5. 数据模型草案

新增表写入 `src/core/schema.h`（现有 agent 表在同文件 144-171 行，迁移机制同源）。

| 表 | 关键列 | 约束 |
|---|---|---|
| `schedule_rule` | `id, name, enabled, paused_until, kind, spec_json, tz_id(''), action_kind, action_json, channel_ids(JSON), missed_policy, conflict_policy, max_runs, next_fire_at, last_fire_at, last_eval_at, run_count, archived_at, deleted_at, created_at, updated_at` | `kind` 六种；编辑事务内重算 `next_fire_at` |
| `schedule_run` | `id, origin('user' / 'system:<module>'), origin_key, rule_id(NULL 允许), planned_at, started_at, ended_at, status, attempt, duration_ms, result_summary, error` | 唯一索引 `(origin, origin_key, planned_at)`；`rule_id` 不建 FK（系统项无规则行） |
| `schedule_delivery` | `id, run_id, channel_id, attempt, status(pending / success / failed / retry / dropped), response_code, remote_errcode, error, next_retry_at, created_at` | 每渠道每次尝试一行 |
| `schedule_channel` | `id, kind, name, enabled, config_json(endpoint/msgtype/mentions/proxy_mode/timeout), secret_ref, created_at, updated_at` | `secret_ref` 只是 keychain account 名；key 本身不落库 |

**系统派生只读项推荐不落库**：不建表，由 `ScheduleProvider` 返回视图；运行记录写入 `schedule_run`（`origin='system:<module>'`），不建 FK；删来源模块后历史仍在。
## 6. 与智能体现有六触发的关系

| agent 类型 | 新模型映射 | 迁移 |
|---|---|---|
| manual | 动作（无规则） | 不迁移 |
| schedule(cron) | `cron` kind | 第二阶段可迁 |
| resident | `resident` + 手动入队 | 不迁移 |
| event | `event` kind（冷却/去重） | 第二阶段可迁 |
| daily | 无等价（「当天首次打开」不是墙钟规则） | **保留独立** |
| once | `once` kind | 第二阶段可迁 |

**明确建议：第一阶段不并入**。新模块只负责用户提醒与系统展示；动作层提供 `agent:<task_id>` 内部任务，用户规则可静默触发 agent，但不改 agent 自身触发路径。第二阶段把 `schedule` / `event` / `once` 三条时间触发切到 core 引擎，`daily` / `resident` / `manual` 留在 `module_agent`。
**迁移代价**：核心时间引擎抽取 1-2 人日；`trigger_config` 双读写兼容 1 人日；agent 回归与 UI 不回归 1-2 人日；合计约 **3-5 人日**，风险集中在「限次 / 冷却 / 错过」三条旧约定。
**前置重构**：`cron.h/.cpp` 现在 `module_agent` 命名空间（`src/module_agent/cron.h:28-39`），而 `core::schedule` 不能依赖 `module_agent`。建议 `git mv` 到 `src/core/cron.{h,cpp}`（namespace `core::cron`），在 `module_agent` 留一层 `using`/包装保持旧调用零改动；这是一次纯搬迁，需单独提交并跑 `--filter agent_cron`。
## 7. 工程约束（照抄项目纪律，不得发挥）

- **不抛 C++ 异常**：所有错误走 `Result<T>`（`src/core/result.h`）。
- **分层依赖**：`core` 不依赖 `platform`、不调用 EUI-NEO 运行时行为（仅 header-only 类型桥接例外）；`platform` 可依赖 `core`；`module_*` 可依赖 `core`+`platform`；`module_*` 之间不可互相依赖。
- **平台隔离**：平台无关接口在头文件，实现按平台拆 `*_win.cpp` / `*_macos.cpp` / `*_posix.cpp`，CMake 按平台选编；禁止在业务代码散布 `#ifdef _WIN32` / `__APPLE__`。
- **测试与构建**：每条用例对应具体业务场景并注明「防什么事故」，新测试文件必须登记 `tests/CMakeLists.txt`（参照 `tests/CMakeLists.txt:125`）；构建只走 `bash scripts/build-test-run.sh`，定向用 `--filter <子串>`，禁止手敲 `cmake -B` / `cmake --build`（诊断例外须报告写明理由）。
- **worktree 与并发**：主工作树禁跑构建，worktree 首建由脚本自动 configure；完成即原子 commit（显式 pathspec，禁 `git add -A`）并串行合入 `dev`；在飞子任务 ≤8 但只允许 1 个编译/测试，同一文件同一时刻一个写者，禁止 `sleep > 60s`，Bash 前台 timeout 默认 ≤3 分钟，长构建后台跑。
- **注释纪律**：只写契约、并发/线程约束、错误码语义、跨文件不变量与反重构警告；不写演变叙事、出处、复述代码。
- **不重写 ticker**：接口与判定顺序以 `ticker.h` 为准；新增调度逻辑放新文件。修改 ticker 契约必须先经用户拍板。
## 8. 未决问题（逐条给推荐，待用户拍板）

1. **规则表达选型**：自定义 `kind+spec_json`，还是 cron/RRULE 引擎？→ 推荐自定义六 kind；cron 仅作高级兼容，不引入 libical/libcron。
2. **补跑默认值**：所有规则默认补一次，还是按动作类型分？→ 推荐用户提醒 `once`、静默执行 `skip`，逐规则可改。
3. **系统定时是否落库**：复用规则表，还是只读派生？→ 推荐只读派生（`ScheduleProvider` 视图），运行记录写 `schedule_run` 但不建 FK。
4. **渠道失败默认降级**：每次重试都弹、最终弹一次，还是只记历史？→ 推荐最终失败降级本地 toast 一次并标红历史；不每次弹。
5. **智能体六触发是否合并**：第一阶段并入，还是先独立？→ 推荐第一阶段独立、动作层提供 `agent:<id>`；第二阶段迁 `schedule/once/event`。
6. **时区**：v1 是否只支持本地时区？→ 推荐只支持本地，预留 `tz_id` 字段，不引入 IANA tzdb。
7. **并发度是否可调**：→ 推荐重动作 1、HTTP 每渠道 2、本地提醒并行；v1 不暴露用户调参。
8. **一次性完成后的保留**：→ 推荐自动归档 + 保留，不自动删除；彻底删除需二次确认。
9. **删除规则的历史**：→ 推荐软删除保留 90 天，提供彻底删除。
10. **webhook @all**：→ 推荐默认不 @，规则级开关，仅高等级提醒可选。
11. **代理**：→ 推荐默认跟随应用代理，渠道可覆盖；公司固定代理地址需用户提供后配置，不写死本机调试值。
12. **系统通知渠道是否保留**：→ 推荐保留为低优先级渠道（无点击动作），高优先级走全屏。
13. **事件触发是否 v1 进入新模块**：→ 推荐 v1 不进入，继续由 agent EventBus 承担；新模块有独立事件源再扩展。
14. **cron 是否上移 core**：→ 推荐上移（`core::cron`）+ `module_agent` 兼容包装，避免双真源。
## 9. 验收标准（可执行判据）

1. 「每两周周二 09:00」，`anchor_date=2026-10-27`（周二），假时钟连续推进 8 周：触发时刻集合必须恰好为 `2026-10-27, 11-10, 11-24, 12-08` 的 09:00 各 4 次；单测逐时刻断言。
2. 「每月 15 日 10:00」从 1 月 31 日开始，下一次必须是 2 月 15 日；「每月 31 日」在 2 月跳过并于 3 月 31 日触发；「每月最后一个工作日」在 2026-10 结果为 10-30。
3. 「工作日 09:00」周六/周日不触发，周一触发；跨法定节假日 v1 不处理（文档明示）。
4. 一次性规则到点触发后 `archived_at` 非空、`enabled=0`，重启后不再触发；列表默认折叠。
5. 停机跨越 3 个到期点后启动：`skip` → 3 条 `status=missed` 记录、0 次动作；`once` → 1 次动作且记录标注原定时刻；`all` → 3 次动作（上限 100，超出合并为一条摘要）。
6. 同一规则同一 `planned_at` 在动作执行中重启应用：重启后只存在 1 条 `schedule_run`（唯一索引命中），不重复执行。
7. 注入 webhook 连续 3 次失败（HTTP 500 / 超时 / `errcode=45009`）：产生 3 条 `schedule_delivery`（0s/30s/300s），最终 `failed`，本地 toast 恰好 1 次，`schedule_run.status=success` 且投递记录可区分。
8. `errcode=93000/93001/93004`：不重试，渠道 `enabled=0` 并产生一条本地告警；`93017` 同样判永久失败。
9. `text.content` 超 2048 字节、`markdown` 超 4096 字节：按 UTF-8 边界截断（不切半个汉字），断言输出字节数 ≤ 上限且末字符合法。
10. DST 春季 02:30 不存在：在 03:00 触发一次；秋季 01:30 出现两次：只在第一次触发，第二次被去重键抑制。
11. 时钟从 10:00 回拨到 09:00：不重放 09:00-10:00 的触发；`last_eval_at` 更新为回拨后时刻，下次按新时间重算。
12. 100 条规则同一分钟到期：同规则同 `planned_at` 只有 1 条 run；重动作全局并发 ≤1；本地提醒全部入队不阻塞主线程。
13. 系统定时来源（release）出现在列表且只读；点击跳转 `source_route`；新引擎不重复执行、不产生双弹。
14. `bash scripts/build-test-run.sh --filter schedule` 全绿（`Failed: 0`）；新增用例已在 `tests/CMakeLists.txt` 登记；修复类用例有「先红后绿」证据。
## 10. 参考

| 内容 | 路径 |
|---|---|
| 调研文档（选型/限频/未决） | `D:/wt-rel-core/docs/superpowers/research/2026-10-28-scheduling-module-research.md` |
| ticker 契约与实现 | `src/core/ticker.h`、`src/core/ticker.cpp`、`src/main.cpp:641` |
| ticker 测试 | `tests/test_modules/test_ticker.cpp`、`tests/CMakeLists.txt:125` |
| 智能体调度 | `src/module_agent/scheduler.cpp`（200ms 线程、daily/once/schedule/event）、`src/module_agent/cron.h:28-39`、`src/module_agent/trigger_config.h` |
| 六触发定义 | `src/module_agent/task_draft_job.h:44-47` |
| 全屏提醒参考 | `src/module_release/release_module.cpp:88-115`、`src/module_release/release_reminder.h:1-40` |
| 平台能力 | `src/platform/notification.h`、`src/platform/http_client.h`、`src/platform/keychain.h`、`src/core/service_locator.h`、`src/core/time.h` |
| 工程纪律 | `D:/project/gui/DevDesk/CLAUDE.md`（分层/Result/平台隔离/测试登记/构建脚本/worktree/注释/派单） |