# 定时调度子系统调研（2026-10-28）

目标：把 `core::ticker` 之上长成完整调度子系统（触发 / 动作 / 通知渠道三层），同时给出与智能体现有六触发的关系与开源 C++ 选型。
标注：**[补]** = 调研方补齐、需用户确认；**[已核实]** / **[未核实]** 指外部事实核验状态。禁止把推测写成结论。

## 0. 结论先行

**推荐：自研「规则模型 + 单条 1s ticker 驱动 + 触发/动作/渠道三层分离」；不引入 libical / libcron；智能体六触发第一阶段保持独立，第二阶段再抽适配器。**

1. 用例（每两周周二、每月第 N 天、工作日、一次性、间隔、事件、常驻）用自定义模型 6 个 kind 就能全覆盖；cron 标准 5 段表达不了「每两周」，为它引入 RRULE 引擎的成本远高于收益（`FREQ=WEEKLY;INTERVAL=2;BYDAY=TU` 虽精确，但 libical 是 C 库 + 可选 ICU + 时区/VTIMEZONE 语义，见 §15）。
2. 现有 `nextCronMatch` / `previousCronMatch`（`src/module_agent/cron.h:28-39`）与本地时间工具 `core::time` 已能撑起时间计算；新增工作主要是「规则模型 + 下一次时刻 + 补跑判定 + 动作分发」，不是重造日历库。
3. 智能体调度自带线程、库表与「错过不补跑」语义（`src/module_agent/scheduler.cpp`），直接合并会把提醒场景的补跑/重试需求反向压进 agent；先做独立模块、用动作适配器复用「跑 agent 任务」，回归面最小。

## 1. 现有能力盘点

### 1.1 智能体六触发（`trigger_type`，`src/module_agent/task_draft_job.h:44-47`）

| 类型 | 实现 / 驱动 | 存储 | 错过/补跑 | 可复用性 |
|---|---|---|---|---|
| 手动 manual | 用户点「启动」走 `AgentModule::startTask`；无调度 | 无触发载荷 | 不适用 | 动作层可复用 |
| 定时 schedule(cron) | 独立 tick 线程 200ms 轮询、每分钟扫一次；`cronMatches(local tm)` | `agent_tasks.trigger_config={"cron":...}` | **错过分钟不补** | 可复用 cron 解析/匹配 |
| 常驻 resident | 列表「入队」→ `Scheduler::residentThreadLoop` 队列 | 无触发载荷 | 不适用 | 动作层可复用 |
| 事件 event | `core::event::EventBus` → `onEvent` 轻入队 → `processEvent`；任务×事件冷却默认 300s | `trigger_config.events[] / cooldown_sec` | 应用关着时事件直接丢 | 可复用事件模型 |
| 每天 daily | 每分钟扫；本地当天 `agent_runs` 无记录则派发一次 | 无触发载荷 | 当天没开应用 = 不跑 | 语义特殊，不建议直接映射 |
| 闹钟 once | `once_at` Unix 秒；窗口 `[once_at, once_at+60)`；派发后自动停用 | `trigger_config.once_at` | `now >= once_at+60` 即判错过，**不补跑**、自动停用 | 可复用 once 语义 |

关键位置：`src/module_agent/scheduler.cpp:22`（200ms 轮询）、`:339-345`（daily）、`:358-425`（once 窗口与错过）、`:426-455`（schedule + 同分钟防重）、`:305-315`（事件冷却）；`src/module_agent/trigger_config.h:33-56`（once_at/max_runs 互斥与限次）。

### 1.2 其它现有能力

