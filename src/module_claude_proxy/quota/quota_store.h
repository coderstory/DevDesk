// src/module_claude_proxy/quota/quota_store.h
// 余量配置的**唯一落库口**（G5）
// （实施计划 docs/superpowers/plans/2026-10-02-quota-implementation-plan.md §G5；
//  表结构 = spec 2026-10-02-provider-quota-and-storage-design §4.1）
//
// 两张表、逐 profile 一行：
//   profile_quota_config  配了哪家供应商 + 额外字段 + 刷新策略
//   profile_quota_result  最近一次查询的结果 + 最近一次错误
//
// 四条贯穿本文件的不变量（下游 G6/G7/G8 全按这个读）：
//
//   1. **windows_json 的编解码只有这一处**（encodeQuotaWindows /
//      decodeQuotaWindows），行结构里存的是**已解码的 `std::vector<QuotaWindow>`**。
//      于是任何消费方都不可能手搓 JSON 拼串——那是 message_blocks.h
//      （agent_messages.contentJson 的唯一解码点）同款纪律：编解码散成两份
//      就是「改一处漏一处」，往返用例（test_claude_quota_store）钉住它。
//
//   2. **落库句柄一律经本类**，不做第二把连接。构造收 `core::data::SqliteDb&`
//      （全进程唯一那把），每个公开方法 `std::lock_guard` 覆盖整段
//      prepare → bind → step → fetchAll（agent_store.cpp 同款纪律；
//      core/sqlite_db.h 头注释：macOS libsqlite3 THREADSAFE=2，连接内**没有**
//      自家互斥，并发调用直接损坏连接）。
//
//   3. **失败韧性只有一处决策**：`applyFetchResult` 是「一次 fetch 落到表上」
//      的唯一入口，成功 / Transient / 鉴权类三类走同一段代码（见实现注释）。
//      谁要是绕过它直接 saveResult，韧性就废了——所以本文件**不**对外暴露
//      「写任意 result 行」的语义化入口给刷新路径。
//
//   4. **「从未成功过」必须可分辨**：数据列（kind / windows / balance）只在
//      **成功**时写；失败只写 last_error / last_error_kind。G7 靠
//      `result.hasEverSucceeded() == false` 决定「只渲染错误行、绝不画 0% 条」。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/result.h"
#include "core/sqlite_db.h"
#include "quota_provider.h"

namespace module::claude::quota {

// ============ windows_json 编解码（**单点**，见不变量 1）============

// QuotaWindow 数组 → JSON 原文。空数组 → "[]"（不是空串——空串在库里读回来
// 会被 decode 当成「没查过」，与「查到但零窗口」混淆）。
// usedPercent 按 double 存（上游给小数不截断，见 quota_provider.h 不变量 3）。
std::string encodeQuotaWindows(const std::vector<QuotaWindow>& windows);

// JSON 原文 → QuotaWindow 数组。**容错不报错**（同 message_blocks 的读侧口径：
// 这是展示面，一行坏 JSON 不该让整个列表崩）：
//   空串 → 空列表；非 JSON / 非数组 → 空列表；元素非对象或缺 key → 跳过该元素。
core::Result<std::vector<QuotaWindow>> decodeQuotaWindows(const std::string& json);

// ============ 枚举 ↔ 列文本（last_error_kind / kind 两列的唯一映射）============

// QuotaError → 库列文本。"none" 是**成功**的落库值（不是空串）：空串在 schema
// 里是「这行还没写过」的默认值，与「写过且当前无错」必须可分辨。
const char* quotaErrorName(QuotaError error);

// 库列文本 → QuotaError。未知文本 → QuotaError::Transient（**不**回落 None：
// 认不出的错误当成「没错」= 把失败伪装成成功，正是 G7 要避免的假象）。
QuotaError quotaErrorFromName(const std::string& name);

// QuotaKind → 库列文本（"tiered" / "balance"）。
const char* quotaKindName(QuotaKind kind);

// ============ 行结构 ============

// profile_quota_config 一行。默认值与 DDL 默认值**逐项一致**（providerId 空 =
// 未配置；enabled 默认 false——「配了就要显式打开」；intervalMinutes 默认 15）。
struct QuotaConfigRow {
    int64_t profileId = 0;
    std::string providerId;    // 空串 = 该 profile 未配置余量查询（G7「去配置」）
    std::string extraJson;     // fields() 里非 secret 键的 JSON 对象原文
    // fields() 里 secret 键的 JSON 对象原文。**结构体里始终是明文**：写库时
    // 经 platform::credential::protect（Windows = DPAPI 密文 hex），读库时经
    // unprotect —— 与 api_key_encrypted 复用同一对函数（评审 M3：同表两列
    // 保护级别必须一致，不能让 secrets 落成明文而兄弟列是密文）。
    // 调用方只管给明文 / 拿明文，不感知存储形态。
    std::string secretsJson;
    bool enabled = false;
    bool autoRefresh = true;
    int intervalMinutes = 15;
};

// profile_quota_result 一行。windows 是**解码后**的内存形态（见不变量 1）。
struct QuotaResultRow {
    int64_t profileId = 0;
    QuotaKind kind = QuotaKind::Tiered;
    // kind 列是否已被写成合法值。**false = 从未成功过一次**（失败落库不写它）。
    // G7「失败且从未成功 → 只渲染错误行」的判据（不靠 windows 空不空来猜：
    // Balance 型成功时 windows 本来就恒空，见不变量 4）。
    bool hasEverSucceeded = false;
    std::vector<QuotaWindow> windows;
    double balanceRemaining = 0.0;
    std::string currency;
    std::string planName;
    // 最近一次**尝试**的时间戳（成败都写）。**只用于驱动定时器的到期判定**
    //（interval 从它起算，避免一次持续失败每个 tick 重打供应商 = 重试风暴）。
    int64_t fetchedAt = 0;
    // 最近一次**成功**的时间戳（失败不推进）。**G7 显示「数据时间」用这一列**，
    // 不要用 fetchedAt：一次 Transient 失败后数据仍是 interval 之前的，若拿
    // fetchedAt 展示会得到「15 分钟前的数字 + 刚刚查询过」这种自相矛盾的话
    //（评审 m1）。
    int64_t lastSuccessAt = 0;
    std::string lastError;                 // 面向用户的中文文案；成功时清空
    QuotaError lastErrorKind = QuotaError::None;

