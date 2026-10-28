# 定时调度子系统 · 独立审计 B（分层依赖/模块隔离/迁移/测试有效性/文档口径）

- 基线：dev 分支 tip `1bdc500d`（范围 `f42ea2c4..dev`，merge-base = f42ea2c4，范围干净）
- 方式：只读（全部源码经 `git show dev:<path>` 取证，不读工作区——工作区为另一分支且调度文件已被删除）
- 范围排除已遵守：S7a（UI/装配）、整合 A/B、引擎跨波对齐点、wecom 注册接线、令牌桶全局口径未报
- 审计时间：2026-10-28

## 维度 1：分层依赖铁律

已核，core 侧未发现问题：

- core 新增三文件（ticker/cron/schedule_rule）include 面只有 core 内部头（result.h/logging.h/time.h/cron.h）+ 第三方 header-only `yyjson.h`（EUI-NEO 3rd，core/CMakeLists.txt:65/72/87 在本范围之前已接线，platform/json.cpp 同源用法）。无 platform、无 EUI 运行时、无 module_* include。
- module_schedule 全部文件零 module 间 include（未引用 module_agent/module_release 等源码）；对 core/platform/ui_kit 的依赖方向合法。

| 条目 | file:line | 问题 | 影响 | 严重度 |
|---|---|---|---|---|
| toast 渠道反向依赖 app 层 | src/module_schedule/channels/toast_channel.cpp:7 `#include "app_sidebar.h"`（调 `app::showAppNotice`） | 渠道层（module_schedule）依赖 app 层符号——`app_sidebar.cpp` 是主程序源文件、不在任何库中；依赖方向倒置（应为 app → module）。当前仅因测试二进制直编 app_sidebar.cpp 才能链接 | 宿主波把渠道层并入模块库/主程序时，除非 app 层符号与渠道同链接集，否则 link 失败；module 依赖 app 破坏分层契约 | 中 |

## 维度 2：cron 上移回归面

| 条目 | file:line | 问题 | 影响 | 严重度 |
|---|---|---|---|---|
| module_schedule 库不自洽：渠道层未登记进模块库 | src/module_schedule/CMakeLists.txt（库内无 channel_registry.cpp 与 fullscreen/toast/os/silent 四渠道；channels/wecom.cpp 却在库内，登记口径分裂） | schedule_engine.cpp:64、action_runner.cpp:89 引用 `ChannelRegistry&`（channel_registry.h），实现只登记在 tests/CMakeLists.txt:12-16 随测试二进制直编。该库已被 add_subdirectory + 构建，但 DevDesk 主程序未链接它（根 CMakeLists 注释「本波不注册」）。现状测试二进制能链接（直编对象提供符号），主程序一旦接入该库而未同步补登记渠道源文件即 undefined reference | 宿主波接入 app 时必现 link 失败；库自身不是自洽交付单元 | 中 |
| module_schedule 链接依赖重复行 | src/module_schedule/CMakeLists.txt:20-22 `devdesk_platform` 出现两次 | 无害冗余 | 仅噪音 | 低 |

已核，未发现问题：

- 转发头 src/module_agent/cron.h 保留 CronExpr/parseCron/cronMatches/nextCronMatch/previousCronMatch 五个 using 声明；实际调用点 12 处 / 7 文件（scheduler.cpp:435/448、trigger_config.cpp:87、agent_module_schedule.cpp:348/356/494/593、agent_time_text.cpp:205/209/304/311、task_draft.cpp:387、agent_module_dialogs.cpp:320），全部为 module::agent 命名空间内非限定调用，转发头全覆盖、零改动可用。交接文档「六处调用点」为约数（实际 12 处），非缺陷。
- core/CMakeLists.txt：cron.cpp/ticker.cpp/schedule_rule.cpp 均已加；module_agent/CMakeLists.txt 已删 cron.cpp；无残留对旧 cron.cpp 的引用。test_agent_cron.cpp 仍在登记内。

## 维度 3：迁移与老库

已核，未发现问题：

