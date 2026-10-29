// 调度引擎实现（S4）。分层：只依赖 core（ticker/time/result）+ S3 ScheduleStore
// + S5 ChannelRegistry + S2 core::schedule 纯函数；不碰 platform/EUI-NEO。
#include "module_schedule/schedule_engine.h"

#include "core/logging.h"
#include "core/schedule_rule.h"  // S2：RuleKind/RuleSpec/parseRuleSpec/nextFire/missedFires/FireContext
#include "core/time.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <utility>

namespace module::schedule {

namespace {

// S2 对齐点：RuleKind 枚举名以 core/schedule_rule.h 实际定义为准（真实枚举无
// k 前缀：Once/Interval/Calendar/Cron/Event/Resident）；S2 未提供 kind 字符串
// 转换函数，本函数保留。
core::schedule::RuleKind kindFromString(const std::string& kind) {
    if (kind == "once") return core::schedule::RuleKind::Once;
    if (kind == "interval") return core::schedule::RuleKind::Interval;
    if (kind == "calendar") return core::schedule::RuleKind::Calendar;
    if (kind == "cron") return core::schedule::RuleKind::Cron;
    if (kind == "event") return core::schedule::RuleKind::Event;
    return core::schedule::RuleKind::Resident;
}

// 补跑窗口上限：all 策略最多补 100 次（§4.6），多取 1 个判溢出。
constexpr int kMissedCap = 100;
constexpr int kMissedProbeCap = 101;

std::string formatLocalTime(int64_t unixSec) {
    std::tm local{};
    core::time::localTime(static_cast<std::time_t>(unixSec), &local);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d",
                  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                  local.tm_hour, local.tm_min);
    return std::string(buf);
}

// 指定 plannedAt 的 run 是否已存在（唯一索引口径，§验收 6：重启不重复）。
bool runExists(ScheduleStore& store, int64_t ruleId, int64_t plannedAt) {
    auto found = store.findRun("user", "rule:" + std::to_string(ruleId), plannedAt);
    return found.isOk() && found.value().has_value();
}

// 插入一条无动作的错过/摘要记录（dedupe 守卫：已存在则跳过）。
void insertMissedRun(ScheduleStore& store, int64_t ruleId, int64_t plannedAt,
                     const std::string& summary) {
    if (runExists(store, ruleId, plannedAt)) return;
    RunRecord r;
    r.origin = "user";
    r.originKey = "rule:" + std::to_string(ruleId);
    r.ruleId = ruleId;
    r.plannedAt = plannedAt;
    r.status = "missed";
    r.resultSummary = summary;
    (void)store.insertRun(r);
}

// 过期 next_fire_at 缓存重算（禁用/暂停分支共用）：以 now 为锚算下一触发点
// （once 等已过期规则推成空），防重启用/解除暂停当 tick 把旧到期点当
// 「当前到期」补发。解析失败保持旧缓存（与原禁用分支行为一致）。
std::optional<int64_t> recomputeNextFrom(const RuleRecord& rule, int64_t nowUnix) {
    auto parsed = core::schedule::parseRuleSpec(kindFromString(rule.kind), rule.specJson);
    if (parsed.isFail()) return rule.nextFireAt;
    std::tm localNow{};
    core::time::localTime(static_cast<std::time_t>(nowUnix), &localNow);
    core::schedule::FireContext ctx;
    ctx.nowUnix = nowUnix;
    ctx.localNow = localNow;
    ctx.tzId = rule.tzId;
    ctx.lastEvalAt = nowUnix;
    auto next = core::schedule::nextFire(parsed.value(), nowUnix, ctx);
    return next.isOk() ? next.value() : std::nullopt;
}

} // namespace

ScheduleEngine::ScheduleEngine(core::data::SqliteDb& db, ChannelRegistry& channels)
    : store_(db), channels_(channels), tasks_(), runner_(store_, channels, tasks_),
      deliveryBucket_(kGlobalDeliveryCapacity, kGlobalDeliveryRefillPerSec) {}

