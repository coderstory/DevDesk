// src/module_claude_proxy/quota/quota_home_view.cpp
// 首页余量卡片的只读数据侧（中立桥 quota_home_bridge.h 的实现）。
// 三条纪律：
//   1. 所有句柄操作在 db.mutex() 锁内：全仓只有一把 sqlite 连接且 macOS libsqlite3 THREADSAFE=2
//      （连接内无互斥），后台刷新线程写、UI 线程读，漏锁即真崩（2026-10-02 有 SIGSEGV 前科）。
//   2. 不重写解码：windows_json 走 decodeQuotaWindows，错误枚举走 quotaErrorFromName，文案走
//      quotaErrorHeadline，裁剪走 clampUsedPercent——全部复用既有单点。
//   3. last_error 有错时恒非空：写侧已兜底，读侧再兜一次，让旧行（老行不迁）也显示得出话。
#include "quota_home_bridge.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "quota_error_text.h"
#include "quota_percent.h"
#include "quota_provider.h"
#include "quota_registry.h"
#include "quota_store.h"

namespace module::claude::quota {

// 两份同义枚举的同步闸（编译期）：quota_home_bridge.h 的 QuotaHomeKind 与 quota_provider.h 的 QuotaKind
// 是同一件事的两份枚举，桥头要保持中立（不能 include 供应商层头），漂移在任何头文件里都看不见。
// 两条断言：逐值同义 + 哨兵相等（只逐值比会漏掉「加了第 3 个值忘了改另一处」，那正是注释冒充断言）。
static_assert(static_cast<int>(QuotaKind::Count) ==
                  static_cast<int>(QuotaHomeKind::Count),
              "QuotaKind 与 QuotaHomeKind 的取值个数必须相等：给 quota_provider.h "
              "的 QuotaKind 加了新值而忘了改 quota_home_bridge.h 的 QuotaHomeKind —— "
              "桥层解码出来的形态与渲染层理解的形态会静默错位");
static_assert(static_cast<int>(QuotaKind::Tiered) ==
                  static_cast<int>(QuotaHomeKind::Tiered) &&
                  static_cast<int>(QuotaKind::Balance) ==
                      static_cast<int>(QuotaHomeKind::Balance),
              "QuotaKind 与 QuotaHomeKind 必须逐值同义（不只是个数相等）");

namespace {

// 真实查库计数（测试缝）。原子而非整型：后台刷新线程与 UI 线程同时加减，非原子会丢计数 → 用例误判。
std::atomic<int64_t>& globalQueryCount() {
    static std::atomic<int64_t> count{0};
    return count;
}

// 规范窗口键 → 中文标签（唯一映射）。未知键回落成 key 本身而非空串（空标签会让进度条左侧空一截）。
const char* windowLabelFor(const std::string& key) {
    if (key == "five_hour") return "五小时";
    if (key == "weekly") return "本周";
    if (key == "monthly") return "本月";
    return nullptr;
}

// last_error_kind 列文本 → 「当前有错吗」。空串必须先判为无错：列默认值 '' 含义是「从没写过」，
// 而 quotaErrorFromName("") 回落 Transient，会把它误报成「网络失败」。
bool errorColumnMeansError(const std::string& kindName) {
    return !kindName.empty() && kindName != "none";
}

}  // namespace

QuotaCardState quotaCardState(const std::string& providerId,
                              const std::string& errorKindName,
                              bool hasEverSucceeded) {
    // 优先级即契约，改顺序必须改 test_home_quota_card 的五态用例
    // 1) provider_id 空 = 未配置（哪怕留着历史结果也不画条：那不是有效值）
    if (providerId.empty()) return QuotaCardState::Unconfigured;
    const bool errored = errorColumnMeansError(errorKindName);
    // 2) 无错 + 有过成功值 = 正常。不用 windows 空不空：Balance 型成功时 windows 恒空，会误判成「从没查过」。
    if (!errored && hasEverSucceeded) return QuotaCardState::Normal;
    // 3) 无错 + 从未成功 = 刚配好还没到点
    if (!errored) return QuotaCardState::Pending;
    // 4) 有错 + 有过成功值 = 旧值 + 「可能过期」
    if (hasEverSucceeded) return QuotaCardState::FailedStale;
    // 5) 有错 + 从未成功 + 配置类 = 配置不完整（引导去补全）
    if (quotaErrorFromName(errorKindName) == QuotaError::NotConfigured) {
        return QuotaCardState::Incomplete;
    }
    // 6) 有错 + 从未成功 + 其余 = 只渲染错误行
    return QuotaCardState::FailedNever;
}

core::Result<std::vector<QuotaHomeRow>> listQuotaHomeRows(
    core::data::SqliteDb& db) {
    // 计数 +1 在取锁之前：本函数从头到尾持锁，放锁后则「等锁」那段时间不算数，而用例量的正是「真查了几次库」。
    globalQueryCount().fetch_add(1, std::memory_order_relaxed);
    // 锁从第一行持到返回（不变量 1）。recursive_mutex：下面 QuotaStore 各方法会再取一次，重入安全。
    std::lock_guard<std::recursive_mutex> lock(db.mutex());

    // profile 名（claude_profiles 是 core schema 表，任何模块可直读）
    std::map<int64_t, std::string> names;
    {
        auto st = db.prepare("SELECT id, name FROM claude_profiles ORDER BY id");
        if (st.isFail()) {
            return core::Result<std::vector<QuotaHomeRow>>::fail(
                st.errorCode(), st.errorMessage());
        }
        auto rows = st.value().fetchAll();
        if (rows.isFail()) {
            return core::Result<std::vector<QuotaHomeRow>>::fail(
                rows.errorCode(), rows.errorMessage());
        }
        for (const auto& r : rows.value()) {
            const auto id = r.getInt64("id");
            const auto name = r.getString("name");
            if (id.isOk() && name.isOk()) names[id.value()] = name.value();
        }
    }

    QuotaStore store(db);
    auto configs = store.listConfigs();
    if (configs.isFail()) {
        return core::Result<std::vector<QuotaHomeRow>>::fail(
            configs.errorCode(), configs.errorMessage());
    }
    auto results = store.listResults();
    if (results.isFail()) {
        return core::Result<std::vector<QuotaHomeRow>>::fail(
            results.errorCode(), results.errorMessage());
    }

    std::map<int64_t, QuotaConfigRow> configBy;
    for (const auto& c : configs.value()) configBy[c.profileId] = c;
    std::map<int64_t, QuotaResultRow> resultBy;
    for (const auto& r : results.value()) resultBy[r.profileId] = r;

    std::vector<QuotaHomeRow> out;
    out.reserve(names.size());
    // names 是 std::map → 键序 = profile_id 升序（不变量 2：刷新不重排）
    for (const auto& [profileId, profileName] : names) {
        QuotaHomeRow row;
        row.profileId = profileId;
        row.profileName = profileName;

        const auto cit = configBy.find(profileId);
        if (cit != configBy.end()) row.providerId = cit->second.providerId;

        const auto rit = resultBy.find(profileId);
        bool hasResult = rit != resultBy.end();
        if (hasResult) {
            const QuotaResultRow& src = rit->second;
            row.hasEverSucceeded = src.hasEverSucceeded;
            row.lastSuccessAt = src.lastSuccessAt;
            // 枚举 → 列文本走 quotaErrorName（单点）。中立头只见列文本，判据可用字面量在用例里断言。
            row.lastErrorKind = quotaErrorName(src.lastErrorKind);
            row.hasError = src.hasError();
            row.kind = src.kind == QuotaKind::Balance ? QuotaHomeKind::Balance
                                                      : QuotaHomeKind::Tiered;
            row.balanceRemaining = src.balanceRemaining;
            row.currency = src.currency;
            row.planName = src.planName;
            row.lastError = src.lastError;
            // 按 kind 选文案（不按有没有数据）：headline 按错误分类定，last_error 原作细节接在后面。
            // 故 401 一定显示「密钥无效…」；headline 对非 None 恒非空，旧行也显示得出话。
            if (row.hasError) {
                row.lastError =
                    quotaErrorHeadline(src.lastErrorKind, src.lastError);
            }
            // 窗口已是解码后的内存形态（编解码单点在 quota_store.cpp），这里只补中文标签 + 裁剪。
            // 从未成功时 windows 恒空，铺出来也是空数组，交给 showsQuotaBar 判 false。
            for (const QuotaWindow& w : src.windows) {
                QuotaHomeWindow hw;
                hw.key = w.key;
                const char* label = windowLabelFor(w.key);
                hw.label = label != nullptr ? std::string(label) : w.key;
                hw.usedPercent = clampUsedPercent(w.usedPercent);
                hw.resetsAt = w.resetsAt;
                row.windows.push_back(std::move(hw));
            }
        }

        row.state = quotaCardState(row.providerId, row.lastErrorKind,
                                   row.hasEverSucceeded);
        // 未注册供应商（id 写错）→ 供应商名空，不猜名字；刷新侧会按 NotConfigured 落库，卡片进 Incomplete 态。
        if (!row.providerId.empty()) {
            const QuotaProvider* p = findProvider(row.providerId);
            if (p != nullptr) row.providerDisplayName = p->displayName();
        }
        out.push_back(std::move(row));
    }
    return core::Result<std::vector<QuotaHomeRow>>::ok(std::move(out));
}

int64_t quotaHomeRowsQueryCountForTest() {
    return globalQueryCount().load(std::memory_order_relaxed);
}

void quotaHomeResetQueryCountForTest() {
    globalQueryCount().store(0, std::memory_order_relaxed);
}

}  // namespace module::claude::quota