| 能力 | 位置 | 现状 | 对本子系统的用法 |
|---|---|---|---|
| 全局帧泵 | `src/core/ticker.h:35-54`、`src/core/ticker.cpp:64-103`、`src/main.cpp:641` | 主线程独占、无锁；`{firstAt, intervalSec, expireAt}`；回调内可注销自己 | 调度引擎只挂**一条** 1s 订阅 |
| cron 解析/下次时刻 | `src/module_agent/cron.cpp`、`cron.h:28-39` | 5 段，纯函数，本地时间逐分钟回扫/前探，上限一年 | 直接复用或上移 core |
| 触发配置编解码 | `src/module_agent/trigger_config.h` | JSON，严格校验，含限次/冷却/一次性 | 作为新规则 JSON 的参考范式 |
| 运行记录 | `src/core/schema.h:160-171` `agent_runs` | 状态/起止/迭代/token/error，`status=running` 唯一索引 | 新子系统另建运行表，不复用 agent_runs |
| 全屏提醒 | `src/module_release/release_reminder.*`、`release_module.cpp:94-103` | 1s ticker；到期聚合开全屏窗；关闭后 5 分钟冷却；**重启首 tick 补弹（错过不吞）** | 可抽象为通知渠道；数据源与窗口外壳可复用 |
| OS 通知 | `src/platform/notification.h:27-74` | 托盘 NIF_INFO；标题 63/正文 255 UTF-16；去重 2s；限速 3 突发/1 每秒；**无点击动作** | 渠道之一；不能承担「跳转」 |
| 应用内 toast | `src/app_sidebar.h:81` | 跨页轻提示，自动消失 | 渠道之一 + webhook 失败降级 |
| HTTP | `src/platform/http_client.h:18-25`、`http_client.cpp:67` | libcurl 同步/流式；未暴露 per-request 代理，libcurl 默认吃环境代理，仅排除 localhost | 企业微信投递；需补 `Request.proxy` |
| 凭据 | `src/platform/keychain.h:41-49`、`keychain_win.cpp:1-5`、`credential_protection.h:3-4` | keychain 每 secret 一个 DPAPI 文件；credential_protection 为 DB 列加密 | webhook key 走 keychain，不落 sqlite |

**结论**：时间计算与 UI 外壳可复用；`Scheduler` 类本身与 `AgentStore/AgentLoop` 强耦合，不能直接当新子系统。

## 2. 触发模型全集（统一表达 + 下次时刻）

统一规则：`kind + spec_json`。所有日历字段按**本地墙钟**存，`next_fire_at` 另存 Unix 秒缓存；动作与触发彻底分离。

| kind | spec_json 关键字段 | 下次时刻算法 | 覆盖用例 |
|---|---|---|---|
| once | `at`(Unix 秒) 或 `at_local` | 直接取绝对时刻 | 闹钟、单次提醒 |
| interval | `every_sec`、`anchor`、可选 `window` | `anchor + ceil((after-anchor)/N)*N`，遇休眠/停机只算一次到期 | 每 30 分钟巡检 |
| calendar | `unit`(day/week/month/year)、`interval`、`weekdays[]`、`month_day` / `nth_weekday`、`time`、`anchor_date`、`workdays_only` | 本地日历推进；周规则以 `anchor_date` 所在周为第 0 周，`week_index % interval == 0`；月规则按第 N 天/第 N 个周几；不存在日期跳过 | 每两周周二 09:00、每月 15 日、每月最后一个工作日 |
| cron | `expr`(5 段，可选 6 段秒) | 复用 `nextCronMatch` | 兼容旧习惯 / 高级用户 |
| event | `events[]`、`cooldown_sec` | 无下次时刻；事件到达即判定 | 待办创建、笔记保存 |
| resident | 无 | 无下次时刻；仅手动/内部入队 | 值守类动作 |

「每两周的周二 09:00」在自定义模型里：`{"unit":"week","interval":2,"weekdays":[2],"time":"09:00","anchor_date":"2026-10-27"}`；`anchor_date` 必须是周二，否则校验失败（避免「每两周」歧义）。

**明确不支持**：cron 5 段无法表达「每两周」——字段只有分/时/日/月/周，没有「周奇偶」；Quartz 的 `L/W/#` 也没有周间隔。这是引入日历模型的直接原因（Windows Task Scheduler 也是单独加了 `WeeksInterval`，见 §17）。

## 3. 三类规则表达方案对比