ScheduleEngine::~ScheduleEngine() { stop(); }

void ScheduleEngine::resetForTest() {
    // 先注销 ticker 订阅：对已 start() 的引擎直接 handle_=0 会泄漏订阅
    // （悬挂 user 指针）。stop() 幂等，顺带清内存队列。
    stop();
    lastTickNow_ = 0;
    deliveryBucket_ =
        GlobalDeliveryBucket(kGlobalDeliveryCapacity, kGlobalDeliveryRefillPerSec);
    dueFires_.clear();
    evalMemo_.clear();
    parseErrorOnce_.clear();
    runner_.resetForTest();
    tasks_.resetForTest();
}

void ScheduleEngine::start() {
    if (handle_ != 0) {
        core::ticker::unsubscribe(handle_);
    }
    core::ticker::Subscription sub;
    sub.name = "schedule.engine";
    sub.fn = [](core::ticker::Now now, void* user) {
        static_cast<ScheduleEngine*>(user)->tick(now);
    };
    sub.user = this;
    sub.firstAt = 0;      // 立即
    sub.intervalSec = 1;  // 每秒（§2.4 唯一订阅）
    sub.expireAt = 0;     // 永不过期；一次性规则由引擎归档收尾，不用 expireAt
    handle_ = core::ticker::subscribe(sub);
}

void ScheduleEngine::stop() {
    if (handle_ != 0) {
        core::ticker::unsubscribe(handle_);
        handle_ = 0;
    }
    // 停机前边界：内存推进的 last_eval_at 落库（崩溃重启的重扫窗口缩到最小）。
    flushEvalMemo();
    // 清内存队列；落库的 pending/running 行保留（重启靠唯一索引不重复执行）。
    pendingFires_.clear();
    deliveryRetries_.clear();
    runningFires_.clear();
    runToRule_.clear();
    runningRuleIds_.clear();
    heavyRunning_ = false;
}

void ScheduleEngine::flushEvalMemo() {
    if (evalMemo_.empty()) return;
    auto rules = store_.listRules();
    if (rules.isFail()) {
        evalMemo_.clear();
        return;
    }
    for (auto& rule : rules.value()) {
        const auto it = evalMemo_.find(rule.id);
        if (it == evalMemo_.end() || rule.archivedAt.has_value() ||
            !rule.lastEvalAt.has_value() || it->second <= *rule.lastEvalAt) {
            continue;
        }
        rule.lastEvalAt = it->second;
        (void)store_.updateRule(rule);
    }
    evalMemo_.clear();
}

void ScheduleEngine::tick(int64_t nowUnix) {
    if (lastTickNow_ != 0 && nowUnix < lastTickNow_) {
        // 时钟回拨/NTP（§4.8）：只更新 last_eval_at，不重放；下次按新时间重算。
        rollbackLastEval(nowUnix);
        lastTickNow_ = nowUnix;
        channels_.flushAllToMainThread();
        return;
    }
    lastTickNow_ = nowUnix;

    drainDeliveryRetries(nowUnix);
    drainPendingFires(nowUnix);

    auto rules = store_.listRules();
    if (rules.isFail()) {
        LOG_ERROR("schedule.engine: 规则列表读取失败 " + rules.errorMessage());
        // 早退也必须 flush：本 tick 排进队列的全屏/toast 投递不能滞留。
        channels_.flushAllToMainThread();
        return;
    }
    dueFires_.clear();
    for (const auto& rule : rules.value()) {
        evaluateRule(rule, nowUnix);
    }
    // 同刻多条优先级：系统定时 > 用户提醒 > 静默动作，同级按 rule id（§4.7）。
    std::sort(dueFires_.begin(), dueFires_.end(),
              [](const DueFire& a, const DueFire& b) {
                  if (a.priority != b.priority) return a.priority > b.priority;
                  if (a.rule.id != b.rule.id) return a.rule.id < b.rule.id;
                  return a.dueAt < b.dueAt;
              });
    for (const auto& fire : dueFires_) {
        fireRule(fire.rule, fire.dueAt, fire.catchUp, nowUnix);
    }
    dueFires_.clear();
    // 入队型渠道（全屏/toast）落 UI 的唯一时机（registry 契约：ticker onTick 每tick）。
    channels_.flushAllToMainThread();
}

