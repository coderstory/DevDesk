// 调度引擎（S4）：全局唯一 ticker 订阅 {name="schedule.engine", firstAt=0,
// intervalSec=1, expireAt=0}，引擎内部算到期（反重构警告 §2.4：禁逐规则重注册，
// 一次性规则不能用 expireAt 收尾——由引擎触发后归档）。
//
// 每 tick 顺序（§4.6/§4.7/§4.8）：
//   1) 时钟回拨（now < 上次 tick）：只把各活跃规则 last_eval_at 刷成 now，不重放；
//   2) 投递重试队列推进（渠道 Receipt 分类消费，阶梯 0s/30s/300s）；
//   3) 待执行队列推进（同规则串行 / 重动作全局并发 1 / 全局投递桶的等待队列，
//      出队前复核规则行，已禁用/删除则丢弃）；
//   4) 逐规则评估：错过补跑（last_eval_at 口径，once/skip/all）+ 到期触发
//      （next_fire_at 到期扫描），每规则每 tick 至多一个新触发；禁用/暂停规则
//      仅推进 last_eval_at（停用/暂停期间不补）；
//   5) 渠道 flush（§channel_registry 契约）：入队型渠道（全屏/toast）落 UI 的
//      唯一时机，引擎 ticker onTick 每 tick 调用 flushAllToMainThread（含
//      规则列表读取失败的 tick，防入队投递滞留）。
//
// 补跑口径（§4.6）：补跑扫描 = (last_eval_at, now) 开区间；now 时刻的点由
// next_fire_at 缓存正常触发，不算错过。冷启动（last_eval_at 为空）不补
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
// 必达（§验收 6 的口径）。停机恢复瞬间恰逢到期点时该点静默丢失（补跑窗口
// 上界排除 now + 缓存过期），all 策略丢一次，once/skip 无实损——后续可把
// 「当前到期」改为实算。
//
// 已知 v1 边界（有意不修，登记自审计 P2-12）：①skipped 运行状态全链无人写
// （补跑窗口跳过只记日志）；②崩溃遗留 status=running 的行永悬（无看门狗改判）；
// ③schedule_run/schedule_delivery 的 90 天历史保留无清理任务（库只增不减）；
// ④max_runs 字段解析与落库双不消费（写进记录但引擎不按它截停规则）。
#pragma once

#include "core/result.h"
#include "core/sqlite_db.h"
#include "core/ticker.h"
#include "module_schedule/action_runner.h"
#include "module_schedule/channel_registry.h"  // S5：ChannelRegistry
#include "module_schedule/schedule_store.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace module::schedule {

// 全局投递令牌桶（调研 §10.5 上限口径：目标 18 条/分钟，官方 20 留 2 条余量）：
// 一桶计全渠道外发总量，防多 schedule_channel 行叠加超频（多行指向同一机器人时
// 渠道级桶各自为政挡不住合并流量）；渠道级桶（wecom/os 内部）在其后串联。
// 非线程安全：引擎主线程专用（ticker 契约）。
class GlobalDeliveryBucket {
public:
    GlobalDeliveryBucket(int capacity, double refillPerSec)
        : capacity_(capacity), refillPerSec_(refillPerSec), tokens_(capacity) {}

    // 取 n 个令牌，全有或全无（不足不消费）。nowMs 单调毫秒；回拨不回填不重置。
    bool tryAcquire(std::int64_t nowMs, int n) {
        if (n <= 0) return true;
        if (lastMs_ != 0 && nowMs > lastMs_) {
            tokens_ = std::min(static_cast<double>(capacity_),
                               tokens_ + (nowMs - lastMs_) / 1000.0 * refillPerSec_);
        }
        lastMs_ = std::max(lastMs_, nowMs);
        if (tokens_ < static_cast<double>(n)) return false;
        tokens_ -= static_cast<double>(n);
        return true;
    }

private:
    int capacity_;
    double refillPerSec_;
    double tokens_;            // 含小数（浮点回填）
    std::int64_t lastMs_ = 0;  // 0 = 未初始化（首次只记时不回填，桶满起算）
};