    // 「从未成功过」：kind 列从未被写过。此时数据列一律是初始值
    // （windows 空 / balance 0）——**这正是「不写假 0%」的落库形态**。
    bool neverSucceeded() const { return !hasEverSucceeded; }

    // 当前带着错误吗（UI 决定要不要挂「可能过期」角标）。
    bool hasError() const { return lastErrorKind != QuotaError::None; }
};

// ============ 存储 ============

class QuotaStore {
public:
    explicit QuotaStore(core::data::SqliteDb& db) : db_(db) {}

    // ---- profile_quota_config ----

    // 读一行；无行 → fail(NotFound)（**不是**返回一行默认值——「没配过」与
    // 「配了但字段全空」在 UI 上是两种引导，不能混）。
    core::Result<QuotaConfigRow> loadConfig(int64_t profileId);

    // 写一行（INSERT OR REPLACE 语义：整行覆盖，调用方读改写）。
    core::Result<void> saveConfig(const QuotaConfigRow& row);

    // 删一行；无行 → fail(NotFound)。
    core::Result<void> deleteConfig(int64_t profileId);

    // 列出全部配置行（定时器一轮扫的就是它；按 profile_id 升序，结果确定）。
    core::Result<std::vector<QuotaConfigRow>> listConfigs();

    // ---- profile_quota_result ----

    core::Result<QuotaResultRow> loadResult(int64_t profileId);
    core::Result<void> deleteResult(int64_t profileId);
    core::Result<std::vector<QuotaResultRow>> listResults();

    // **一次 fetch 落库的唯一入口**（不变量 3）。刷新路径只许调它。
    //
    // 三类结局（决策集中在这里，下游不重复判断）：
    //   error == None                  → 写数据列（kind/windows/balance/…）、
    //                                     清空 last_error / last_error_kind
    //   error == Transient             → **保留上次成功的数据列**，只写
    //                                     last_error / last_error_kind
    //   error == Auth/Entitlement/
    //          Shape/NotConfigured     → 同上（保留数据列 + 写错误），差异由
    //                                     last_error_kind 承载，UI 据此选文案
    //
    // 保留数据列的理由（spec §5.1 五态）：网络抖一下不该让用户的余量条整个消失。
    // 「失败且有过成功值」→ 旧条 + 角标；「失败且从未成功」→ 只有错误行。
    //
    // fetchedAt 无论成败都写（驱动到期判定，见 QuotaResultRow::fetchedAt）。
    // 任何一条 DB 写失败都透传错误，**不**吞。
    core::Result<void> applyFetchResult(int64_t profileId, const QuotaResult& result,
                                        int64_t fetchedAt);

private:
    core::data::SqliteDb& db_;
};

}  // namespace module::claude::quota