void ScheduleEngine::rollbackLastEval(int64_t nowUnix) {
    auto rules = store_.listRules();
    if (rules.isFail()) return;
    for (auto rule : rules.value()) {
        if (!rule.enabled || rule.archivedAt.has_value()) continue;
        if (rule.lastEvalAt == nowUnix) continue;
        rule.lastEvalAt = nowUnix;
        (void)store_.updateRule(rule);
        evalMemo_[rule.id] = nowUnix;  // 内存推进态一并回拨（取 max 口径下须显式压低）
    }
    LOG_WARN("schedule.engine: 时钟回拨，已刷新 last_eval_at 不重放");
}

int ScheduleEngine::firePriority(const RuleRecord& rule) {
    // 系统定时(3) > 用户提醒(2) > 静默动作(1)；系统层 S8 接入后由来源注入 3。
    if (rule.actionKind == "remind" || rule.actionKind == "remind_open") return 2;
    return 1;  // run_task / webhook_only 静默
}

int ScheduleEngine::globalDeliveryUnits(const RuleRecord& rule) const {
    // 与 ActionRunner::run 的投递分支同口径：三类提醒动作走渠道投递。
    if (rule.actionKind != "remind" && rule.actionKind != "remind_open" &&
        rule.actionKind != "webhook_only") {
        return 0;
    }
    if (rule.channelIds.empty()) return 0;
    return static_cast<int>(
        std::min<std::size_t>(rule.channelIds.size(),
                              static_cast<std::size_t>(kGlobalDeliveryCapacity)));
}