| 方案 | 表达力 | 依赖/许可 | 实现复杂度 | 「每两周周二」 | 结论 |
|---|---|---|---|---|---|
| cron 5/6 段 | 分/时/日/月/周；Quartz 扩展有 L/W/# | 现有自研 186 行，零依赖 | 低 | **不能**；需额外 `week_interval` 字段 | 保留为高级 kind，不做唯一模型 |
| iCalendar RRULE (RFC 5545) | 最强：`FREQ=WEEKLY;INTERVAL=2;BYDAY=TU` 官方示例即「every other week」；含 COUNT/UNTIL/BYSETPOS/RDATE/EXDATE | libical MPL2.0 或 LGPL2.1；C 库 + 可选 ICU；静态构建需 CMake | 高（时区/VTIMEZONE/递归展开） | **能** | 仅当需要 iCal 导入导出/第三方日历时引入 |
| 自定义轻量模型 | day/week/month/year + interval + 星期集合/第 N 天/第 N 个周几/工作日 | 零依赖，复用 `core::time` | 中（约 300-500 行 + 测试） | **能**（需 anchor_date） | **推荐** |

推荐：自定义模型为唯一真源，UI 不暴露表达式；cron 作为「高级」可选 kind 复用现有解析器；RRULE 只留 `RuleCodec` 接口缝，未来需要再实现。

## 4. 时区与夏令时

- **存储口径**：日历/CRON 规则存「本地墙钟字段」，不存 UTC 偏移；`next_fire_at` 存 Unix 秒缓存（换时区/改规则时重算）。一次性规则按绝对时刻存 epoch。
- **多时区**：桌面单用户、公司场景默认跟随系统本地时区；规则表预留 `tz_id TEXT DEFAULT ''`，v1 不实现 IANA 时区库（`core::time` 只有 `localtime_s`/`localtime_r`，`src/core/time.h:11-18`）。
- **DST 春季跳变**（02:00 跳到 03:00，02:30 不存在）：在「最早存在的时刻」触发，即 03:00；与 Windows Task Scheduler 行为一致（[已核实]，见 §17）。
- **DST 秋季回拨**（01:30 出现两次）：只在第一次触发；以「本地日期 + 当地时分」为去重键，第二次同墙钟时刻直接抑制。
- **跨零点**：用 `mktime(tm_isdst=-1)` + `core::time::daysFromCivil` 推进日历日，不做 `+86400`（DST 日会偏 ±1h）。
- **时钟回拨/NTP**：若 `now < last_eval_at`，只更新 `last_eval_at`，不重放；回拨后下一次按新时间重算，避免重复触发。
- **休眠跨过触发点**：等同「错过」，由 §5 的补跑策略决定，不依赖 ticker 语义。

## 5. 错过与补跑 [补]

每条规则独立配置 `missed_policy`，默认值按动作类型分：

| 场景 | 默认策略 | 理由 |
|---|---|---|
| 用户提醒（弹窗/全屏） | `once`：启动/唤醒后补一次，并标注「原定 X 时间」 | 漏提醒比重复更伤；release reminder 已是「重启首 tick 补弹」口径（`release_reminder.h:6-7`） |
| 静默执行（agent/上线检查） | `skip`：不补 | 重复执行有副作用，与 agent「错过不补跑」一致（`scheduler.cpp:393-399`） |
| 一次性闹钟 | `skip` 或 `once`（用户选） | 到点后仍提醒一次通常有意义，但需限制迟到窗口（建议 24h） |
| 事件触发 | 无补跑概念 | 应用关闭期间事件不落库即丢 |

实现：`last_eval_at` 记录上次评估时刻；启动/唤醒时计算 `(last_eval_at, now]` 内所有到期点。`all` 策略最多补 100 次，超出部分合并为一条「遗漏 N 次」提醒；同一 `(origin, origin_key, planned_at)` 有唯一索引，重启不会重复补。

## 6. 冲突与串行 [补]

- **去重**：运行表唯一键 `(origin, origin_key, planned_at)`；同规则同计划时刻只产生一条 run。
- **同规则不并发**：run 状态 `running` 唯一（部分索引），第二条到期点入队等待，不启动。
- **同一时刻多条触发**：按优先级排队——系统定时 > 用户提醒 > 静默动作，同优先级按 rule id；轻量提醒可并行开窗，重动作串行。
- **并发度**：重动作（agent 任务/上线检查）全局 1；HTTP 渠道每渠道 2；本地提醒不限（UI 主线程只入队）。
- **队列满**：`skip` 并记 `suppressed`，不静默丢弃；用户可查运行历史。

