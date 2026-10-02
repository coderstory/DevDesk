// src/quota_home_bridge.h
// G7：**首页余量卡片的中立只读桥**（同 `module_pet_bridge.h` 的做法）
//
// 为什么是「src/ 根的中立头」而不是 module_home 直读两张表：
//
//   需求是「首页展示 claude 切换器里所有 profile 的余量」，数据在
//   `profile_quota_config` / `profile_quota_result`（core/schema.h 里的稳定表，
//   与 `claude_call_logs` 同级）。直读在**连接与表**这一层不违反模块隔离
//   （HomeService 早就直读 claude_call_logs），真正会违约的是**解码层**：
//   windows_json 的编解码单点在 `quota_store.cpp::decodeQuotaWindows`、
//   错误枚举的列文本映射单点在 `quotaErrorFromName`、错误中文文案单点在
//   `quotaErrorHeadline`。首页若自己再写一份 JSON 解析 + 一份枚举映射 +
//   一份中文表，就是 F1 那类「两份真值源」——改一处漏一处，而且**没有任何
//   机制会告诉你漏了**（三份都能编译、都能显示、只是慢慢不一致）。
//
//   所以：数据与解码全部经本桥，**module_home 零 include module_claude_proxy**
//   （guard 口径同 pet 桥：`::module::claude::quota::` 前缀的**实现**在
//   module_claude_proxy 里，中立头只放纯数据类型 + 自由函数声明）。
//
// 三条不变量（首页渲染与用例都按这个读）：
//
//   1. **只读**。本桥不发任何 HTTP、不写任何行。首页的 `[刷新]` 是往刷新侧
//      写一个请求信号（`requestQuotaImmediateRefresh`），不是自己查网络。
//   2. **排序确定**：按 profile_id 升序返回。刷新结果回来不该让行序跳动。
//   3. **五态判据是纯函数**（`quotaCardState`），判据只吃三个入参且与
//      「windows 空不空」无关（Balance 型成功时 windows 恒空，拿它当判据
//      就是假 0% 的温床，见 quota_store.h 不变量 4）。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/result.h"
#include "core/sqlite_db.h"

namespace module::claude::quota {

// 首页余量行的**五态 + 一个必要补充态**（spec 2026-10-02-provider-quota-and-
// storage-design §5.1 五态表）。
//
//   Unconfigured  provider_id 为空（「未配置」→ 去配置按钮）
//   Incomplete    已选供应商但必填字段空 / key 读不到（NotConfigured 且从未成功）
//   Pending       配好了但一次都没成功过、也没错误（刚保存，定时器还没到点）
//   Normal        有过成功值且当前无错
//   FailedStale   当前有错但有过成功值（旧条 + 「可能过期」角标）
//   FailedNever   当前有错且从未成功过（只渲染错误行，绝不画 0% 条）
//
// `Pending` 不在 spec 的五态表里：spec 那张表的「正常」判据写的是「有窗口 /
// 有余额」，但那正是 G5 裁决 #6 踩过的坑（Balance 成功时 windows 恒空）。改用
// 「有过成功值」（kind 列非空）判正常之后，「刚配好、还没到点」就落进了表外，
// 必须给它一个显式态，否则要么被误判成失败、要么被误判成 0%。
enum class QuotaCardState {
    Unconfigured = 0,
    Incomplete,
    Pending,
    Normal,
    FailedStale,
    FailedNever,
};

// 余量形态（与 QuotaKind 同义；此处独立枚举以免中立头 include 供应商层头）。
enum class QuotaHomeKind { Tiered = 0, Balance = 1 };

// 一个窗口的展示态。usedPercent 是**已用**百分比，且**已由桥 clamp 到 [0,100]**
// （clampUsedPercent 的口径在这里兑现，首页不再自己裁）。
struct QuotaHomeWindow {
    std::string key;         // 规范键：five_hour / weekly / monthly
    std::string label;       // 中文标签：五小时 / 本周 / 本月（未知键回落为 key）
    double usedPercent = 0.0;
    std::string resetsAt;    // ISO8601；空串 = 无重置语义 / 上游未给
};

// 一个 profile 的余量展示行。字段全部是**已经算好的展示事实**，首页不再做
// 任何业务判断（这正是把桥做成「数据层 + 判据」而不是「原始行」的原因）。
struct QuotaHomeRow {
    int64_t profileId = 0;
    std::string profileName;
    std::string providerId;           // 空 = 未配置
    std::string providerDisplayName;  // 未注册供应商 = 空串

    QuotaCardState state = QuotaCardState::Unconfigured;
    QuotaHomeKind kind = QuotaHomeKind::Tiered;
    bool hasEverSucceeded = false;    // kind 列非空（**不是** windows 非空）
    bool hasError = false;            // last_error_kind 非 none / 非空

    std::vector<QuotaHomeWindow> windows;   // Tiered 用；Balance 恒空
    double balanceRemaining = 0.0;          // Balance 用
    std::string currency;
    std::string planName;

    int64_t lastSuccessAt = 0;   // **数据时间**（失败不推进），不是 fetchedAt
    // 面向用户的中文错误文案。**由桥保证：hasError 时恒非空**（见实现注释），
    // 首页可以直接渲染，不需要自己的错误文案表。
    std::string lastError;
    std::string lastErrorKind;   // 库列文本：none/auth/entitlement/shape/
                                 // transient/not_configured
};

// 五态判据（纯函数，无 IO）。`errorKindName` 是 `profile_quota_result` 的
// last_error_kind **列文本原样**（空串 = 这行从没写过 → 无错）。
//
// 优先级（顺序本身就是契约，改顺序必须改用例）：
//   1. providerId 空               → Unconfigured（哪怕有历史结果也不画条）
//   2. 无错 + 有过成功值            → Normal
//   3. 无错 + 从未成功              → Pending
//   4. 有错 + 有过成功值            → FailedStale
//   5. 有错 + 从未成功 + 配置类    → Incomplete
//   6. 其余（有错 + 从未成功）      → FailedNever
QuotaCardState quotaCardState(const std::string& providerId,
                              const std::string& errorKindName,
                              bool hasEverSucceeded);

// 列出**全部 profile** 的余量展示行，按 profile_id 升序（不变量 2）。
// 没有配置行的 profile 也会出现（state = Unconfigured）——用户的需求是
// 「所有 profile」，不是「配过余量的 profile」。
// 只读；任何一条子查询失败即整体 fail（首页显示「加载失败」，不静默空态）。
core::Result<std::vector<QuotaHomeRow>> listQuotaHomeRows(core::data::SqliteDb& db);

// ---- 手动刷新请求（G6 留给本批的欠账：保存后不即时刷新，后台最多等 60s）----
//
// 只置一个进程级请求位，**不阻塞、不发 HTTP**。后台刷新线程在 sleep 切片里
// 消费它 → 最坏 ~一个切片（默认 200ms）就开跑，比 interval 分钟或 60s 扫描
// 节流都快得多。
//
// 为什么不是同步 refreshNow()：那是真网络查询（每家 provider timeout 15s，
// 火山一次两发），N 个到期 profile 最坏 N×15s，**会把 UI 线程冻住**——
// 那正是批次 W5 把「测试连通」挪到后台要治的病。
void requestQuotaImmediateRefresh();

// 消费请求位（后台刷新线程每片调一次；返回 true = 本次消费到了，要立即跑一轮）。
// 做成可测的独立函数：断言「按钮点了之后确实有一轮被放行」不依赖读源码顺序。
bool consumeQuotaImmediateRefreshRequest();

}  // namespace module::claude::quota