void ScheduleEngine::evaluateRule(const RuleRecord& rule, int64_t nowUnix) {
    // 归档：终态（§4.4），永不复启，不推进。
    if (rule.archivedAt.has_value()) return;
    // 停用：不参与调度（§4.4）；last_eval_at 与暂停同口径逐 tick 推进，重启用后
    // 从 now 干净续跑、不补发禁用期间的陈旧动作；过期的 next_fire_at 缓存一并
    // 推走，防重启用当 tick 把旧到期点当「当前到期」补发（once 规则过期点推成空）。
    if (!rule.enabled) {
        RuleRecord r = rule;
        r.lastEvalAt = nowUnix;
        if (r.nextFireAt.has_value() && *r.nextFireAt <= nowUnix) {
            r.nextFireAt = recomputeNextFrom(rule, nowUnix);
        }
        (void)store_.updateRule(r);
        return;
    }
    // 暂停中：期间到期点不补（§4.4）；只推进 last_eval_at，解除后从 now 干净续跑。
    // 与禁用分支同款：过期 next_fire_at 缓存一并推走，防解除暂停当 tick 把
    // 暂停前的旧到期点当「当前到期」补发。
    if (rule.pausedUntil.has_value() && *rule.pausedUntil > nowUnix) {
        RuleRecord r = rule;
        r.lastEvalAt = nowUnix;
        if (r.nextFireAt.has_value() && *r.nextFireAt <= nowUnix) {
            r.nextFireAt = recomputeNextFrom(rule, nowUnix);
        }
        (void)store_.updateRule(r);
        return;
    }
    // event/resident：无下次时刻，引擎不驱动（v1 由外部入队）。
    if (rule.kind == "event" || rule.kind == "resident") return;

    auto parsed = core::schedule::parseRuleSpec(kindFromString(rule.kind), rule.specJson);
    if (parsed.isFail()) {
        if (parseErrorOnce_.insert(rule.id).second) {
            LOG_ERROR("schedule.engine: 规则 #" + std::to_string(rule.id) +
                      " spec 解析失败：" + parsed.errorMessage());
        }
        return;
    }
    const core::schedule::RuleSpec& spec = parsed.value();

    std::tm localNow{};
    core::time::localTime(static_cast<std::time_t>(nowUnix), &localNow);
    core::schedule::FireContext ctx;
    ctx.nowUnix = nowUnix;
    ctx.localNow = localNow;
    ctx.tzId = rule.tzId;
    ctx.lastEvalAt = rule.lastEvalAt.value_or(nowUnix);

    RuleRecord r = rule;
    if (!rule.lastEvalAt.has_value()) {
        // 冷启动（last_eval_at 为空，首次评估）：不补历史；next_fire_at 缺失时初始化。
        // 判定必须看「字段是否为空」而非数值 ≤0——上方 value_or(nowUnix) 已把空值
        // 替换成 now，数值判定在正常时刻恒为假，next_fire_at 永远初始化不出来。
        r.lastEvalAt = nowUnix;
        if (!r.nextFireAt.has_value()) {
            auto next = core::schedule::nextFire(spec, nowUnix, ctx);
            r.nextFireAt = next.isOk() ? next.value() : std::nullopt;
        }
        (void)store_.updateRule(r);
        evalMemo_[rule.id] = nowUnix;
        return;
    }
    // 有效 last_eval_at = max(行值, 内存推进值)：内存持有边界落库之间的推进态，
    // 行值可能被外部编辑（recomputeNextFire）刷得更大——取大者两边都不丢。
    int64_t lastEval = *rule.lastEvalAt;
    const auto memoIt = evalMemo_.find(rule.id);
    if (memoIt != evalMemo_.end() && memoIt->second > lastEval) lastEval = memoIt->second;
    bool cacheRecomputed = false;  // 本 tick 是否重算了 next_fire_at 缓存（落库边界）

    if (nowUnix > lastEval) {
        const std::string& policy = rule.missedPolicy;
        // 补跑窗口 = (last_eval_at, now) 开区间（§4.6）：下界排除 last_eval_at——
        // 该时刻已在上轮评估过，收进窗口会把已触发点重新当成错过（once 的 cap=1
        // 截断后真正的新错过点永远补不到）；上界排除 now——此刻的点属「当前到期」，
        // 由下方 next_fire_at 缓存正常触发，不算错过（停机跨 3 个到期点 = 恰 3 条
        // missed，now 时刻不额外记一条）。
        if (nowUnix - lastEval >= 2) {
            const int64_t windowFrom = lastEval + 1;
            const int64_t windowTo = nowUnix - 1;
            if (policy == "skip") {
                // 不补：逐点写 missed 运行记录（§4.6）；窗口过大只记前 100 + 摘要。
                auto missed =
                    core::schedule::missedFires(spec, windowFrom, windowTo, kMissedProbeCap, ctx);
                if (missed.isOk()) {
                    const auto& points = missed.value();
                    const std::size_t count = std::min(points.size(), std::size_t(kMissedCap));
                    for (std::size_t i = 0; i < count; ++i) {
                        insertMissedRun(store_, rule.id, points[i], "停机错过，不补跑");
                    }
                    if (points.size() > std::size_t(kMissedCap)) {
                        insertMissedRun(store_, rule.id, points[kMissedCap],
                                        "停机窗口过大，仅记前 100 个错过点");
                    }
                }
            } else if (policy == "once") {
                // 补一次：只取最早一个错过点，run 摘要标注原定时刻（§4.6）。
                auto missed = core::schedule::missedFires(spec, windowFrom, windowTo, 1, ctx);
                if (missed.isOk() && !missed.value().empty()) {
                    pushDueFire(rule, missed.value().front(), true);
                }
            } else {
                // all：最多补 100 次，超出合并为一条「遗漏超过 100 次」摘要（§4.6）。
                auto missed =
                    core::schedule::missedFires(spec, windowFrom, windowTo, kMissedProbeCap, ctx);
                if (missed.isOk()) {
                    const auto& points = missed.value();
                    const std::size_t count = std::min(points.size(), std::size_t(kMissedCap));
                    for (std::size_t i = 0; i < count; ++i) {
                        pushDueFire(rule, points[i], true);
                    }
                    if (points.size() > std::size_t(kMissedCap)) {
                        const int64_t overflowAt = points[kMissedCap];
                        insertMissedRun(store_, rule.id, overflowAt,
                                        "遗漏超过 100 次（已合并摘要）");
                        Delivery alert;
                        alert.title = rule.name;
                        alert.body = "「" + rule.name + "」停机期间遗漏超过 100 次，"
                                     "已按上限补跑并合并为摘要";
                        alert.level = 2;
                        alert.runId = 0;
                        alert.dedupeKey =
                            "rule:" + std::to_string(rule.id) + ":overflow:" +
                            std::to_string(overflowAt);
                        (void)runner_.localAlert(alert);
                    }
                }
            }
        }
        // 当前到期：next_fire_at 缓存 ≤ now → 正常触发（非补跑）。与补跑点重合时
        // pushDueFire 去重并保留补跑标注，fireRule 内 runExists 再兜底防重。
        if (r.nextFireAt.has_value() && *r.nextFireAt <= nowUnix) {
            pushDueFire(rule, *r.nextFireAt, false);
        }
        // 触发点收集完，next_fire_at 若仍停在 now 之前则推到未来
        //（finishFire 会再以 now 重算，此处防 skip 路径遗留旧缓存卡死调度）。
        if (r.nextFireAt.has_value() && *r.nextFireAt <= nowUnix) {
            auto next = core::schedule::nextFire(spec, nowUnix, ctx);
            r.nextFireAt = next.isOk() ? next.value() : std::nullopt;
            cacheRecomputed = true;
        }
    } else if (r.nextFireAt.has_value() && *r.nextFireAt <= nowUnix) {
        // 窗口为空但缓存到期（missedFires 边界差异兜底）：正常触发。
        pushDueFire(rule, *r.nextFireAt, false);
    }

    // last_eval_at 内存推进（每 tick）；整行落库只在本 tick 重算了 next_fire_at
    // 缓存（触发收集 tick）时发生，防每秒整行写放大（updated_at 不再被刷）。
    // 崩溃重启安全：重启后按落库边界重扫 (边界, now) 补跑窗口，窗口内已处理点
    // 由 (origin, origin_key, planned_at) 唯一索引经 runExists/insertMissedRun
    // 硬挡——不重复执行、已处理点不漏记。
    evalMemo_[rule.id] = nowUnix;
    if (cacheRecomputed) {
        r.lastEvalAt = nowUnix;
        (void)store_.updateRule(r);
    }
}