class ScheduleEngine {
public:
    ScheduleEngine(core::data::SqliteDb& db, ChannelRegistry& channels);
    // 兜底注销 ticker：持有方漏调 stop() 时防 ticker 里悬挂 user 指针（UAF）。
    ~ScheduleEngine();

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
    // 全局投递桶上限（调研 §10.5：目标 18 条/分钟，官方 20 留 2 条余量）。
    static constexpr int kGlobalDeliveryCapacity = 18;
    static constexpr double kGlobalDeliveryRefillPerSec = 18.0 / 60.0;

private:
    // 一次待执行触发（已在 schedule_run 落 pending/running 行）。
    struct PendingFire {
        RuleRecord rule;        // 入队时的快照；drain 出队时重读最新行复核
                                // （禁用/删除/归档丢弃，编辑生效），直通路径即本 tick 列表行
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
    // 收集一条到期触发：同规则同 dueAt 去重，补跑标注优先（§4.6/§4.7）。
    void pushDueFire(const RuleRecord& rule, int64_t dueAt, bool catchUp);
    // 对 dueAt 时刻发起一次触发：并发检查通过就执行，否则排队/suppressed。
    void fireRule(const RuleRecord& rule, int64_t dueAt, bool catchUp, int64_t nowUnix);
    // 执行一个已就绪的触发（run 行已插入）：run 状态机 + 动作 + 重算下次。
    void executeFire(PendingFire fire, int64_t nowUnix);
    // run 终态回写 + 释放串行占用 + 重算下次/归档（executeFire 与 completeRun 共用）。
    void finalizeRun(const PendingFire& fire, int64_t nowUnix, const RunOutcome& outcome);
    // 触发后收尾：next_fire_at 重算（anchor=now，防补跑续触发）、last_eval_at/
    // last_fire_at/run_count 推进、一次性归档。
    void finishFire(int64_t ruleId, int64_t firedAt, bool onceKind, int64_t nowUnix);
    void drainPendingFires(int64_t nowUnix);
    void drainDeliveryRetries(int64_t nowUnix);
    // 时钟回拨（§4.8）：只把活跃规则 last_eval_at 刷成 now，不重放。
    void rollbackLastEval(int64_t nowUnix);
    // 停机前边界：把内存推进的 last_eval_at 落库（缩小重启补跑重扫窗口）并清空。
    void flushEvalMemo();
    static int firePriority(const RuleRecord& rule);
    // 本次触发占用的全局投递桶令牌数：带渠道投递的动作按目标渠道数计
    // （一条外发一个令牌），run_task / 无渠道动作不占桶；超出容量截断
    // （多渠道规则不得因令牌永远凑不齐卡死待执行队列）。
    int globalDeliveryUnits(const RuleRecord& rule) const;

    ScheduleStore store_;
    ChannelRegistry& channels_;    // flushAllToMainThread 每 tick 泵（registry 契约）
    InternalTaskRegistry tasks_;   // v1 无生产注册来源（run_task 未注册 key → invoke fail），待 S8 经 taskRegistry() 注册
    ActionRunner runner_;
    core::ticker::Handle handle_ = 0;
    GlobalDeliveryBucket deliveryBucket_;
    int64_t lastTickNow_ = 0;                 // 上次 tick 时刻（回拨判定）
    std::set<int64_t> runningRuleIds_;        // 同规则不并发（内存态，§4.7）
    std::map<int64_t, int64_t> runToRule_;    // runId → ruleId（completeRun 路由）
    std::map<int64_t, PendingFire> runningFires_;  // runId → 在飞触发（终态回写源）
    std::deque<PendingFire> pendingFires_;
    std::deque<DeliveryRetry> deliveryRetries_;
    std::vector<DueFire> dueFires_;           // 本轮收集，统一排序后执行
    bool heavyRunning_ = false;
    std::set<int64_t> parseErrorOnce_;        // 规则解析失败只报一次日志
    // 活跃规则 last_eval_at 的内存推进态（ruleId → 已评估到的时刻）。活跃规则
    // 只在边界（冷启动/触发/缓存重算/回拨/停机）落库，防每秒整行写放大；崩溃
    // 重启按落库边界重扫补跑窗口，已处理点由 run 唯一索引硬挡不重不漏。
    std::map<int64_t, int64_t> evalMemo_;
};

} // namespace module::schedule
