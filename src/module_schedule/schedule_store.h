// 定时调度子系统存储层（S3）：schedule_rule / schedule_run / schedule_delivery /
// schedule_channel 四表 CRUD 的唯一入口。所有可能失败的操作返回 core::Result<T>；
// 经 core::data::SqliteDb 引用访问数据库。本层只做行读写与幂等守卫（唯一索引命中、
// 软删隔离）；时刻计算、重试编排、渠道选择归 core::schedule 与引擎层。
//
// 凭据契约：渠道 key 只存 platform::keychain（service devdesk.schedule），本层
// 只接触 secret_ref（keychain account 名）——API 上没有 key 入参、库里没有 key 列。
#pragma once

#include "core/result.h"
#include "core/sqlite_db.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace module::schedule {

// 用户规则行（schedule_rule）。kind ∈ once/interval/calendar/cron/event/resident；
// spec_json 按 kind 的字段结构（§4.1），action_json 按 action_kind 的参数结构（§4.3）。
struct RuleRecord {
    int64_t id = 0;
    std::string name;
    bool enabled = true;
    std::optional<int64_t> pausedUntil;   // 非空 = 暂停中，期间到期点不补（§4.4）
    std::string kind;
    std::string specJson;
    std::string tzId;                     // 空串 = 系统本地时区（v1 只支持本地，字段预留）
    std::string actionKind;               // remind/remind_open/run_task/webhook_only
    std::string actionJson;
    std::vector<int64_t> channelIds;      // 落库为 JSON 数组文本
    std::string missedPolicy;             // once/skip/all（§4.6）
    std::string conflictPolicy;           // priority 等（§4.7）
    int64_t maxRuns = 0;                  // 0 = 不限
    std::optional<int64_t> nextFireAt;    // Unix 秒缓存：换时区/改规则重算
    std::optional<int64_t> lastFireAt;
    std::optional<int64_t> lastEvalAt;
    int64_t runCount = 0;
    std::optional<int64_t> archivedAt;    // 一次性规则执行后归档（§4.4）
    std::optional<int64_t> deletedAt;     // 软删时间戳；查询侧一律过滤
    int64_t createdAt = 0;
    int64_t updatedAt = 0;
};

// 运行记录行（schedule_run）。origin='user' 或 'system:<module>'；系统项 rule_id 为空。
// status ∈ pending/running/success/failed/skipped/missed/suppressed（§4.5）。
struct RunRecord {
    int64_t id = 0;
    std::string origin;
    std::string originKey;
    std::optional<int64_t> ruleId;
    int64_t plannedAt = 0;
    std::optional<int64_t> startedAt;
    std::optional<int64_t> endedAt;
    std::string status = "pending";
    int attempt = 0;
    std::optional<int64_t> durationMs;
    std::string resultSummary;
    std::string error;
};

// 投递记录行（schedule_delivery）。每渠道每次尝试一行（§4.5）：
// 渠道失败只记本表，不改变 run 的 success。
// status ∈ pending/success/failed/retry/dropped。
struct DeliveryRecord {
    int64_t id = 0;
    int64_t runId = 0;
    int64_t channelId = 0;
    int attempt = 0;
    std::string status = "pending";
    std::optional<int64_t> responseCode;
    std::optional<int64_t> remoteErrcode;  // 企业微信 errcode 等业务错误码
    std::string error;
    std::optional<int64_t> nextRetryAt;
    int64_t createdAt = 0;
};

// 渠道配置行（schedule_channel）。kind ∈ fullscreen/toast/os/silent/wecom（§4.9）。
// secretRef 是 keychain account 名（如 wecom.<channel_id>），key 本身不落库。
struct ChannelRecord {
    int64_t id = 0;
    std::string kind;
    std::string name;
    bool enabled = true;
    std::string configJson;   // endpoint/msgtype/mentions/proxy_mode/timeout 等
    std::string secretRef;
    int64_t createdAt = 0;
    int64_t updatedAt = 0;
};

class ScheduleStore {
public:
    explicit ScheduleStore(core::data::SqliteDb& db);

    // ---- 规则 ----
    // 新建规则（入参 id 忽略）；created/updated 由 store 落当前时刻。
    core::Result<RuleRecord> createRule(const RuleRecord& rule);
    // 按 id 读（软删行视为 NotFound——软删后对外不可见）。
    core::Result<RuleRecord> getRule(int64_t id);
    // 全部未软删规则（含归档行；归档折叠是 UI 职责）。id 升序。
    core::Result<std::vector<RuleRecord>> listRules();
    // 业务字段覆盖更新（archived_at/deleted_at/created_at 不可经此写：归档与
    // 软删只有 archiveRule/deleteRule 两个入口）；id 不存在或已软删 → NotFound；updated_at 刷新。
    core::Result<void> updateRule(const RuleRecord& rule);
    // 软删（deleted_at = now）：配置与历史保留（§4.4）。
    core::Result<void> deleteRule(int64_t id);
    // 彻底删除规则行（「彻底删除」二次确认由 UI 层做）。schedule_run 无 FK，
    // 历史记录保留。已软删的行也可以 purge（清除保留期数据）。
    core::Result<void> purgeRule(int64_t id);
    // 一次性规则执行后归档：archived_at = now 且 enabled = 0（§4.4/验收 4）。
    core::Result<void> archiveRule(int64_t id);

    // ---- 运行记录 ----
    // 按唯一键查已有 run（重启补跑前判「是否已处理过」）。
    core::Result<std::optional<RunRecord>> findRun(const std::string& origin,
                                                   const std::string& originKey,
                                                   int64_t plannedAt);
    // 插入运行记录；(origin, origin_key, planned_at) 已存在 → AlreadyExists
    //（errorMessage 含已有行 id）。唯一索引是硬守卫：同规则同计划时刻只有一条 run。
    core::Result<RunRecord> insertRun(const RunRecord& run);
    // 全行覆盖更新（状态机推进）；id 不存在 → NotFound。
    core::Result<void> updateRun(const RunRecord& run);
    // 某规则（或系统项 origin_key）的运行历史，planned_at 倒序；limit 条。
    core::Result<std::vector<RunRecord>> listRunsByRule(int64_t ruleId, int limit);

    // ---- 投递记录 ----
    core::Result<DeliveryRecord> insertDelivery(const DeliveryRecord& delivery);
    // 全行覆盖更新（重试推进 attempt/status/next_retry_at）；id 不存在 → NotFound。
    core::Result<void> updateDelivery(const DeliveryRecord& delivery);
    // 某 run 的全部投递尝试，attempt 升序。
    core::Result<std::vector<DeliveryRecord>> listDeliveriesByRun(int64_t runId);

    // ---- 渠道配置 ----
    core::Result<ChannelRecord> createChannel(const ChannelRecord& channel);
    core::Result<std::optional<ChannelRecord>> getChannel(int64_t id);
    core::Result<std::vector<ChannelRecord>> listChannels();
    // 全行覆盖更新；id 不存在 → NotFound；updated_at 刷新。
    core::Result<void> updateChannel(const ChannelRecord& channel);
    // 硬删渠道配置（无历史保留要求；运行/投递历史不受影响）。
    core::Result<void> deleteChannel(int64_t id);

private:
    core::data::SqliteDb& db_;
};

} // namespace module::schedule