// 收集一条到期触发：同规则同 dueAt 只保留一条（§4.7 同计划时刻只一条 run）；
// 补跑标注优先保留——补跑点与缓存到期点重合时，run 摘要须标注原定时刻（§4.6）。
void ScheduleEngine::pushDueFire(const RuleRecord& rule, int64_t dueAt, bool catchUp) {
    for (auto& existing : dueFires_) {
        if (existing.rule.id == rule.id && existing.dueAt == dueAt) {
            if (catchUp) existing.catchUp = true;
            return;
        }
    }
    DueFire f;
    f.rule = rule;
    f.dueAt = dueAt;
    f.catchUp = catchUp;
    f.priority = firePriority(rule);
    dueFires_.push_back(std::move(f));
}

void ScheduleEngine::fireRule(const RuleRecord& rule, int64_t dueAt, bool catchUp,
                              int64_t nowUnix) {
    if (runExists(store_, rule.id, dueAt)) return;  // 唯一索引硬挡重复（§验收 6）

    // 全局投递桶（调研 §10.5）：带渠道投递的触发先过全局一桶，无令牌按并发冲突
    // 同路径排队（pending 行留痕），令牌随 tick 回填后在 drainPendingFires 放行。
    const int units = globalDeliveryUnits(rule);
    const bool blocked = runningRuleIds_.count(rule.id) != 0 ||
                         (rule.actionKind == "run_task" && heavyRunning_) ||
                         (units > 0 && !deliveryBucket_.tryAcquire(nowUnix * 1000, units));
    if (blocked) {
        // 队列满：skip 并记 suppressed，不静默丢弃（§4.7）。
        if (pendingFires_.size() >= kMaxPendingFires) {
            RunRecord r;
            r.origin = "user";
            r.originKey = "rule:" + std::to_string(rule.id);
            r.ruleId = rule.id;
            r.plannedAt = dueAt;
            r.status = "suppressed";
            r.resultSummary = "待执行队列已满，跳过本次触发";
            (void)store_.insertRun(r);
            LOG_WARN("schedule.engine: 待执行队列满，规则 #" + std::to_string(rule.id) +
                     " 触发被抑制");
            return;
        }
        RunRecord r;
        r.origin = "user";
        r.originKey = "rule:" + std::to_string(rule.id);
        r.ruleId = rule.id;
        r.plannedAt = dueAt;
        r.status = "pending";
        auto inserted = store_.insertRun(r);
        if (inserted.isFail()) {
            LOG_ERROR("schedule.engine: 排队 run 落库失败 " + inserted.errorMessage());
            return;
        }
        PendingFire f;
        f.rule = rule;
        f.plannedAt = dueAt;
        f.catchUp = catchUp;
        f.run = inserted.value();
        pendingFires_.push_back(std::move(f));
        return;
    }

    RunRecord r;
    r.origin = "user";
    r.originKey = "rule:" + std::to_string(rule.id);
    r.ruleId = rule.id;
    r.plannedAt = dueAt;
    r.status = "running";
    r.startedAt = nowUnix;
    r.attempt = 1;
    auto inserted = store_.insertRun(r);
    if (inserted.isFail()) {
        LOG_ERROR("schedule.engine: run 落库失败 " + inserted.errorMessage());
        return;
    }
    PendingFire f;
    f.rule = rule;
    f.plannedAt = dueAt;
    f.catchUp = catchUp;
    f.run = inserted.value();
    executeFire(std::move(f), nowUnix);
}