- kSql 幂等（CREATE TABLE/INDEX IF NOT EXISTS）；四表 DDL 在脚本中段、四索引在其后同脚本、种子 INSERT 在最末且守卫只涉 todo_columns——顺序无相互影响。启动路径 main.cpp:185 → Database::migrate → core::schema::migrate（schema.cpp:733）→ applySchema(kSql)（:735），老库/空库同一条路。
- 与 H30 既有迁移（todo 冷启动归档清扫，schema.h:524，applySchema 之后执行）零交集（只动 todo_columns）。
- 双源一致性：唯一迁移源即 kSql；唯一索引 idx_schedule_run_unique 库层硬挡重复 run，insertRun 冲突返回 AlreadyExists（schedule_store.cpp:551），并有绕过 store 的裸 SQL 测试（test_schedule_store.cpp:164）。

| 条目 | file:line | 问题 | 影响 | 严重度 |
|---|---|---|---|---|
| 老库升级路径无针对性用例 | tests/test_modules/test_schedule_store.cpp:73 `schedule_store_migrate_idempotent` | 「老库反复打开」= 建表后二次启动（schedule 表已存在再跑 migrate），未覆盖「旧版库（有旧表、无 schedule 表）首次升级新增四表」的路径 | 该路径与幂等路径代码同源（IF NOT EXISTS），实际风险低；仅覆盖缺口 | 低 |

## 维度 4：测试有效性

逐条判定（全部用例名已过目）：

- test_ticker（7 条）：计数/句柄/时序断言真实，可红。✔
- test_schedule_rule（14 条）：once/interval/calendar/cron/monthly/workday 断言具体时刻值，可红。✔
- test_schedule_store（7 条）、test_schedule_engine（12 条，run 计数/状态分布/串行度/归档/编辑重算断言具体）、test_channels（12 条）、test_wecom_channel（20 条）、test_http_proxy（6 条）、test_schedule_ui_edit（15 条）、test_schedule_ui_history（4 条）：未发现恒真断言、断言写反或引用不存在 API。✔

| 条目 | file:line | 问题 | 影响 | 严重度 |
|---|---|---|---|---|
| 两条 DST 验收用例在本机恒真通过 | tests/test_modules/test_schedule_rule.cpp:390 `schedule_dst_spring_gap`、:451 `schedule_dst_fall_dedupe` | 入口 `if (!zoneHasDst()) return true;`——本机（中国标准时区无 DST）及 UTC CI 上直接跳过，跳过与通过不可区分。验收标准 §9.10 在本机/常见 CI 无任何实测覆盖 | 手写 DST 语义（firstAndLastOccurrence/resolveWallClock 跳变前探）在唯一会跑它的环境里从未被执行；「测试绿」对 §9.10 是空证据 | 中 |

## 维度 5：文档口径

已核，未发现问题：

- §2.4 红线兑现：schedule_engine.cpp:82-96 全局只挂一条 `{name="schedule.engine", firstAt=0, intervalSec=1, expireAt=0}`，start() 重挂前先注销（幂等），一次性规则不借 expireAt 收尾。
- §4.8 时区纪律：schedule_rule.h 文件头注释（isdst 两遍试探、daysFromCivil 推进、禁 +86400、回拨不重放、秋季去重）与实现逐条一致。

| 条目 | file:line | 问题 | 影响 | 严重度 |
|---|---|---|---|---|
| §11 台账与实际波次状态不符 | docs/superpowers/plans/2026-10-28-scheduling-subsystem-handoff.md §11 | dev tip 已合入 S4（6e80f921/1bdc500d）、S6b（7c8fafcc）、S7b（33e90e0a/6f6aae8a），台账仍标 S4/S6b/S7「⬜ 排队」；S7b 波次无台账行（S7 行未拆分） | 接手会话按台账派单会重复开发已合入波次 | 中 |
| 头文件契约与实现措辞矛盾：calendar 无下次返回 fail 而非 nullopt | src/core/schedule_rule.h:80-81（宣称「超出可表示范围…返回 ok(nullopt)」）vs src/core/schedule_rule.cpp calendarNextFire 末尾返回 fail(ValidationError「400 年内无触发时间」) | calendar.interval 校验允许 ≤1,000,000，但搜索窗仅 146097 天（400 年）——合法配置（如 interval=1e6 天）被 fail 拒绝，与头文件契约不符 | 调用方按契约处理 ok(nullopt) 会漏接 fail；边界配置被误拒 | 低 |
| §0「提交状态：未提交 ticker」为落笔时快照 | 交接文档 §0 | c508a662 已把 ticker 三文件+两处 CMake 注册合入 dev，§0 描述已过时 | 接手会话可能重复「原子提交三件未跟踪文件」动作 | 低 |
