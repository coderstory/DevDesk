// src/module_claude_proxy/quota/quota_store.h
// 余量配置的唯一落库口。两张表、逐 profile 一行：profile_quota_config（供应商 + 额外字段 + 刷新策略）、profile_quota_result（最近一次结果 + 错误）。
// 不变量：1) windows_json 编解码只有这一处，任何消费方不得手搓 JSON；2) 落库句柄一律经本类（不做第二把连接），
//   每个公开方法 lock_guard 覆盖整段 prepare → step → fetchAll（连接内无互斥，并发直接损坏连接）；
//   3) applyFetchResult 是「一次 fetch 落表」的唯一入口；4) 数据列只在成功时写，失败只写错误两列。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/result.h"
#include "core/sqlite_db.h"
#include "quota_provider.h"

namespace module::claude::quota {

// QuotaWindow 数组 → JSON 原文。空数组 → "[]"（空串读回会被当成「没查过」，与「查到但没有窗口」混淆）。usedPercent 按 double 存（不截断）。
std::string encodeQuotaWindows(const std::vector<QuotaWindow>& windows);

// JSON 原文 → QuotaWindow 数组。容错不报错（展示面，一行坏 JSON 不该让整个列表崩）：空串 / 非 JSON / 非数组 → 空列表；元素非对象或缺 key → 跳过。
core::Result<std::vector<QuotaWindow>> decodeQuotaWindows(const std::string& json);

// QuotaError → 库列文本。"none" 是成功的落库值（不是空串）：空串在 schema 里是「这行还没写过」，两者必须可分辨。
const char* quotaErrorName(QuotaError error);

// 库列文本 → QuotaError。未知文本 → Transient（不回落 None：认不出的错误当「没错」= 把失败伪装成成功）。
QuotaError quotaErrorFromName(const std::string& name);

// QuotaKind → 库列文本（"tiered" / "balance"）。
const char* quotaKindName(QuotaKind kind);

// profile_quota_config 一行。默认值与 DDL 逐项一致（providerId 空 = 未配置；enabled 默认 false；intervalMinutes 默认 15）。
struct QuotaConfigRow {
    int64_t profileId = 0;
    std::string providerId;    // 空串 = 该 profile 未配置余量查询
    std::string extraJson;     // fields() 里非 secret 键的 JSON 对象原文
    // fields() 里 secret 键的 JSON 对象原文。结构体里始终是明文：写库经 platform::credential::protect、读库经 unprotect（与 api_key_encrypted 同一对函数）。
    std::string secretsJson;
    // cc-switch provider 引用，非空 = 凭据经 cc-switch 只读读取器现取；代码保证非空时 extraJson / secretsJson 一律清空。
    std::string ccswitchProviderId;
    bool enabled = false;
    bool autoRefresh = true;
    int intervalMinutes = 15;
};

// profile_quota_result 一行。windows 是解码后的内存形态（见不变量 1）。
struct QuotaResultRow {
    int64_t profileId = 0;
    QuotaKind kind = QuotaKind::Tiered;
    // kind 列是否已被写成合法值。false = 从未成功过一次（失败落库不写它），是「失败且从未成功只渲染错误行」的判据（不靠 windows 空不空猜）。
    bool hasEverSucceeded = false;
    std::vector<QuotaWindow> windows;
    double balanceRemaining = 0.0;
    std::string currency;
    std::string planName;
    // 最近一次尝试的时间戳（成败都写）。只用于驱动定时器到期判定（interval 从它起算，避免重试风暴）。
    int64_t fetchedAt = 0;
    // 最近一次成功的时间戳（失败不推进）。显示「数据时间」用这一列，不要用 fetchedAt（否则会得到「15 分钟前的数字 + 刚刚查询过」）。
    int64_t lastSuccessAt = 0;
    std::string lastError;                 // 面向用户的中文文案；成功时清空
    QuotaError lastErrorKind = QuotaError::None;

    // 「从未成功过」：kind 列从未被写过，数据列一律初始值（windows 空 / balance 0）。
    bool neverSucceeded() const { return !hasEverSucceeded; }

    bool hasError() const { return lastErrorKind != QuotaError::None; }
};

class QuotaStore {
public:
    explicit QuotaStore(core::data::SqliteDb& db) : db_(db) {}

    // 读一行；无行 → fail(NotFound)（不返回默认行：「没配过」与「配了但字段全空」在 UI 上是两种引导）。
    core::Result<QuotaConfigRow> loadConfig(int64_t profileId);

    // 写一行（INSERT OR REPLACE 语义：整行覆盖，调用方读改写）。
    core::Result<void> saveConfig(const QuotaConfigRow& row);

    // 删一行；无行 → fail(NotFound)。
    core::Result<void> deleteConfig(int64_t profileId);

    // 列出全部配置行（定时器一轮扫的就是它；按 profile_id 升序，结果确定）。
    core::Result<std::vector<QuotaConfigRow>> listConfigs();

    core::Result<QuotaResultRow> loadResult(int64_t profileId);
    core::Result<void> deleteResult(int64_t profileId);
    core::Result<std::vector<QuotaResultRow>> listResults();

    // 一次 fetch 落库的唯一入口（不变量 3），刷新路径只许调它。fetchedAt 无论成败都写。
    // None → 写数据列并清空错误两列；其余错误 → 保留上次成功的数据列，只写错误两列（网络抖动不该让余量条消失）。
    core::Result<void> applyFetchResult(int64_t profileId, const QuotaResult& result,
                                        int64_t fetchedAt);

private:
    core::data::SqliteDb& db_;
};

}  // namespace module::claude::quota