void ScheduleEngine::executeFire(PendingFire fire, int64_t nowUnix) {
    const int64_t ruleId = fire.rule.id;
    runningRuleIds_.insert(ruleId);
    runToRule_[fire.run.id] = ruleId;
    runningFires_[fire.run.id] = fire;
    if (fire.rule.actionKind == "run_task") heavyRunning_ = true;

    RunContext ctx;
    ctx.runId = fire.run.id;
    ctx.plannedAt = fire.plannedAt;
    ctx.nowUnix = nowUnix;
    ctx.originKey = "rule:" + std::to_string(ruleId);
    ctx.actionKind = fire.rule.actionKind;
    ctx.actionJson = fire.rule.actionJson;
    ctx.ruleName = fire.rule.name;
    ctx.channelIds = fire.rule.channelIds;

    auto outcome = runner_.run(ctx);
    if (outcome.isFail()) {
        RunOutcome failed;
        failed.status = "failed";
        failed.error = outcome.errorMessage();
        finalizeRun(fire, nowUnix, failed);
        return;
    }
    // 渠道投递的可重试结局入引擎重试队列（§4.9 阶梯 0s/30s/300s 靠后续 tick 推进）：
    // 漏入队 = 第 1 次尝试后重试静默丢失，schedule_delivery 只剩 1 行、无终态。
    for (auto& retry : outcome.value().retries) {
        deliveryRetries_.push_back(std::move(retry));
    }
    if (outcome.value().status == "async") {
        // 异步任务：保持 running 占用，收尾走 completeRun（§4.7 同规则不并发）。
        return;
    }
    finalizeRun(fire, nowUnix, outcome.value());
}

