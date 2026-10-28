// 动作执行与渠道投递编排（S4）：remind / remind_open / run_task / webhook_only
// 四动作 + 渠道 Receipt 分类消费（成功 / 可重试带建议延迟 / 永久失败）+ 重试排队
// 阶梯（0s/30s/300s，§4.9）+ schedule_delivery 每渠道每次尝试一行记账。
//
// 跨波对接约定（已按 dev 合入的 S5 真实接口对齐）：
//   - Delivery / Receipt / Channel / ChannelRegistry 由 S5 波定义在
//     module_schedule/channel_registry.h。真实 Receipt = {status, detail}
//     稳定串（分类映射见 action_runner.cpp classifyReceipt）；真实注册表键 =
//     渠道 kind 字符串，deliver(vector<string>, Delivery) 无 deliverLocal。
//     所有读 Receipt 字段、调 ChannelRegistry 方法的代码集中在
//     action_runner.cpp 的三个「S5 对齐点」函数里——再变只改这三处。
//   - 动作代码禁止写 if(wecom)：渠道选择由规则 channel_ids[] 决定，动作只按
//     列表分发（webhook_only 与 remind 共用同一分发路径，差别在规则配置）。
//   - deeplink 跳转动作（remind_open）只产出载荷（Delivery.deeplink），
//     跳转执行归 app 层。
//
// 运行记录口径（§4.5）：渠道失败只记 schedule_delivery，不改变 run 的
// success——run 成功、投递失败必须能分辨。
//
// 并发契约：本层全部在主线程调用（ticker 回调线程契约）；无锁。投递本身可能
// 是网络 IO，由 S5 渠道实现自行决定同步/异步；HTTP 每渠道并发 ≤2 由
// dispatchOnce 内的在飞计数强制——同步渠道恒不阻塞，异步渠道超并发时本次
// 尝试不记账、返回「0 秒后重试」交引擎重试队列。
#pragma once

