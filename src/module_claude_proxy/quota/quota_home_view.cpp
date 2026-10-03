// src/module_claude_proxy/quota/quota_home_view.cpp
// G7：首页余量卡片的**只读数据侧**（中立桥 src/quota_home_bridge.h 的实现）
//
// 实现纪律（三条，都用代码保证而不是注释保证）：
//
//   1. **所有句柄操作都在 QuotaStore / db.mutex() 的锁内**。本仓只有一把
//      sqlite 连接且 macOS libsqlite3 是 THREADSAFE=2（连接内无自家互斥），
//      跨线程并发调用直接损坏连接（2026-10-02 真机 SIGSEGV 前科）。后台刷新
//      线程正在写这两张表，首页在 UI 线程读 —— 这里漏锁就是真崩。
//   2. **不重写解码**。windows_json 走 `decodeQuotaWindows`（编解码单点，
//      quota_store.h 不变量 1），错误枚举走 `quotaErrorFromName`，文案走
//      `quotaErrorHeadline`，百分比裁剪走 `clampUsedPercent` —— 全部复用
//      既有单点，首页侧零副本。
//   3. **last_error 恒非空**（有错时）。写侧 quota_store.cpp 已兜底；这里读侧
//      再兜一次，让本兜底之前落下的旧行（无迁移纪律：老行不迁）也显示得出话。
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

// ---- 两份同义枚举的同步闸（编译期，静态审计 m1）-------------------
// quota_home_bridge.h 的 QuotaHomeKind 与 quota_provider.h 的 QuotaKind 是
// 同一件事的两份枚举。桥头要保持中立（不能 include 供应商层头，那会把
// platform/http_client.h 拖进 module_home），所以「加一个 QuotaKind 值忘了
// 改 QuotaHomeKind」这条漂移在**任何头文件里都看不见** —— 两边都能编译、
// 都能显示，只是慢慢不一致，且没有任何机制会告诉你（这正是中立头文件头注释
// 里点名要避免的「两份真值源」）。
//
// 这里把漂移变成**编译期红**：加一个枚举值而忘了另一处 → 编译直接失败，
// 强迫人当场改另一处并想清楚为什么。放在本 .cpp 而不是桥头，是因为只有这里
// 两个枚举都可见；写在桥头只能写出一句自说自话的话。
static_assert(static_cast<int>(QuotaKind::Tiered) ==
                  static_cast<int>(QuotaHomeKind::Tiered) &&
                  static_cast<int>(QuotaKind::Balance) ==
                      static_cast<int>(QuotaHomeKind::Balance),
              "QuotaKind 与 QuotaHomeKind 必须逐值同义：新增枚举值时两处都要改，"
              "否则桥层解码出来的形态与渲染层理解的形态会静默错位");
static_assert(static_cast<int>(QuotaKind::Balance) == 1,
              "QuotaKind 新增值时，同步更新上面那条 static_assert 与 "
              "quota_home_bridge.h 的 QuotaHomeKind（本断言是它的哨兵）");

namespace {

// 真实查库计数（测试缝，见 quota_home_bridge.h 的声明处）。
// 原子而非普通整型：它会被后台刷新线程与 UI 线程同时加减（刷新器落库后
// 首页可能同帧重排），非原子会丢计数 → 用例读到偏小的增量而误判。
std::atomic<int64_t>& globalQueryCount() {
    static std::atomic<int64_t> count{0};
    return count;
}

// 规范窗口键 → 中文标签。**未知键回落成 key 本身**（不返回空串：空标签会
// 让进度条左侧空一截，用户看不出这条是什么周期）。键的取值只有三个
// （quota_provider.h 不变量），这里是它们到中文的唯一映射。
const char* windowLabelFor(const std::string& key) {
    if (key == "five_hour") return "五小时";
    if (key == "weekly") return "本周";
    if (key == "monthly") return "本月";
    return nullptr;
}

// last_error_kind 列文本 → 「当前有错吗」。
// ⚠ 空串**必须**判为无错：列默认值是 ''，含义是「这行从没写过」，而
// `quotaErrorFromName("")` 按设计回落成 Transient（认不出的错误不能当没错）。
// 这里在**列文本层面**先判掉空串，避免把「从没写过」误报成「网络失败」。
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
    // 2) 无错 + 有过成功值（kind 列非空）= 正常。**不用 windows 空不空**：
    //    Balance 型成功时 windows 恒空，拿它当判据会把「余额查到了」误判成
    //    「从没查过」（G5 裁决 #6）。
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
    // 计数 +1 在**取锁之前**：本函数从头到尾持锁，计数若放在锁后，
    // 「调用者等锁」的那段时间就不算数，而用例量的正是「真查了几次库」。
    globalQueryCount().fetch_add(1, std::memory_order_relaxed);
    // 锁从第一行持到函数返回（不变量 1）。recursive_mutex：下面 QuotaStore 的
    // 各方法会再取一次，重入安全。
    std::lock_guard<std::recursive_mutex> lock(db.mutex());

    // ---- profile 名（claude_profiles 是 core schema 表，任何模块可直读；
    //      与 HomeService 读 claude_call_logs 同款先例）----
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
            // 枚举 → 列文本走 quotaErrorName（单点映射，quota_store.h）。中立头
            // 只见列文本不见 QuotaError，判据因此可以用**字面量**在用例里断言。
            row.lastErrorKind = quotaErrorName(src.lastErrorKind);
            row.hasError = src.hasError();
            row.kind = src.kind == QuotaKind::Balance ? QuotaHomeKind::Balance
                                                      : QuotaHomeKind::Tiered;
            row.balanceRemaining = src.balanceRemaining;
            row.currency = src.currency;
            row.planName = src.planName;
            row.lastError = src.lastError;
            // **按 kind 选文案**（spec §5.1「呈现必须按 last_error_kind 选文案，
            // 不能按有没有数据」）——headline 是按错误分类定的那一句，
            // last_error 列是供应商原文，当细节接在后面（quotaTestMessage
            // 同款口径）。所以 401 一定显示「密钥无效…」，不会退化成
            // 「可能过期」这种与错误无关的话。
            // ⚠ 顺带保证非空：headline 对每个非 None 的 kind 都非空，
            // 于是**兜底之前落下的旧行**（last_error 列为空）也显示得出话。
            if (row.hasError) {
                row.lastError =
                    quotaErrorHeadline(src.lastErrorKind, src.lastError);
            }
            // 窗口**已经是解码后的内存形态**（QuotaStore 的行结构里存的就是
            // `std::vector<QuotaWindow>`，编解码单点在 quota_store.cpp）——
            // 这里既不重解码也不重裁剪之外的处理，只做「补中文标签 + 裁剪」。
            // ⚠ 只在「有过成功值」时才铺窗口：从未成功时 windows 恒空，
            // 铺出来也是空数组，交给 `showsQuotaBar` 判 false 即可。
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
        // 未注册供应商（配了个写错的 id）→ 供应商名空。刷新侧会把它按
        // NotConfigured 落库，卡片随即进 Incomplete 态；此处不猜名字。
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