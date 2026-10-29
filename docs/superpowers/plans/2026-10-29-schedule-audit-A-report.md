# 调度线审计 A 报告（范围重定：当前工作树态）

- 日期：2026-10-29。审计者：code-reviewer 隔离子任务（只读静态审计，含模块隔离守卫复跑）。
- 范围：`src/module_schedule/` 全部 + `src/core/schedule_rule.*` + 相关测试；对照 `2026-10-28-scheduling-subsystem-handoff.md` 验收标准与 `2026-10-28-scheduling-line-audit-B.md`。
- 对象态：S1–S7a + 收口批次 1/2 已落地未提交；全量基线 1934/1928/0（审计时点）。

## 一、分级问题清单与处置

### P0
无。四表唯一索引 + insertRun `ON CONFLICT DO NOTHING` + 补跑前 runExists 三层防重，数据不丢不重。

### P1（5 条）

| # | 位置 | 问题 | 处置 |
|---|---|---|---|
| P1-1 | schedule_engine.cpp:183 | 禁用规则不推进 last_eval_at，重启用后整段禁用期被当「错过」补跑（once 补发陈旧动作 / all 最多 100 连发 / skip 写满 100 条 missed）；drainPendingFires 出队前也不复核 enabled | **R1 已修**（禁用逐 tick 推进 + 过期缓存推走 + drain 出队复核，6 用例先红后绿） |
| P1-2 | action_runner.cpp:31-37 + 240-251 | missing_secret 被归永久失败 → 未配 key 渠道第一次投递即被停用且无恢复入口（wecom.h:144 自己定义为配置问题；§4.9 只允许 93000 类停用） | **R1 已修**（kConfigMissing 单列：dropped 终态、不停用、不重试 + 降级提示） |
| P1-3 | schedule_ui_edit.cpp:440 | **编辑弹窗生产侧零接线**：drawEditDialog/EditDialogHost/saveRule 除测试外零调用——用户「增删改/暂停」全无 UI 入口（spec §4.2 欠账），且 TODO §B 未登记此项跟踪。关联：EditFormState.actionJson 无输入控件（remind_open 产不出 deeplink、run_task 永远空 key）；schedule_ui_edit.h:77 写 `"task_key"` 而 action_runner.cpp:60 只取 `"key"`——键名不一致 | **待派 R3**（UI 接线波：挂载 + actionJson 三字段 + 键名统一 + 删除/暂停入口） |
| P1-4 | app_assembly.cpp:68-70 vs action_runner.h:73-84 | 同名类 InternalTaskRegistry 双定义，装配点注册的是引擎永不读的那个；run_task 端到端必败 | **R2 处理中**（删 internal_task_registry 死真源 + run_task 任务源现状核实） |
| P1-5 | channel_registry.cpp:23-27 | wecom 以默认配置实例注册，per-row 装配缝 registerWecomChannel 零调用；keychain devdesk.schedule 无写入 UI——多行配置永远落不到第二行（注册表按 kind 串去重） | **裁决：v1 单 wecom 渠道行**（默认配置；per-row 装配 + key 写入 UI 记 backlog，等编辑弹窗波一起看）；行级配置需求出现时再升级 |

### P2（14 条）

| # | 位置 | 问题 | 处置 |
|---|---|---|---|
| 6 | schedule_ui_list.cpp:203/212 | specInt 读不存在的键（nth_weekday_nth 应读 nth_weekday 对象；year 摘要读 month 应读 month_day）——「每月第 N 个周几」摘要恒回退 | R3 随批 |
| 7 | schedule_engine.cpp:128-131 | listRules 失败早退跳过 flushAllToMainThread | **R1 已修**（两处早退都补） |
| 8 | schedule_engine.cpp:302-303 | 每秒对每条活跃规则整行 updateRule（写放大 + updated_at 被刷） | **R1 已修**（内存推进 + 边界落库；崩溃安全经唯一索引兜底论证；禁用/暂停期仍逐 tick 落库——该期间无 run 行兜底，必须持久化；后续可加 store 单列 UPDATE 收尾） |
| 9 | action_runner.cpp:262-270 | completeAsync 死代码且注释与行为相反 | R2 处理中（删除） |
| 10 | schedule_engine.h:117 | 注释「执行前会重读最新行」未兑现 | R2 处理中（核 R1 drain 复核后改注释或补实现） |
| 11 | schedule_rule.h:90-91 vs .cpp:610-611 | calendar 400 年无触发 fail(ValidationError) vs 头文件承诺 ok(nullopt)；合法长 interval 被误拒 | 待修（一行级，随 R3 或 R4） |
| 12 | 全链 | skipped 状态无人写；崩溃遗留 running 行永悬；90 天历史保留无清理；max_runs 双不消费 | 登记为 v1 有意边界（schedule_engine.h 边界清单）或后续卡 |
| 13 | action_runner.cpp:167-171 | slotBusy 重试不记账但引擎统一 attempt+=1（虚耗配额；当前渠道全同步不触发，潜伏） | R2 处理中 |
| 14 | TODO.md:43-51 | §B 6 条中 5 条已落地未销账（会误导重复派单） | 主会话已刷新 TODO |

## 二、规格逐条核对（摘要）

已覆盖：§4.1 六 kind、§4.2 系统定时展示、§4.5 run 状态机、§4.6 补跑三策略+上限+唯一索引、§4.7 串行/并发/队列满、§4.8 DST/回拨/跨零点、§9.1-9.11/9.14。
缺口：§4.3 四动作（run_task/remind_open 载荷不可达，P1-3/P1-4）、§4.4 增删改/暂停 UI（P1-3）与 90 天保留（P2-12）、§4.9 wecom 凭据写入入口（P1-5）与 missing_secret 分类（P1-2 已修）、§9.12 100 规则规模用例（部分）、§9.13 系统来源不双弹（S8 未接，已登记）。

## 三、审计 B 余项核对

| 余项 | 状态 |
|---|---|
| devdesk_platform 链接重复行 | 已修 |
| 老库（无 schedule 表）首次升级用例 | 未修（test_schedule_store.cpp:73 仍是幂等路径）——记 R4 |
| calendar 400 年 fail vs ok(nullopt) | 未修（=P2-11） |
| 交接文档 §0/§11 快照过时 | 未修——R4 文档刷新一并 |

审计 B 已修项：toast 反向依赖、渠道源入库、DST 恒真、库不自洽、全局令牌桶。审计 A 新增发现：internal_task_registry 双真源（P1-4，B 未收录）。

## 四、修复批次规划与执行状态

- **R1 行为正确性**：P1-1、P1-2、P2-7、P2-8 —— ✅ 已闭环（1943/1937/0，先红后绿 6 用例）
- **R2 双真源与断链**：P1-4、P2-9/10/13、R1 遗留（暂停分支旧到期点）、QA BUG-1（memory_probe --size 口径）—— 🔄 执行中
- **R3 UI 接线波**：P1-3 编辑弹窗挂载 + actionJson 三字段 + 键名统一 + 删除/暂停入口 + P2-6 摘要键名 —— 待派（spec §4.2 欠账，约 1-2 天）
- **R4 文档销账**：TODO §B 刷新（✅ 主会话已做）、handoff §0/§11 刷新、P2-11 契约二选一、P2-12 边界登记、老库升级用例 —— 部分✅

## 五、测试有效性抽查

8 条关键用例（retry ladder / permanent 停用 / DST 春秋 / 唯一索引裸 SQL / 回拨不重放 / 开关落库 / wecom 凭据不进日志 / 编辑往返）均「删被测代码必红」。缺口：P1-1/P1-2 修复前无测试触达（R1 已补）。