## 7. 生命周期 [补]

| 状态/动作 | 语义 | 实现 |
|---|---|---|
| enabled | 正常参与调度 | `enabled=1` |
| paused | 暂停到某时刻，期间到期点不补（可配） | `paused_until` 非空 |
| disabled | 用户关闭；保留配置与历史 | `enabled=0` |
| 一次性执行后 | 自动归档，不删除 | `archived_at=now`，列表默认折叠 |
| 编辑 | 事务内重算 `next_fire_at`；正在跑的 run 不打断 | `updated_at` 变更即重算 |
| 删除 | 软删除 `deleted_at`，历史保留 90 天 | 提供「彻底删除」二次确认 |

## 8. 执行记录与失败重试

每次触发（含被跳过/被抑制）写一条 `schedule_run`：计划时刻、实际开始、结束、耗时、结果摘要、错误、状态。状态：`pending|running|success|failed|skipped|missed|suppressed`。

- 动作失败：按动作类型重试（agent 任务不自动重跑，交给原任务的重试语义；内部检查任务可配 1 次）。
- 渠道失败：见 §10，单独记 `schedule_delivery`，不影响 run 的 success（run 成功、投递失败要能分辨）。
- 运行历史 UI 展示「计划 vs 实际」；复用 agent 的 `previousCronMatch` 口径（`cron.h:34-39`）做对照。

## 9. 数据模型

**用户定时（可编辑）`schedule_rule`**：`id, name, enabled, paused_until, kind, spec_json, time_mode(local|utc), tz_id(空=系统本地), action_kind, action_json, missed_policy, conflict_policy, max_runs, next_fire_at, last_fire_at, last_eval_at, run_count, archived_at, deleted_at, created_at, updated_at`。

**运行记录 `schedule_run`**：`id, rule_id(NULL 允许), origin('user'|'system:<module>'), origin_key, planned_at, started_at, ended_at, status, attempt, duration_ms, result_summary, error`；唯一索引 `(origin, origin_key, planned_at)`。

**投递记录 `schedule_delivery`**：`id, run_id, channel_id, attempt, status(pending|success|failed|retry|dropped), response_code, remote_errcode, error, next_retry_at, created_at`。

**渠道配置 `schedule_channel`**：`id, kind, name, enabled, config_json(endpoint/msgtype/mentions/proxy_mode/timeout), secret_ref, created_at, updated_at`；`secret_ref` 只是 keychain 的 account 名，**key 本身不落库**。

**系统定时（只读，不落库）**：不建表。由各模块实现 `ScheduleProvider`，返回视图项：
`{source, key, title, summary, next_fire_at, enabled, source_route, executable}`。
列表在刷新时合并「DB 用户规则 + providers 视图」，按 `next_fire_at` 排序。`origin_key` 用稳定合成键（如 `release:task:<id>:precheck`），运行记录可写入 `schedule_run` 但不建 FK；删来源模块后历史仍在。**`[补]` 这是对「系统定时不落库又要同列表」的推荐解法；是否接受需确认。**

## 10. 通知渠道层

### 10.1 抽象

动作与渠道是**两个正交维度**，不要用一个「执行方式」枚举把二者耦合死：
- 动作（做什么）：`remind`（仅提醒）/ `remind_open`（提醒并跳转 deeplink）/ `run_task`（静默调内部任务，如 `agent:<id>`、`release:<check>`）/ `webhook_only`（只投递）。
- 渠道（送到哪）：下表注册表。

渠道注册表 + 统一接口：`Channel::deliver(Delivery{title, body, level, deeplink, run_id, dedupe_key}) -> Result<Receipt>`。内置渠道：全屏提醒、应用内 toast、OS 通知、静默（空渠道）、企业微信 webhook。规则上 `action_json.channel_ids[]` 选渠道，**不允许在动作代码里 if(wecom)**。

### 10.2 企业微信 webhook（[已核实]，官方文档 2025-08-07 版）