void ScheduleEngine::finalizeRun(const PendingFire& fire, int64_t nowUnix,
                                 const RunOutcome& outcome) {
    auto rit = runningFires_.find(fire.run.id);
    if (rit == runningFires_.end()) return;
    PendingFire f = rit->second;

    RunRecord r = f.run;
    r.status = (outcome.status == "success") ? "success" : "failed";
    r.endedAt = nowUnix;
    if (r.startedAt.has_value()) r.durationMs = nowUnix - *r.startedAt;
    r.resultSummary = outcome.summary;
    if (f.catchUp) {
        // 补跑标注原定时刻（§4.6）。
        r.resultSummary = "补跑（原定 " + formatLocalTime(f.plannedAt) + "）" +
                          (r.resultSummary.empty() ? "" : "；" + r.resultSummary);
    }
    r.error = outcome.error;
    (void)store_.updateRun(r);

    runningFires_.erase(rit);
    runToRule_.erase(f.run.id);
    if (f.rule.actionKind == "run_task") heavyRunning_ = false;
    runningRuleIds_.erase(f.rule.id);

    finishFire(f.rule.id, f.plannedAt, f.rule.kind == "once", nowUnix);
}

core::Result<void> ScheduleEngine::completeRun(int64_t runId, const RunOutcome& outcome,
                                               int64_t nowUnix) {
    const auto it = runningFires_.find(runId);
    if (it == runningFires_.end()) {
        return core::Result<void>::fail(core::ErrorCode::NotFound,
                                        "schedule.engine: 未知在飞 runId " +
                                            std::to_string(runId));
    }
    finalizeRun(it->second, nowUnix, outcome);
    return core::Result<void>::ok();
}

void ScheduleEngine::finishFire(int64_t ruleId, int64_t firedAt, bool onceKind,
                                int64_t nowUnix) {
    auto got = store_.getRule(ruleId);
    if (got.isFail()) return;
    RuleRecord rule = got.value();

    // 重算下次：anchor 用 now（不是 firedAt）——补跑场景下 firedAt 是历史时刻，
    // nextFire(firedAt) 可能仍 ≤ now，会造成逐 tick 续补（违反 §4.6 once 只补一次）。
    auto parsed = core::schedule::parseRuleSpec(kindFromString(rule.kind), rule.specJson);
    if (parsed.isOk()) {
        std::tm localNow{};
        core::time::localTime(static_cast<std::time_t>(nowUnix), &localNow);
        core::schedule::FireContext ctx;
        ctx.nowUnix = nowUnix;
        ctx.localNow = localNow;
        ctx.tzId = rule.tzId;
        ctx.lastEvalAt = nowUnix;
        auto next = core::schedule::nextFire(parsed.value(), nowUnix, ctx);
        rule.nextFireAt = next.isOk() ? next.value() : std::nullopt;
        if (onceKind) rule.nextFireAt = std::nullopt;  // 一次性：无下次
    } else if (parseErrorOnce_.insert(ruleId).second) {
        LOG_ERROR("schedule.engine: 规则 #" + std::to_string(ruleId) +
                  " 重算失败：" + parsed.errorMessage());
    }
    rule.lastFireAt = firedAt;
    rule.runCount += 1;
    rule.lastEvalAt = nowUnix;  // 触发边界：last_eval_at 随收尾落库
    (void)store_.updateRule(rule);

    if (onceKind) {
        // 一次性规则触发后归档停用（§4.4/验收 4）：不删除，重启不再触发。
        (void)store_.archiveRule(ruleId);
    }
}

core::Result<void> ScheduleEngine::recomputeNextFire(int64_t ruleId, int64_t nowUnix) {
    auto got = store_.getRule(ruleId);
    if (got.isFail()) return core::Result<void>::fail(got.errorCode(), got.errorMessage());
    RuleRecord rule = got.value();
    auto parsed = core::schedule::parseRuleSpec(kindFromString(rule.kind), rule.specJson);
    if (parsed.isFail()) {
        return core::Result<void>::fail(parsed.errorCode(), parsed.errorMessage());
    }
    std::tm localNow{};
    core::time::localTime(static_cast<std::time_t>(nowUnix), &localNow);
    core::schedule::FireContext ctx;
    ctx.nowUnix = nowUnix;
    ctx.localNow = localNow;
    ctx.tzId = rule.tzId;
    ctx.lastEvalAt = nowUnix;
    auto next = core::schedule::nextFire(parsed.value(), nowUnix, ctx);
    rule.nextFireAt = next.isOk() ? next.value() : std::nullopt;
    // 编辑事务语义（§4.4）：last_eval_at 同步刷新，防历史窗口放大成补跑。
    rule.lastEvalAt = nowUnix;
    return store_.updateRule(rule);
}

