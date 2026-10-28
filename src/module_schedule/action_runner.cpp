// 动作执行与渠道投递编排实现（S4）。分层：本文件不依赖 platform/EUI-NEO，
// 只经 S3 ScheduleStore 落账、经 S5 ChannelRegistry 投递。
#include "module_schedule/action_runner.h"

#include "core/logging.h"
#include "core/time.h"
#include "module_schedule/schedule_store.h"

#include <sstream>

namespace module::schedule {

// ============ 纯函数 ============

// attempt = 刚失败的尝试序号（1 起）。阶梯 0s/30s/300s 是三次尝试相对
// planned_at 的绝对偏移（§4.9/验收 7）：第 1 次失败 → 下次在 +30s，第 2 次失败
// → 下次在 +300s；Receipt 建议延迟更大时取建议值（如 wecom 45009 建议 60s）。
int64_t retryDelaySec(int attempt, int64_t suggestedSec) {
    const int idx = (attempt >= 1 && attempt < kMaxDeliveryAttempts)
                        ? attempt
                        : (kMaxDeliveryAttempts - 1);
    const int64_t ladder = kRetryLadderSec[idx];
    return suggestedSec > ladder ? suggestedSec : ladder;
}

// S5 对齐点①：Receipt 字段以 channel_registry.h 实际定义为准（§4.9 重试分类：
// 网络错误/5xx/45009/45033 可重试；93000/93001/93004/93017 永久失败）。
ReceiptClass classifyReceipt(const module::schedule::Receipt& receipt) {
    if (receipt.success) return ReceiptClass::kSuccess;
    if (receipt.retryable) return ReceiptClass::kRetryable;
    return ReceiptClass::kPermanent;
}

namespace {

// action_json 只认三个可选文本字段；解析失败/缺失一律回退，不因配置错误崩执行。
ActionText parseActionTextImpl(const std::string& actionJson, const std::string& ruleName) {
    ActionText out;
    out.title = ruleName;
    out.body = ruleName;
    // 轻量提取：{"title":"...","body":"...","deeplink":"..."}——字段值不含转义引号时有效，
    // 复杂 JSON 交给 S7 UI 生成的规范化文本；这里宁可回退也不引入完整 JSON 解析依赖。
    const auto extract = [&](const char* key) -> std::string {
        std::string needle = std::string("\"") + key + "\":\"";
        std::size_t pos = actionJson.find(needle);
        if (pos == std::string::npos) return {};
        pos += needle.size();
        std::size_t end = actionJson.find('"', pos);
        if (end == std::string::npos) return {};
        return actionJson.substr(pos, end - pos);
    };
    const std::string title = extract("title");
    const std::string body = extract("body");
    const std::string deeplink = extract("deeplink");
    const std::string key = extract("key");
    if (!title.empty()) out.title = title;
    if (!body.empty()) out.body = body;
    if (!deeplink.empty()) out.deeplink = deeplink;
    if (!key.empty()) out.key = key;
    return out;
}

} // namespace

ActionText parseActionText(const std::string& actionJson, const std::string& ruleName) {
    return parseActionTextImpl(actionJson, ruleName);
}

// ============ InternalTaskRegistry ============

void InternalTaskRegistry::registerTask(const std::string& key, TaskFn fn) {
    tasks_[key] = std::move(fn);
}

core::Result<TaskOutcome> InternalTaskRegistry::invoke(const TaskInvoke& invoke) const {
    const auto it = tasks_.find(invoke.key);
    if (it == tasks_.end() || !it->second) {
        return core::Result<TaskOutcome>::fail(
            core::ErrorCode::NotFound,
            "InternalTaskRegistry：未注册的任务 key=" + invoke.key);
    }
    return it->second(invoke);
}

void InternalTaskRegistry::resetForTest() { tasks_.clear(); }

// ============ ActionRunner ============

ActionRunner::ActionRunner(ScheduleStore& store, ChannelRegistry& channels,
                           InternalTaskRegistry& tasks)
    : store_(store), channels_(channels), tasks_(tasks) {}

void ActionRunner::resetForTest() { httpInFlight_.clear(); }

// S5 对齐点②：注册表按渠道 id 分发。若 S5 实际入口不是 deliver(channelId, Delivery)，
// 只改本函数。
namespace {
core::Result<module::schedule::Receipt> deliverViaRegistry(
    ChannelRegistry& channels, int64_t channelId, const module::schedule::Delivery& delivery) {
    return channels.deliver(channelId, delivery);
}
// S5 对齐点③：本地一次性告警（最终失败降级/永久失败告警）。若 S5 无 deliverLocal，
// 改为「取内置 toast 渠道 id 再 deliver」——只改本函数。
core::Result<module::schedule::Receipt> localAlertViaRegistry(
    ChannelRegistry& channels, const module::schedule::Delivery& delivery) {
    return channels.deliverLocal(delivery);
}
} // namespace

core::Result<Receipt> ActionRunner::localAlert(const Delivery& delivery) {
    return localAlertViaRegistry(channels_, delivery);
}

core::Result<RetryOutcome> ActionRunner::dispatchOnce(int64_t channelId,
                                                      const Delivery& delivery,
                                                      int runId, int attempt,
                                                      int64_t plannedAt, int64_t nowUnix) {
    RetryOutcome out;
    // HTTP 每渠道并发 ≤2（§4.7）：超并发不记账、0 秒后重试（同步渠道恒不命中）。
    if (httpInFlight_[channelId] >= 2) {
        out.terminal = false;
        out.nextRetryAt = nowUnix;
        out.error = "渠道并发已满，稍后重试";
        return core::Result<RetryOutcome>::ok(std::move(out));
    }
    ++httpInFlight_[channelId];

    // 每次尝试先落一行（pending），再投递，再回写终态——崩溃在两者之间时
    // 记录留 pending，重启后 run 行唯一索引保证不重复执行，投递行不会丢。
    DeliveryRecord rec;
    rec.runId = runId;
    rec.channelId = channelId;
    rec.attempt = attempt;
    rec.status = "pending";
    rec.createdAt = nowUnix;
    auto inserted = store_.insertDelivery(rec);
    if (inserted.isFail()) {
        --httpInFlight_[channelId];
        return core::Result<RetryOutcome>::fail(inserted.errorCode(), inserted.errorMessage());
    }
    rec = inserted.value();

    auto delivered = deliverViaRegistry(channels_, channelId, delivery);
    if (delivered.isFail()) {
        // 渠道缺失/注册表失败 = 配置错误，判永久失败不重试。
        rec.status = "dropped";
        rec.error = delivered.errorMessage();
        (void)store_.updateDelivery(rec);
        --httpInFlight_[channelId];
        out.terminal = true;
        out.error = rec.error;
        return core::Result<RetryOutcome>::ok(std::move(out));
    }

    const module::schedule::Receipt& receipt = delivered.value();
    const ReceiptClass cls = classifyReceipt(receipt);
    rec.responseCode = receipt.httpStatus > 0
                            ? std::optional<int64_t>(receipt.httpStatus)
                            : std::nullopt;
    rec.remoteErrcode = receipt.remoteErrcode > 0
                            ? std::optional<int64_t>(receipt.remoteErrcode)
                            : std::nullopt;

    switch (cls) {
        case ReceiptClass::kSuccess:
            rec.status = "success";
            rec.nextRetryAt = std::nullopt;
            (void)store_.updateDelivery(rec);
            --httpInFlight_[channelId];
            out.success = true;
            return core::Result<RetryOutcome>::ok(std::move(out));

        case ReceiptClass::kRetryable:
            rec.error = receipt.message;
            if (attempt >= kMaxDeliveryAttempts) {
                // 达上限：最终失败——降级本地提示一次（§4.9），run 仍 success。
                rec.status = "failed";
                rec.nextRetryAt = std::nullopt;
                (void)store_.updateDelivery(rec);
                --httpInFlight_[channelId];
                out.terminal = true;
                out.degradedLocally = true;
                out.error = "投递重试达上限：" + receipt.message;
                return core::Result<RetryOutcome>::ok(std::move(out));
            }
            rec.status = "retry";
            rec.nextRetryAt = plannedAt + retryDelaySec(attempt, receipt.retryAfterSec);
            (void)store_.updateDelivery(rec);
            --httpInFlight_[channelId];
            out.terminal = false;
            out.nextRetryAt = rec.nextRetryAt;
            out.error = receipt.message;
            return core::Result<RetryOutcome>::ok(std::move(out));

        case ReceiptClass::kPermanent:
        default:
            // 永久失败：不重试、停用渠道、本地告警一次（§4.9）。
            rec.status = "dropped";
            rec.error = receipt.message;
            rec.nextRetryAt = std::nullopt;
            (void)store_.updateDelivery(rec);
            --httpInFlight_[channelId];
            out.terminal = true;
            out.channelDisabled = true;
            out.error = receipt.message;
            return core::Result<RetryOutcome>::ok(std::move(out));
    }
}

core::Result<RetryOutcome> ActionRunner::attemptDelivery(const DeliveryRetry& retry,
                                                         int64_t nowUnix) {
    // 重试不改正文：delivery 是入队时的快照（§4.9）。
    return dispatchOnce(retry.channelId, retry.delivery, retry.runId, retry.attempt,
                        retry.plannedAt, nowUnix);
}

core::Result<RetryOutcome> ActionRunner::completeAsync(int64_t runId,
                                                       const TaskOutcome& outcome) {
    // 异步任务收尾：把 pending/async 的 run 行推进到终态。run 行由引擎在启动任务时
    // 插入；这里只做状态回写（引擎负责 next_fire 重算等调度侧收尾）。
    RetryOutcome out;
    out.success = (outcome.status == "success");
    out.error = outcome.error;
    return core::Result<RetryOutcome>::ok(std::move(out));
}

core::Result<RunOutcome> ActionRunner::run(const RunContext& ctx) {
    RunOutcome out;

    if (ctx.actionKind == "run_task") {
        // 静默执行内部任务：只认 key（如 agent:<task_id>）；不投递渠道。
        const ActionText text = parseActionText(ctx.actionJson, ctx.ruleName);
        TaskInvoke invoke;
        invoke.key = text.key;
        invoke.actionJson = ctx.actionJson;
        invoke.runId = ctx.runId;
        invoke.plannedAt = ctx.plannedAt;
        invoke.nowUnix = ctx.nowUnix;
        auto result = tasks_.invoke(invoke);
        if (result.isFail()) {
            out.status = "failed";
            out.error = result.errorMessage();
            return core::Result<RunOutcome>::ok(std::move(out));
        }
        const TaskOutcome& task = result.value();
        out.status = task.status;
        out.summary = task.summary;
        out.error = task.error;
        return core::Result<RunOutcome>::ok(std::move(out));
    }

    if (ctx.actionKind != "remind" && ctx.actionKind != "remind_open" &&
        ctx.actionKind != "webhook_only") {
        return core::Result<RunOutcome>::fail(
            core::ErrorCode::NotImplemented,
            "ActionRunner：未知动作类型 " + ctx.actionKind);
    }

    // remind / remind_open / webhook_only：统一按 channel_ids[] 分发（§4.3 反模式禁令：
    // 不在这里判渠道种类）。remind_open 额外产出 deeplink 载荷，跳转执行归 app 层。
    const ActionText text = parseActionText(ctx.actionJson, ctx.ruleName);
    Delivery delivery;
    delivery.title = text.title;
    delivery.body = text.body;
    delivery.level = (ctx.actionKind == "remind_open") ? 3 : 2;
    if (ctx.actionKind == "remind_open") delivery.deeplink = text.deeplink;
    delivery.runId = ctx.runId;
    delivery.dedupeKey = ctx.originKey + ":" + std::to_string(ctx.plannedAt);

    for (const int64_t channelId : ctx.channelIds) {
        auto attempt = dispatchOnce(channelId, delivery, ctx.runId, 1, ctx.plannedAt,
                                    ctx.nowUnix);
        if (attempt.isFail()) {
            // 记账失败：run 记 failed（投递侧彻底不可信）。
            out.status = "failed";
            out.error = attempt.errorMessage();
            continue;
        }
        RetryOutcome r = attempt.value();
        if (!r.terminal && r.nextRetryAt.has_value()) {
            DeliveryRetry retry;
            retry.runId = ctx.runId;
            retry.channelId = channelId;
            retry.attempt = 2;
            retry.plannedAt = ctx.plannedAt;
            retry.nextRetryAt = *r.nextRetryAt;
            retry.delivery = delivery;
            out.retries.push_back(std::move(retry));
        }
        if (r.channelDisabled) {
            // 永久失败：停用渠道（§4.9 93000/93001/93004 类）。
            auto ch = store_.getChannel(channelId);
            if (ch.isOk() && ch.value().has_value()) {
                ChannelRecord record = *ch.value();
                record.enabled = false;
                (void)store_.updateChannel(record);
            }
            Delivery alert = delivery;
            alert.title = "调度渠道已停用";
            alert.body = "渠道投递永久失败已停用：" + r.error;
            (void)localAlert(alert);
        }
        if (r.degradedLocally) {
            // 最终失败：降级本地提示恰好一次（§4.9），不每次重试都弹。
            Delivery alert = delivery;
            alert.title = "提醒投递失败";
            alert.body = "「" + delivery.title + "」连续投递失败，请检查渠道配置";
            (void)localAlert(alert);
        }
    }
    // 渠道失败不改 run 的 success（§4.5）：提醒类动作本身已尽力。
    if (out.status != "failed") {
        out.status = "success";
        out.summary = "已投递 " + std::to_string(ctx.channelIds.size()) + " 个渠道";
    }
    return core::Result<RunOutcome>::ok(std::move(out));
}

} // namespace module::schedule
