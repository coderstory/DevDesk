// 首页余量卡片的中立只读桥：数据与解码单点在此，module_home 零 include module_claude_proxy。
// 三条不变量：1) 只读——不发 HTTP 不写行，[刷新] 只置请求信号；2) 排序确定——按 profile_id 升序；
// 3) 五态判据是纯函数 quotaCardState
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "core/result.h"
#include "core/sqlite_db.h"

namespace module::claude::quota {

// 五态语义见 quotaCardState（优先级即契约；FailedNever 只渲染错误行，绝不画 0% 条）。
// Pending 是补充态：Balance 成功时 windows 恒空，须以「有过成功值」（kind 列非空）显式区分，
// 否则刚配好没到点被误判成失败或 0%
enum class QuotaCardState {
    Unconfigured = 0,
    Incomplete,
    Pending,
    Normal,
    FailedStale,
    FailedNever,
};

// 余量形态（与 quota_provider.h 的 QuotaKind 同义）。本头保持中立，不能拖入供应商层头，
// 故另立同义枚举；同步闸在 quota_home_view.cpp 的 static_assert（含哨兵 Count 相等）
enum class QuotaHomeKind { Tiered = 0, Balance = 1, Count = 2 };

// 一个窗口的展示态。usedPercent 是已用百分比，已由桥 clamp 到 [0,100]（首页不再自己裁）
struct QuotaHomeWindow {
    std::string key;         // 规范键：five_hour / weekly / monthly
    std::string label;       // 中文标签：五小时 / 本周 / 本月（未知键回落为 key）
    double usedPercent = 0.0;
    std::string resetsAt;    // ISO8601；空串 = 无重置语义 / 上游未给
};

// 一个 profile 的余量展示行，字段全是已算好的展示事实，首页不再做业务判断
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

    int64_t lastSuccessAt = 0;   // 数据时间（失败不推进），不是 fetchedAt
    // 面向用户的中文错误文案；桥保证 hasError 时恒非空，首页可直接渲染
    std::string lastError;
    std::string lastErrorKind;   // 库列文本：none/auth/entitlement/shape/transient/not_configured
};

// 五态判据（纯函数，无 IO）；errorKindName 是 last_error_kind 列文本原样（空串=无错）。优先级即
// 契约（改顺序必须改用例）：providerId 空→Unconfigured；无错+有成功值→Normal；无错+从未成功→
// Pending；有错+有成功值→FailedStale；有错+从未成功+配置类→Incomplete；其余→FailedNever
QuotaCardState quotaCardState(const std::string& providerId,
                              const std::string& errorKindName,
                              bool hasEverSucceeded);

// 列出全部 profile 的余量展示行，按 profile_id 升序。没有配置行的 profile 也出现（Unconfigured）。
// 只读；任一子查询失败即整体 fail（首页显示「加载失败」，不静默空态）
core::Result<std::vector<QuotaHomeRow>> listQuotaHomeRows(core::data::SqliteDb& db);

// 手动刷新请求：只置一个进程级请求位，不阻塞不发 HTTP；后台刷新线程在 sleep 切片消费，
// 最坏 ~一个切片（默认 200ms）就开跑。不做同步 refreshNow——网络查询最坏 N×15s 会冻住 UI 线程
void requestQuotaImmediateRefresh();

// 消费请求位（后台刷新线程每片调一次；true=本次消费到要立即跑一轮）；独立函数以便断言
bool consumeQuotaImmediateRefreshRequest();

// 真实查库计数（测试缝，生产零调用点）：数 listQuotaHomeRows 被调几次；换路径（改名/内联/转发）
// 静默失效时计数仍有效。语义是全局计数，用例只断言增量
int64_t quotaHomeRowsQueryCountForTest();

// 复位计数（用例开头调）
void quotaHomeResetQueryCountForTest();

// 余量功能总开关（默认 false）。关闭 = 整套余量子系统不可见；数据保留不删。
// 消费方：home compose（首页余量卡）、claude compose（详情余量查询入口）、
// claude onStart（后台刷新线程）、agent 工具暴露（暂无）。
// 实现为 inline 函数共享单一 static atomic（零 link 期依赖，任何模块 include 即可调）。

inline std::atomic<bool>& _quotaFeatureFlag() {
    static std::atomic<bool> flag{false};
    return flag;
}

inline bool isQuotaFeatureEnabled() {
    return _quotaFeatureFlag().load(std::memory_order_relaxed);
}

inline void setQuotaFeatureEnabled(bool enabled) {
    _quotaFeatureFlag().store(enabled, std::memory_order_relaxed);
}

}  // namespace module::claude::quota
