// 调度引擎（S4）：全局唯一 ticker 订阅 {name="schedule.engine", firstAt=0,
// intervalSec=1, expireAt=0}，引擎内部算到期（反重构警告 §2.4：禁逐规则重注册，
// 一次性规则不能用 expireAt 收尾——由引擎触发后归档）。
//
// 每 tick 顺序（§4.6/§4.7/§4.8）：
//   1) 时钟回拨（now < 上次 tick）：只把各活跃规则 last_eval_at 刷成 now，不重放；
//   2) 投递重试队列推进（渠道 Receipt 分类消费，阶梯 0s/30s/300s）；
//   3) 待执行队列推进（同规则串行 / 重动作全局并发 1 的等待队列）；
//   4) 逐规则评估：错过补跑（last_eval_at 口径，once/skip/all）+ 到期触发
//      （next_fire_at 到期扫描），每规则每 tick 至多一个新触发，事务内重算下次。
//
// 补跑口径（§4.6）：窗口 = (last_eval_at, now]；冷启动（last_eval_at 为空）不补
// 历史；skip 逐点写 missed 运行记录不动作；once 只补最早一个，run 摘要标注
// 「补跑（原定 <时刻>）」；all 上限 100 次，超出合并为一条「遗漏超过 100 次」
// 摘要（一条 missed 记录 + 一次本地提醒）。dedupe 由 (origin, origin_key,
// planned_at) 唯一索引硬挡，重启不重复补（§验收 6）。
//
// 并发契约：主线程专用（ticker 线程契约），无锁；start() 由 module_schedule
// 宿主 onStart 调、stop() 由 onStop 调；tick() 公开供测试注入假时钟。
//
// 已知 v1 边界（有意不修）：应用在「排队中（pending）」的运行记录落库后崩溃，
// 该记录重启后不会被重放（窗口已随 last_eval_at 越过）——不重复执行优先于
// 必达（§验收 6 的口径）。
#pragma once

#include "core/result.h"
#include "core/sqlite_db.h"
#include "core/ticker.h"
#include "module_schedule/action_runner.h"
#include "module_schedule/channel_registry.h"  // S5：ChannelRegistry
#include "module_schedule/schedule_store.h"

#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace module::schedule {

class ScheduleEngine {
public:
    ScheduleEngine(core::data::SqliteDb& db, ChannelRegistry& channels);

    // onStart：订阅唯一 ticker 泵（§2.4）。幂等：重复 start 先注销旧订阅。
    void start();
    // onStop：注销订阅并清空内存队列（落库记录保留）。
    void stop();

    // 每秒推进：到期扫描 → 补跑判定 → 触发/排队 → 重试推进。公开供测试注入假时钟。
    void tick(int64_t nowUnix);

    // 编辑后重算（§4.4）：事务内重算 next_fire_at 并刷新 last_eval_at = now
    // （防编辑把历史窗口放大成补跑）。
    core::Result<void> recomputeNextFire(int64_t ruleId, int64_t nowUnix);

    // 异步 run_task 收尾：任务方完成后回调；推进 run 行终态并释放串行占用、
    // 重算下次（正在跑的 run 不因编辑打断——编辑只动 next_fire_at）。
    core::Result<void> completeRun(int64_t runId, const RunOutcome& outcome,
                                   int64_t nowUnix);

    // 测试缝：清空内存态（ticker 订阅由 stop() 注销；测试直接用 tick 驱动）。
    void resetForTest();

    // S7 装配缝：app_assembly 在 onStart 前经此注册内部任务回调（agent:<id> 等）。
    InternalTaskRegistry& taskRegistry() { return tasks_; }

    // 待执行队列上限（§4.7 队列满 skip 记 suppressed，不静默丢弃）。
    static constexpr std::size_t kMaxPendingFires = 128;
    // 同一规则同 tick 最多新触发 1 个（1Hz 泵下每秒自然摊平；突发补跑走 missedFires）。
    static constexpr int kMaxFiresPerRulePerTick = 1;

private:
    // 一次待执行触发（已在 schedule_run 落 pending/running 行）。
    struct PendingFire {
        RuleRecord rule;        // 入队时的快照（执行/重算前会重读最新行）
        int64_t plannedAt = 0;
        bool catchUp = false;   // 补跑触发：run 摘要标注原定时刻
        RunRecord run;          // 已插入的 run 行（含 id）
    };

    // 本轮评估收集到的到期触发（执行前统一排序：§4.7 同刻多条优先级
    // 系统定时(3) > 用户提醒(2) > 静默动作(1)，同级按 rule id）。
    struct DueFire {
        RuleRecord rule;
        int64_t dueAt = 0;
        bool catchUp = false;
        int priority = 0;
    };

    // 评估单条规则：补跑窗口 + 到期触发 + last_eval_at 推进。
    void evaluateRule(const RuleRecord& rule, int64_t nowUnix);
    // 对 dueAt 时刻发起一次触发：并发检查通过就执行，否则排队/suppressed。
    void fireRule(const RuleRecord& rule, int64_t dueAt, bool catchUp, int64_t nowUnix);
    // 执行一个已就绪的触发（run 行已插入）：run 状态机 + 动作 + 重算下次。
    void executeFire(PendingFire fire, int64_t nowUnix);
    // run 终态回写 + 释放串行占用 + 重算下次/归档（executeFire 与 completeRun 共用）。
    void finalizeRun(const PendingFire& fire, int64_t nowUnix, const RunOutcome& outcome);
    // 触发后收尾：next_fire_at 重算（anchor=now，防补跑续触发）、last_fire_at/
    // run_count 推进、一次性归档。
    void finishFire(int64_t ruleId, int64_t firedAt, bool onceKind, int64_t nowUnix);
    void drainPendingFires(int64_t nowUnix);
    void drainDeliveryRetries(int64_t nowUnix);
    // 时钟回拨（§4.8）：只把活跃规则 last_eval_at 刷成 now，不重放。
    void rollbackLastEval(int64_t nowUnix);
    static int firePriority(const RuleRecord& rule);

    ScheduleStore store_;
    InternalTaskRegistry tasks_;   // app_assembly（S7）经引擎暴露的注册缝装配
    ActionRunner runner_;
    ChannelRegistry& channels_;
    core::ticker::Handle handle_ = 0;
    int64_t lastTickNow_ = 0;                 // 上次 tick 时刻（回拨判定）
    std::set<int64_t> runningRuleIds_;        // 同规则不并发（内存态，§4.7）
    std::map<int64_t, int64_t> runToRule_;    // runId → ruleId（completeRun 路由）
    std::map<int64_t, PendingFire> runningFires_;  // runId → 在飞触发（终态回写源）
    std::deque<PendingFire> pendingFires_;
    std::deque<DeliveryRetry> deliveryRetries_;
    std::vector<DueFire> dueFires_;           // 本轮收集，统一排序后执行
    bool heavyRunning_ = false;
    std::set<int64_t> parseErrorOnce_;        // 规则解析失败只报一次日志
};

} // namespace module::schedule