| 项 | 结论 |
|---|---|
| 端点 | `POST https://qyapi.weixin.qq.com/cgi-bin/webhook/send?key=<key>`，JSON body |
| msgtype | text / markdown / markdown_v2 / image / news / file / voice / template_card |
| 长度 | text `content` ≤ **2048 字节**；markdown ≤ **4096 字节**；markdown_v2 ≤ 4096 字节（且不支持 @） |
| 限频 | 每个机器人 **20 条/分钟**（官方文档明文） |
| @ 人 | text：`text.mentioned_list` / `mentioned_mobile_list`，值可为 userid/手机号/`@all`；markdown：正文内 `<@userid>`（markdown_v2 不支持） |
| 错误判定 | 必须解析 body 的 `errcode`（不能只看 HTTP 状态）；`45009` 接口调用超限、`45033` 并发超限、`93000` webhook 非法/被移除、`93001` 群不允许推送、`93004` 已停用、`93017` 空请求。**45009 是否即机器人限频所用码，机器人文档未单列，[未核实]，上线前实测一次** |
| 可重试 | 网络错误 / HTTP 5xx / 45009（等待 60s）/ 45033（指数退避）；93000/93001/93004 与 4xx 判永久失败，停用渠道并本地告警 |
| 幂等 | 无服务端幂等键，靠 `(rule, planned_at)` 去重；不要在重试时改正文 |

### 10.3 网络

走 `platform::HttpClient`（libcurl 静态链接，`src/platform/http_client.h:18-25`）。当前 `Request` 无代理字段，libcurl 默认读取 `http_proxy/HTTPS_PROXY/ALL_PROXY` 环境变量，并排除 localhost（`http_client.cpp:67`）。**[补]** 需给 `Request` 增加 `proxy`/`proxy_mode`，渠道配置提供「跟随系统 / 不使用 / 自定义」；自定义代理属于渠道配置，优先级高于应用全局代理。本机 10808 仅用于本次调研联网，不写死进产品（[已核实]：本机 socks5 与 http 两种协议访问 api.github.com 均返回 200）。

### 10.4 凭据

webhook key 存 `platform::keychain`：service `devdesk.schedule`，account `wecom.<channel_id>`；Windows 落 DPAPI 密文文件（`keychain_win.cpp:1-5`），**不写 sqlite、不进日志**。UI 只显示 `****` 与「测试发送」；轮换 key 即更新 keychain 后重建渠道。不要复用 `claude:` 前缀的孤儿清理语义（`keychain.h:14-18`）。

### 10.5 可靠性

每渠道令牌桶（目标 18 条/分钟，留 2 条余量）；失败重试 3 次（0s / 30s / 300s，指数退避），每次写 `schedule_delivery`；最终失败降级本地 toast 一次，并写 run 结果；连续 5 次失败自动停用渠道 + 全屏/本地告警，避免静默丢弃。队列上限默认 100 条，超出记 `dropped` 并告警。

### 10.6 可扩展成本

加钉钉/飞书/邮件：**新增 1 个渠道实现文件 + 1 行注册**；若字段与通用表单不同，再加 1 个表单 schema；DB 无迁移（`config_json`）。即合格线是「2-3 处」，超过说明渠道抽象不合格。

## 11. 分层边界与接口

```
ticker (core)         仅帧泵：{firstAt, intervalSec, expireAt}，主线程无锁
  └ schedule (core)   规则模型 + nextFire/missedFires + 到期队列（纯函数优先）
      └ module_schedule (UI/持久化/动作/渠道)  DB、列表、编辑、运行历史
          ├ action 分发：提醒 / 提醒并跳转 / 静默内部任务 / webhook
          └ channel 注册表：全屏 / toast / OS / 静默 / 企业微信
app_assembly          用 ServiceLocator 注册模块 provider 与内部任务回调
```

- `core` 不依赖 `EUI-NEO`/`platform`；动作需要的 HTTP/通知/keychain 全在 `module_schedule`。
- 系统定时来源通过 `core::ServiceLocator`（`service_locator.h:1-6`，已有 `IClaudeProxyService`/`LlmProfileSource` 先例）注册 `ScheduleProviderRegistry`，模块之间零 include。
- 建议接口：`Result<optional<int64_t>> nextFire(spec, after_utc, nowCtx)`、`Result<vector<int64_t>> missedFires(spec, from, to, cap)`、`ScheduleProvider::items()`、`Channel::deliver()`、`ActionRunner::run()`。

