// 调度引擎实现（S4）。分层：只依赖 core（ticker/time/result）+ S3 ScheduleStore
// + S5 ChannelRegistry + S2 core::schedule 纯函数；不碰 platform/EUI-NEO。
#include "module_schedule/schedule_engine.h"

#include "core/logging.h"
#include "core/schedule_rule.h"  // S2：RuleKind/RuleSpec/parseRuleSpec/nextFire/missedFires/FireContext
#include "core/time.h"

#include <algorithm>
#include <cstdio>
#include <ctime>

namespace module::schedule {

namespace {

// S2 对齐点：RuleKind 枚举名以 core/schedule_rule.h 实际定义为准；若 S2 另提供
// kind 字符串转换函数，删掉本函数直接换用。
core::schedule::RuleKind kindFromString(const std::string& kind) {
    if (kind == "once") return core::schedule::RuleKind::kOnce;
    if (kind == "interval") return core::schedule::RuleKind::kInterval;
    if (kind == "calendar") return core::schedule::RuleKind::kCalendar;
    if (kind == "cron") return core::schedule::RuleKind::kCron;
    if (kind == "event") return core::schedule::RuleKind::kEvent;
    return core::schedule::RuleKind::kResident;
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

} // namespace

ScheduleEngine::ScheduleEngine(core::data::SqliteDb& db, ChannelRegistry& channels)
    : store_(db), tasks_(), runner_(store_, channels, tasks_), channels_(channels) {}

void ScheduleEngine::resetForTest() {
    handle_ = 0;
    lastTickNow_ = 0;
    runningRuleIds_.clear();
    runToRule_.clear();
    runningFires_.clear();
    pendingFires_.clear();
    deliveryRetries_.clear();
    dueFires_.clear();
    heavyRunning_ = false;
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
    // 清内存队列；落库的 pending/running 行保留（重启靠唯一索引不重复执行）。
    pendingFires_.clear();
    deliveryRetries_.clear();
    runningFires_.clear();
    runToRule_.clear();
    runningRuleIds_.clear();
    heavyRunning_ = false;
}

void ScheduleEngine::tick(int64_t nowUnix) {
    if (lastTickNow_ != 0 && nowUnix < lastTickNow_) {
        // 时钟回拨/NTP（§4.8）：只更新 last_eval_at，不重放；下次按新时间重算。
        rollbackLastEval(nowUnix);
        lastTickNow_ = nowUnix;
        return;
    }
    lastTickNow_ = nowUnix;

    drainDeliveryRetries(nowUnix);
    drainPendingFires(nowUnix);

    auto rules = store_.listRules();
    if (rules.isFail()) {
        LOG_ERROR("schedule.engine: 规则列表读取失败 " + rules.errorMessage());
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
}

void ScheduleEngine::rollbackLastEval(int64_t nowUnix) {
    auto rules = store_.listRules();
    if (rules.isFail()) return;
    for (auto rule : rules.value()) {
        if (!rule.enabled || rule.archivedAt.has_value()) continue;
        if (rule.lastEvalAt == nowUnix) continue;
        rule.lastEvalAt = nowUnix;
        (void)store_.updateRule(rule);
    }
    LOG_WARN("schedule.engine: 时钟回拨，已刷新 last_eval_at 不重放");
}

int ScheduleEngine::firePriority(const RuleRecord& rule) {
    // 系统定时(3) > 用户提醒(2) > 静默动作(1)；系统层 S8 接入后由来源注入 3。
    if (rule.actionKind == "remind" || rule.actionKind == "remind_open") return 2;
    return 1;  // run_task / webhook_only 静默
}

void ScheduleEngine::evaluateRule(const RuleRecord& rule, int64_t nowUnix) {
    // 归档/停用：不参与调度（§4.4）。
    if (!rule.enabled || rule.archivedAt.has_value()) return;
    // 暂停中：期间到期点不补（§4.4）；只推进 last_eval_at，解除后从 now 干净续跑。
    if (rule.pausedUntil.has_value() && *rule.pausedUntil > nowUnix) {
        RuleRecord r = rule;
        r.lastEvalAt = nowUnix;
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
    ctx.lastEvalAt = rule.lastEvalAt.valueOr(nowUnix);

    RuleRecord r = rule;
    const int64_t lastEval = rule.lastEvalAt.valueOr(nowUnix);
    if (lastEval <= 0) {
        // 冷启动（首次评估）：不补历史；next_fire_at 缺失时初始化。
        r.lastEvalAt = nowUnix;
        if (!r.nextFireAt.has_value()) {
            auto next = core::schedule::nextFire(spec, nowUnix, ctx);
            r.nextFireAt = next.isOk() ? next.value() : std::nullopt;
        }
        (void)store_.updateRule(r);
        return;
    }

    if (nowUnix > lastEval) {
        const std::string& policy = rule.missedPolicy;
        if (policy == "skip") {
            // 不补：逐点写 missed 运行记录（§4.6）；窗口过大只记前 100 + 摘要。
            auto missed =
                core::schedule::missedFires(spec, lastEval, nowUnix, kMissedProbeCap, ctx);
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
            auto missed = core::schedule::missedFires(spec, lastEval, nowUnix, 1, ctx);
            if (missed.isOk() && !missed.value().empty()) {
                DueFire f;
                f.rule = rule;
                f.dueAt = missed.value().front();
                f.catchUp = true;
                f.priority = firePriority(rule);
                dueFires_.push_back(std::move(f));
            }
        } else {
            // all：最多补 100 次，超出合并为一条「遗漏超过 100 次」摘要（§4.6）。
            auto missed =
                core::schedule::missedFires(spec, lastEval, nowUnix, kMissedProbeCap, ctx);
            if (missed.isOk()) {
                const auto& points = missed.value();
                const std::size_t count = std::min(points.size(), std::size_t(kMissedCap));
                for (std::size_t i = 0; i < count; ++i) {
                    DueFire f;
                    f.rule = rule;
                    f.dueAt = points[i];
                    f.catchUp = true;
                    f.priority = firePriority(rule);
                    dueFires_.push_back(std::move(f));
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
        // 补跑处理完，next_fire_at 若仍停在 now 之前则推到未来
        //（finishFire 会再以 firedAt 重算，此处防 skip 路径遗留旧缓存）。
        if (r.nextFireAt.has_value() && *r.nextFireAt <= nowUnix) {
            auto next = core::schedule::nextFire(spec, nowUnix, ctx);
            r.nextFireAt = next.isOk() ? next.value() : std::nullopt;
        }
    } else if (r.nextFireAt.has_value() && *r.nextFireAt <= nowUnix) {
        // 窗口为空但缓存到期（missedFires 边界差异兜底）：正常触发。
        DueFire f;
        f.rule = rule;
        f.dueAt = *r.nextFireAt;
        f.catchUp = false;
        f.priority = firePriority(rule);
        dueFires_.push_back(std::move(f));
    }

    r.lastEvalAt = nowUnix;
    (void)store_.updateRule(r);
}

void ScheduleEngine::fireRule(const RuleRecord& rule, int64_t dueAt, bool catchUp,
                              int64_t nowUnix) {
    if (runExists(store_, rule.id, dueAt)) return;  // 唯一索引硬挡重复（§验收 6）

    const bool blocked = runningRuleIds_.count(rule.id) != 0 ||
                         (rule.actionKind == "run_task" && heavyRunning_);
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
    if (got.isFail() || !got.value().has_value()) return;
    RuleRecord rule = *got.value();

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
    (void)store_.updateRule(rule);

    if (onceKind) {
        // 一次性规则触发后归档停用（§4.4/验收 4）：不删除，重启不再触发。
        (void)store_.archiveRule(ruleId);
    }
}

core::Result<void> ScheduleEngine::recomputeNextFire(int64_t ruleId, int64_t nowUnix) {
    auto got = store_.getRule(ruleId);
    if (got.isFail()) return core::Result<void>::fail(got.errorCode(), got.errorMessage());
    if (!got.value().has_value()) {
        return core::Result<void>::fail(core::ErrorCode::NotFound, "规则不存在或已软删");
    }
    RuleRecord rule = *got.value();
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
        const bool blocked = runningRuleIds_.count(f.rule.id) != 0 ||
                             (f.rule.actionKind == "run_task" && heavyRunning_);
        if (blocked) break;
        pendingFires_.pop_front();
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
            retry.attempt += 1;
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