#include "core/result.h"
#include "module_schedule/channel_registry.h"  // S5：Delivery / Receipt / Channel / ChannelRegistry

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace module::schedule {

class ScheduleStore;

// ---- Receipt 分类（消费侧唯一入口） ----
enum class ReceiptClass {
    kSuccess,     // 投递成功
    kRetryable,   // 可重试（Receipt 可携带建议延迟）
    kPermanent,   // 永久失败：不重试，停用渠道 + 本地告警（§4.9 93000 类）
};

// 投递尝试上限：3 次（§4.9）；阶梯 0s/30s/300s 为相对 planned_at 的绝对偏移。
inline constexpr int kMaxDeliveryAttempts = 3;
inline constexpr int64_t kRetryLadderSec[kMaxDeliveryAttempts] = {0, 30, 300};

// 第 attempt 次尝试（1 起）失败后的建议重试时刻偏移；Receipt 建议延迟更大时取建议值。
int64_t retryDelaySec(int attempt, int64_t suggestedSec);

// 内部任务注册表缝：app_assembly（S7）装配时注册真实任务（agent:<id> 等），
// 动作层只认 key 不接具体模块；未注册的 key invoke 返回 NotFound（调用方写降级）。
// 线程模型：registerTask 只在启动装配段/测试体内调用，invoke 运行期主线程只读。
struct TaskInvoke {
    std::string key;          // 如 agent:<task_id>、release:<check>
    std::string actionJson;   // 动作参数原文（任务自行解析）
    int64_t runId = 0;
    int64_t plannedAt = 0;    // 原定触发时刻
    int64_t nowUnix = 0;
};

// 任务结局：success / failed / async（异步任务：run 保持 running，
// 完成后由任务方回调 ActionRunner::completeAsync 收尾）。
struct TaskOutcome {
    std::string status = "success";  // success | failed | async
    std::string summary;
    std::string error;
};

class InternalTaskRegistry {
public:
    using TaskFn = std::function<core::Result<TaskOutcome>(const TaskInvoke&)>;

    // 重复注册同 key：后注册覆盖（装配段单线程，无并发问题）
    void registerTask(const std::string& key, TaskFn fn);
    core::Result<TaskOutcome> invoke(const TaskInvoke& invoke) const;
    void resetForTest();

private:
    std::map<std::string, TaskFn> tasks_;
};

// ---- 一次动作执行的上下文与结果 ----
struct RunContext {
    int64_t runId = 0;
    int64_t plannedAt = 0;      // 原定触发时刻（补跑 = 历史时刻，非 now）
    int64_t nowUnix = 0;
    std::string originKey;      // "rule:<id>"，用于投递 dedupeKey 合成
    std::string actionKind;     // remind/remind_open/run_task/webhook_only
    std::string actionJson;
    std::string ruleName;       // 提醒标题回退（action_json 无 title 时）
    std::vector<int64_t> channelIds;
};

// 待重试投递（引擎队列条目；Delivery 快照不可变——重试不得改正文，§4.9）
struct DeliveryRetry {
    int64_t runId = 0;
    int64_t channelId = 0;
    int attempt = 1;            // 下一次尝试的序号（2 起）
    int64_t plannedAt = 0;      // 原定触发时刻（重试阶梯 0s/30s/300s 的锚点）
    int64_t nextRetryAt = 0;
    module::schedule::Delivery delivery;  // 依赖 S5 头里的 Delivery
};

struct RunOutcome {
    std::string status = "success";   // success | failed | async
    std::string summary;
    std::string error;
    std::vector<DeliveryRetry> retries;  // 非终结投递，交引擎重试队列
};

// 单次投递尝试的结局（引擎据此决定继续排队还是收尾）
struct RetryOutcome {
    bool terminal = true;               // true = 投递生命周期结束（成功/永久失败/达上限）
    std::optional<int64_t> nextRetryAt;
    bool success = false;
    bool degradedLocally = false;       // 最终失败降级本地提示一次（§4.9）
    bool channelDisabled = false;       // 永久失败停用渠道
    std::string error;
};

class ActionRunner {
public:
    ActionRunner(ScheduleStore& store, ChannelRegistry& channels,
                 InternalTaskRegistry& tasks);

    // 执行一次动作：本地动作就地执行；渠道投递做第 1 次尝试，
    // 可重试的塞进 RunOutcome.retries 由引擎排队。不落 run 行（引擎负责）。
    core::Result<RunOutcome> run(const RunContext& ctx);

    // 引擎重试队列推进：做第 retry.attempt 次尝试并记账。
    core::Result<RetryOutcome> attemptDelivery(const DeliveryRetry& retry, int64_t nowUnix);

    // 异步任务收尾（run_task 返回 async 后由任务方调用）：
    // 更新 run 行终态；run 的最终状态机归引擎调度，本函数只落库。
    core::Result<RetryOutcome> completeAsync(int64_t runId, const TaskOutcome& outcome);

    // 溢出摘要等本地一次性告警：走注册表内置 toast 渠道（S5 对齐点）。
    core::Result<Receipt> localAlert(const Delivery& delivery);

    // 单渠道单次尝试（供引擎/测试直接使用）：记账 + deliver + 分类。
    // slotBusy 时返回非终结、nextRetryAt=now 且不记账。
    core::Result<RetryOutcome> dispatchOnce(int64_t channelId, const Delivery& delivery,
                                            int runId, int attempt, int64_t plannedAt,
                                            int64_t nowUnix);
    void resetForTest();

private:
    ScheduleStore& store_;
    ChannelRegistry& channels_;
    InternalTaskRegistry& tasks_;
    std::map<int64_t, int> httpInFlight_;  // 渠道在飞计数（HTTP 每渠道 ≤2，§4.7）
};

// Receipt → 分类。S5 对齐点：唯一读 Receipt 字段的函数（按 status 稳定串分类）。
ReceiptClass classifyReceipt(const module::schedule::Receipt& receipt);

// 从 action_json 提取 title/body/deeplink/key（缺省回退规则名）；失败不抛，回退。
struct ActionText {
    std::string title;
    std::string body;
    std::string key;             // run_task 的内部任务 key（如 agent:<id>）
    std::optional<std::string> deeplink;
};
ActionText parseActionText(const std::string& actionJson, const std::string& ruleName);

} // namespace module::schedule