### ticker 用法结论（是否「每次触发后重算并重注册」）

**成立但不推荐逐规则重注册。** 推荐一条 `{name="schedule.engine", firstAt=0, intervalSec=1, expireAt=0}`，引擎内部算到期；理由与坑：

1. `pump` 先清过期、再判 `now>=firstAt`、再判间隔（`ticker.cpp:64-103`）；`expireAt == firstAt` 会在触发前被删，一次性规则不能靠 `expireAt` 收尾。
2. 回调内可 `unsubscribe`/`subscribe`，pump 用句柄快照（`ticker.h:19-21`）；但逐规则重注册会放大「重算遗漏/回调忘记注销」的风险。
3. `intervalSec` 从**上次触发**起算，不是日历对齐；日历规则重注册必须自己算绝对 `firstAt`。
4. 主循环停摆/休眠后只会在下一次 pump 触发一次，这天然是 `once` 补跑；要实现 `skip` 必须在引擎里比较 `planned_at` 与 `now`。
5. ticker 非线程安全；动作线程不得直接 subscribe/pump，必须回主线程（`ticker.h:3-4`）。
6. 秒级分辨率足够（cron 分钟级、日历分钟级）；多次 pump/秒由 `intervalSec=1` 限流。

## 12. 与智能体六触发的关系

| agent 类型 | 新模型映射 | 是否迁移 |
|---|---|---|
| manual | 动作（无规则） | 不迁移 |
| schedule(cron) | `cron` kind | 第二阶段可迁移 |
| resident | `resident` + 手动入队 | 不迁移 |
| event | `event` kind（冷却/去重） | 第二阶段可迁移 |
| daily | 无等价（「当天首次打开」不是墙钟规则） | **保留独立** |
| once | `once` kind | 第二阶段可迁移 |

**建议：第一阶段不并入**，新模块只负责用户提醒与系统展示；动作层提供 `agent_task:<id>` 内部任务，用户规则可静默触发 agent 任务（不影响 agent 自身触发）。第二阶段把 `schedule/event/once` 三条时间触发切到 core 引擎，`daily/resident/manual` 留在 AgentModule。

迁移代价（估算，含测试）：核心时间引擎抽取 1-2 人日；`trigger_config` 双读写兼容 1 人日；agent 回归测试与 UI 不回归 1-2 人日；总计约 **3-5 人日**，风险集中在「限次/冷却/错过语义」三条旧约定。[补]

## 13. UI 面

- 侧边栏模块「定时」；列表两段：**系统定时**（锁图标、来源徽章、只读、点击跳来源模块）与**用户定时**（开关/编辑/删除）。
- 列：名称 / 来源 / 规则摘要（「每两周 周二 09:00」）/ 下次 / 上次结果 / 开关；一次性且已执行的进入「已归档」折叠区。
- 编辑弹窗：类型页签（一次性 / 间隔 / 周期 / 日程(cron) / 事件 / 常驻）；周期表单直接给「每 N 周 [周几] HH:MM」「每月 [第 N 天|第 N 个周几]」「工作日」；实时预览接下来 5 次；动作选择（仅提醒 / 提醒并打开 / 静默执行）；渠道多选；错过策略；保存前校验。
- 运行历史抽屉：计划/实际、状态、耗时、错误、各渠道投递与重试次数、手动重试。
- 系统行：无开关、无编辑；「在来源中编辑」跳转；来源不可用时灰显并说明。

## 14. 可扩展性接口缝

- **第三方日历（CalDAV/iCal）**：`ScheduleSource` 提供者（只读导入或双向同步），与系统定时 provider 同一注册表；RRULE 编解码走 `RuleCodec`。
- **系统级计划任务（Windows Task Scheduler / launchd）**：`platform/schedule_backend.h` 定义 `list/upsert/remove`；Windows COM `ITaskService`、macOS launchd plist 各自实现；模块层按平台选择，不影响规则模型。
- **通知渠道**：注册表（§10.6）。
- **内部任务**：`InternalTaskRegistry`（`agent:<id>`、`release:<check>`），由 app_assembly 注册，动作层只认 key。

## 15. 开源 C++ 实现选型（[已核实]，GitHub API 2026-10-28）