void ScheduleEngine::drainPendingFires(int64_t nowUnix) {
    while (!pendingFires_.empty()) {
        PendingFire f = pendingFires_.front();
        const int units = globalDeliveryUnits(f.rule);
        const bool blocked = runningRuleIds_.count(f.rule.id) != 0 ||
                             (f.rule.actionKind == "run_task" && heavyRunning_) ||
                             (units > 0 && !deliveryBucket_.tryAcquire(nowUnix * 1000, units));
        if (blocked) break;
        pendingFires_.pop_front();
        // 出队复核：入队到放行之间（桶等待/同规则串行）规则可能已被禁用、归档或
        // 删除——丢弃该触发，不执行、不留新 run 行（队列 pending 行留原状）。
        auto live = store_.getRule(f.rule.id);
        if (live.isFail() || !live.value().enabled || live.value().archivedAt.has_value()) {
            LOG_INFO("schedule.engine: 规则 #" + std::to_string(f.rule.id) +
                     " 已停用/删除，丢弃排队触发（planned=" + std::to_string(f.plannedAt) + "）");
            continue;
        }
        f.rule = live.value();  // 执行用最新行：排队期间的编辑/启停在此生效
        // pending → running 状态推进。
        RunRecord r = f.run;
        r.status = "running";
        r.startedAt = nowUnix;
        (void)store_.updateRun(r);
        f.run = r;
        executeFire(std::move(f), nowUnix);
    }
}

void ScheduleEngine::drainDeliveryRetries(int64_t nowUnix) {
    while (!deliveryRetries_.empty()) {
        DeliveryRetry retry = deliveryRetries_.front();
        if (retry.nextRetryAt > nowUnix) break;
        deliveryRetries_.pop_front();
        auto attempt = runner_.attemptDelivery(retry, nowUnix);
        if (attempt.isFail()) {
            LOG_ERROR("schedule.engine: 投递重试失败 " + attempt.errorMessage());
            continue;
        }
        const RetryOutcome& out = attempt.value();
        if (!out.terminal && out.nextRetryAt.has_value()) {
            // slotBusy（渠道并发满）不记账也不计次：本次尝试没发生，虚耗配额会把
            // 真实投递机会耗光（attempt 被顶到上限后渠道恢复也无法再投）。
            if (!out.retryWithoutCharge) retry.attempt += 1;
            retry.nextRetryAt = *out.nextRetryAt;
            deliveryRetries_.push_back(std::move(retry));
        }
        if (out.channelDisabled) {
            // 永久失败：停用渠道 + 本地告警一次（§4.9）。
            auto ch = store_.getChannel(retry.channelId);
            if (ch.isOk() && ch.value().has_value()) {
                ChannelRecord record = *ch.value();
                record.enabled = false;
                (void)store_.updateChannel(record);
            }
            Delivery alert = retry.delivery;
            alert.title = "调度渠道已停用";
            alert.body = "渠道投递永久失败已停用：" + out.error;
            (void)runner_.localAlert(alert);
        }
        if (out.degradedLocally) {
            // 最终失败：降级本地提示恰好一次（§4.9），不每次重试都弹。
            Delivery alert = retry.delivery;
            alert.title = "提醒投递失败";
            alert.body = "「" + retry.delivery.title + "」连续投递失败，请检查渠道配置";
            (void)runner_.localAlert(alert);
        }
    }
}

} // namespace module::schedule