| 候选 | 许可 | header-only / 静态 | C++ 标准 | 「每两周周二 09:00」 | 跨平台 | 维护/体积 | 依赖 |
|---|---|---|---|---|---|---|---|
| mariusbancila/croncpp | MIT | header-only；可静态 | C++11+ | **不能**（仅表达式解析/下次时刻，无周间隔） | Windows/macOS/Ubuntu CI 覆盖 | 279 star；pushed 2026-08-12；347 KB | 零 |
| PerMalmberg/libcron（`Snarp/libcron` 为 404，实际同名库见此处） | MIT | **非 header-only**（src/*.cpp 静态库） | C++17 | **不能** | CI 仅 Ubuntu；Windows/macOS **[未核实]** | 145 star；pushed 2025-10-14；仓库 3.99 MB | 内置 Howard Hinnant date |
| voidlib/cron | — | — | — | — | — | **仓库 404，不存在** | — |
| Quartz C++ 移植 | — | — | — | 理论可扩展，但无成熟实现 | — | GitHub 搜索未发现 >100 star 的维护版；Quartz 本体是 Java、Quartz.NET 是 C# | — |
| libical/libical | **MPL 2.0 或 LGPL 2.1（二选一；LGPL 静态链接有回馈义务）** | C 库 + 可选 C++11 绑定；`-DLIBICAL_STATIC=True` 可静态 | C++ 绑定 C++11 | **能**：`FREQ=WEEKLY;INTERVAL=2;BYDAY=TU`（RFC 5545 官方示例） | Windows 2022/macOS/Ubuntu CI；MinGW 有文档 | 367 star；pushed 2026-10-21；仓库 41 MB | CMake；ICU 可选但「强烈推荐」，可 `-DCMAKE_DISABLE_FIND_PACKAGE_ICU=True` 关闭；二进制体积未核实 |
| HowardHinnant/date | MIT（逐文件） | header-only（date.h） | C++11/14/17 | **不能**（只做日历运算，无递归规则） | 广泛，跨平台 | 3438 star；pushed 2026-10-27；仓库 3.5 MB | 零 |

**三选一推荐**：

| 选项 | 代价 | 适用 |
|---|---|---|
| **A. 自研轻量模型（推荐）** | 300-500 行 + 单测；复用现有 cron/`core::time`；无许可风险；2-3 人日 | 本项目：桌面提醒 + 少量规则，无 iCal 互操作 |
| B. libcron | MIT、静态可链；但 6 段语法与现有 5 段冲突、仍不支持每两周、自带 tick 线程与全局 ticker 重复；适配成本 ≥ 自研 | 如果需要现成 Cron 对象与多线程锁 |
| C. libical | 完整 RRULE/时区/iCal；但 C+CMake+可选 ICU、41 MB 源码、VTIMEZONE 语义与许可审查；1-2 周集成 | 硬需求是导入 Outlook/Google 日历或导出 .ics |

## 16. 未决问题（逐条给推荐答案）

1. **用户提醒默认补跑**：推荐 `once`（启动/唤醒补一次，标注迟到），静默动作默认 `skip`；逐规则可改。是否接受？
2. **智能体六触发是否并入**：推荐第一阶段不并入、仅提供 `agent_task` 动作；第二阶段迁移 schedule/once/event，`daily` 与 `resident` 保留。是否按两阶段走？
3. **系统定时是否由新引擎执行**：推荐系统项默认只读展示、执行仍归来源模块（避免 release 双弹）；来源可注册 `executable=true` 才由新引擎执行。是否接受？
4. **企业微信是全局一个还是每规则一个**：推荐全局渠道注册表、规则引用 `channel_ids`；key 由用户在设置页维护，不随规则复制（避免多份密钥与轮换遗漏）。
5. **webhook 失败是否降级本地弹窗**：推荐最终失败降级一次本地 toast + 运行历史标红；不每次重试都弹。
6. **时区**：推荐 v1 只支持本地时区，保留 `tz_id` 字段；不引入 IANA tzdb。是否需要跨时区提醒？
7. **并发度**：推荐重动作 1、HTTP 每渠道 2、本地提醒并行；是否需要用户可调？
8. **一次性任务完成后**：推荐自动归档 + 90 天保留，不自动删除；是否需要「保留到用户确认」？
9. **删除规则的历史**：推荐软删除保留 90 天，提供彻底删除；是否接受？
10. **`all` 补跑上限**：推荐 100 次，超出合并为一条摘要；上限是否够？
11. **企业微信 @all**：推荐默认不 @，规则级开关，仅高等级提醒可选；是否接受？
12. **代理**：推荐默认跟随应用代理、渠道可覆盖；公司网络是否要求固定代理地址（本机 10808 只是调试值）？
13. **系统通知渠道是否保留**：推荐保留为低优先级渠道（无点击动作），高优先级用全屏；是否需要「点击通知打开应用」？（当前 Windows 后端无此能力，`notification_win.cpp:16-17`）
14. **事件触发是否进入新模块**：推荐 v1 不进入，继续由 agent EventBus 承担；等新模块有独立事件源再扩展。

## 17. 参考来源

本地（均为 `D:/wt-rel-core` 下路径）：
- `src/core/ticker.h:6-54`、`src/core/ticker.cpp:64-103`、`src/main.cpp:641`（帧泵契约）
- `src/module_agent/task_draft_job.h:41-59`（六触发标签与语义）
- `src/module_agent/scheduler.cpp:22,305-315,339-345,358-455`（daily/once/schedule/event 与错过）
- `src/module_agent/cron.h:24-39`、`cron.cpp`（5 段解析、下次/上次时刻）
- `src/module_agent/trigger_config.h:15-56`（once_at / max_runs / cooldown）
- `src/core/schema.h:144-171`（agent_tasks / agent_runs）
- `src/module_release/release_reminder.h:3-10`、`release_module.cpp:88-115`（全屏提醒与补弹）
- `src/platform/notification.h:27-74`、`notification.cpp:28-35`、`notification_win.cpp:1-17`（系统通知能力边界）
- `src/platform/http_client.h:18-25`、`http_client.cpp:67`（libcurl 代理/NOPROXY）
- `src/platform/keychain.h:41-49`、`keychain_win.cpp:1-5`、`credential_protection.h:3-4`（DPAPI 凭据）
- `src/core/service_locator.h:1-6`（跨模块 provider 注册）
- `src/core/time.h:11-18`（本地/UTC 转换、daysFromCivil）

外部（[已核实]）：
- 企业微信「消息推送配置说明」：https://developer.work.weixin.qq.com/document/path/91770 （text 2048 字节、markdown 4096 字节、20 条/分钟、mentioned_list/@all、`<@userid>`；页面标注最后更新 2025/08/07）
- 企业微信「全局错误码」：https://developer.work.weixin.qq.com/document/path/90313 （45009/45033/93000/93001/93004/93017）
- RFC 5545 §3.3.10 RRULE（`FREQ=WEEKLY;INTERVAL=2;BYDAY=TU,TH` 官方「Every other week」示例）：https://www.rfc-editor.org/rfc/rfc5545.txt
- Windows `WeeklyTrigger.WeeksInterval`（含 DST 02:00-02:59 顺延到 03:00 的行为）：https://learn.microsoft.com/en-us/windows/win32/taskschd/weeklytrigger
- Windows `TaskSettings.StartWhenAvailable`（错过任务可延后启动，默认延迟 10 分钟）：https://learn.microsoft.com/en-us/windows/win32/taskschd/tasksettings-startwhenavailable
- Quartz Trigger misfire（IGNORE / DO_NOTHING / FIRE_NOW / SMART_POLICY；CronTrigger 的 smart = FIRE_NOW）：https://www.quartz-scheduler.org/documentation/quartz-2.3.0/tutorials/tutorial-lesson-04.html 、 `.../tutorial-lesson-06.html`
- launchd.plist `StartCalendarInterval`（睡眠错过后唤醒补跑，多次间隔合并为一次）：https://keith.github.io/xcode-man-pages/launchd.plist.5.html
- GitHub API 元数据：`mariusbancila/croncpp`、`PerMalmberg/libcron`、`libical/libical`、`HowardHinnant/date`（2026-10-28 查询）；`Snarp/libcron`、`voidlib/cron` 均返回 404