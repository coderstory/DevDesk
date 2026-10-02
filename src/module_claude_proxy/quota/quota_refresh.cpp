// src/module_claude_proxy/quota/quota_refresh.cpp
// 后台定时刷新实现（契约见 quota_refresh.h 头注释，尤其「先就绪、后唤醒」）。
#include "quota_refresh.h"

#include <chrono>
#include <limits>
#include <utility>

#include <core/platform/platform.h>   // core::platform::requestUiUpdate

#include "core/logging.h"
#include "module_claude_proxy/claude_ui_logic.h"   // wakeAfterPublish（唯一唤醒出口）
#include "quota_registry.h"

namespace module::claude::quota {

namespace {

int64_t systemNowSec() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

// 进程级次序缝（用例读）。为什么是进程级而不是成员：唤醒发生在后台线程、
// 断言发生在用例线程，两者之间没有「同一个对象」的稳定引用可传——批次 D
// 的 claude_calls_query_wake_saw_ready_for_test() 同款。
std::atomic<bool>& globalWakeSawReady() {
    static std::atomic<bool> flag{false};
    return flag;
}
std::atomic<int64_t>& globalWakeCount() {
    static std::atomic<int64_t> count{0};
    return count;
}

}  // namespace

bool quotaRefresherWakeSawReadyForTest() {
    return globalWakeSawReady().load(std::memory_order_relaxed);
}
int64_t quotaRefresherWakeCountForTest() {
    return globalWakeCount().load(std::memory_order_relaxed);
}
void quotaRefresherResetTestSeams() {
    globalWakeSawReady().store(false, std::memory_order_relaxed);
    globalWakeCount().store(0, std::memory_order_relaxed);
}

QuotaRefresher::QuotaRefresher(QuotaStore& store,
                               QuotaCredentialSource& credentials,
                               ProviderResolver resolver, HttpSendFn http,
                               NowFn now, QuotaRefreshOptions options)
    : store_(store),
      credentials_(credentials),
      resolver_(std::move(resolver)),
      http_(std::move(http)),
      now_(std::move(now)),
      options_(options) {}

QuotaRefresher::~QuotaRefresher() {
    stopAll();   // 兜底幂等：调用方漏调也不会留下 joinable thread（terminate）
}

void QuotaRefresher::start() {
    if (stopFlag_.load()) return;   // 已停：不重启（Scheduler 同口径）
    try {
        if (!tickThread_.joinable()) {
            tickThread_ = std::thread([this] { tickThreadLoop(); });
        }
    } catch (const std::system_error& e) {
        LOG_ERROR(std::string("QuotaRefresher: tick 线程创建失败: ") + e.what());
    }
}

void QuotaRefresher::stopAll() {
    // 幂等但**不早退**：stopFlag_ 可反复置位，而每次调用都必须 join 干净
    // ——留 joinable thread 即析构时 std::terminate（Scheduler:70-75 同款）。
    stopFlag_.store(true);
    if (tickThread_.joinable()) tickThread_.join();
}

bool QuotaRefresher::isDue(const QuotaConfigRow& config, int64_t nowSec,
                           int64_t lastFetchedAt) {
    if (!config.enabled) return false;
    if (!config.autoRefresh) return false;
    if (config.providerId.empty()) return false;   // 未配置：不该发请求
    if (lastFetchedAt <= 0) return true;          // 从未查过 → 立即到点
    // interval <= 0 = 「到点即刷」（每次扫描都算到期）。生产写 15；
    // 这条让「手动刷新档」与用例不必造一个极大 interval。
    const int64_t intervalSec =
        config.intervalMinutes > 0
            ? static_cast<int64_t>(config.intervalMinutes) * 60
            : 0;
    return nowSec - lastFetchedAt >= intervalSec;
}

RefreshRoundResult QuotaRefresher::refreshProfile(int64_t profileId, int64_t nowSec) {
    RefreshRoundResult out;
    out.profileId = profileId;

    auto cfg = store_.loadConfig(profileId);
    if (cfg.isFail()) {
        out.skipped = true;
        out.skipReason = "未配置余量查询";
        return out;
    }
    // 距上次**尝试**多久（fetched_at 成败都写）：用它判到期，而不是用配置里的
    // 上次成功时间——否则一次持续失败会每片都重试（重试风暴）。
    int64_t lastFetchedAt = 0;
    auto prev = store_.loadResult(profileId);
    if (prev.isOk()) lastFetchedAt = prev.value().fetchedAt;

    if (!isDue(cfg.value(), nowSec, lastFetchedAt)) {
        out.skipped = true;
        out.skipReason = "未启用 / 未到点";
        return out;
    }

    const QuotaProvider* provider = resolver_ ? resolver_(cfg.value().providerId)
                                              : nullptr;
    if (provider == nullptr) {
        // 未注册的 provider：按 NotConfigured 落库（G7 引导「去配置」），
        // 不静默跳过——否则用户配了个写错的 id 却什么都看不到。
        QuotaResult r;
        r.error = QuotaError::NotConfigured;
        r.errorMessage = "未注册的余量供应商：" + cfg.value().providerId;
        out.error = QuotaError::NotConfigured;
        out.persisted = store_.applyFetchResult(profileId, r, nowSec).isOk();
        return out;
    }

    QuotaResult result = credentials_.resolveAndFetch(profileId, *provider, http_);
    out.error = result.error;

    // ① **先落库**（结果落到共享状态 = DB 行已可读）。落库失败 → 不唤醒
    //    （唤醒了 UI 也读不到东西，白白多一帧）。
    auto persisted = store_.applyFetchResult(profileId, result, nowSec);
    if (persisted.isFail()) {
        LOG_WARN("QuotaRefresher: 落库失败: " + persisted.errorMessage());
        return out;
    }
    out.persisted = true;

    // ② **就绪之后才唤醒**（唯一出口，内部先探一次真实就绪再发）。
    //    批次 D 评审定的 major：次序反了 = UI 抢跑消费唤醒位 → 结果不上屏。
    //
    //    ⚠ 就绪信号必须**由落库事实驱动**，不能是「紧挨着唤醒手动 set_value()」
    //    （评审 M1）。旧写法是同一线程相邻两行 `set_value()` → `wakeAfterPublish`，
    //    中间无可交错点 → `sawReady` 恒为 true，那条断言断言的是「源码里
    //    set_value 写在前面」而不是「结果已就绪」，是恒真的假守卫；头注释还
    //    声明它「不是抄源码顺序」，比断言本身更误导。
    //    现在改成：**用一次真实读回证明这一行可读**，才据此 set_value。
    //    于是「DB 写坏了 / 行读不回来」会让缝真的变红。
    auto promise = std::make_shared<std::promise<void>>();
    std::shared_future<void> published = promise->get_future().share();
    // 读回证明：只有真能从库里读出这一行，才发布「已就绪」。
    const bool rowReadable = store_.loadResult(profileId).isOk();
    if (rowReadable) {
        promise->set_value();
    }
    std::atomic<bool> sawReady{false};
    wakeAfterPublish(published, sawReady);
    if (!rowReadable) {
        // 读不回就绪信号 → 缝停在 false，用例判红。仍要唤醒：UI 收割时至少
        // 能看到 last_error（错误文案比旧值更重要）。
        LOG_WARN("QuotaRefresher: 落库后读不回该行，UI 收割将读到旧值: profile=" +
                 std::to_string(profileId));
    }
    wakeSawReady_.store(sawReady.load(std::memory_order_relaxed),
                        std::memory_order_relaxed);
    globalWakeSawReady().store(sawReady.load(std::memory_order_relaxed),
                               std::memory_order_relaxed);
    globalWakeCount().fetch_add(1, std::memory_order_relaxed);
    wakeCount_.fetch_add(1, std::memory_order_relaxed);
    return out;
}

core::Result<std::vector<RefreshRoundResult>> QuotaRefresher::refreshNow() {
    auto configs = store_.listConfigs();
    if (configs.isFail()) {
        return core::Result<std::vector<RefreshRoundResult>>::fail(
            configs.errorCode(), configs.errorMessage());
    }
    const int64_t nowSec = now_ ? now_() : systemNowSec();
    std::vector<RefreshRoundResult> out;
    out.reserve(configs.value().size());
    for (const QuotaConfigRow& cfg : configs.value()) {
        try {
            out.push_back(refreshProfile(cfg.profileId, nowSec));
        } catch (const std::exception& e) {
            // 单个 profile 的意外不许中断整轮（其余 profile 的结果照样要刷）
            LOG_ERROR(std::string("QuotaRefresher: 单 profile 刷新异常: ") + e.what());
            RefreshRoundResult r;
            r.profileId = cfg.profileId;
            r.skipped = true;
            r.skipReason = "刷新异常";
            out.push_back(std::move(r));
        }
    }
    return core::Result<std::vector<RefreshRoundResult>>::ok(std::move(out));
}

void QuotaRefresher::tickThreadLoop() {
    int64_t lastScanSec = std::numeric_limits<int64_t>::min();
    try {
        while (!stopFlag_.load()) {
            // sleep 切片：每片之间查 stopFlag_ → stopAll 最多等一片（~20ms），
            // 不是「一次睡到下一轮」（那会让退出等最多 scanIntervalSec）。
            for (int i = 0; i < options_.slicesPerRound && !stopFlag_.load(); ++i) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(options_.sliceMs));
            }
            if (stopFlag_.load()) break;

            const int64_t nowSec = now_ ? now_() : systemNowSec();
            // 扫描节流：两次扫描之间至少隔 scanIntervalSec。注入时钟设 0 时
            // 每片都扫（用例用），生产 60 秒一轮。
            if (options_.scanIntervalSec > 0 &&
                lastScanSec != std::numeric_limits<int64_t>::min() &&
                nowSec - lastScanSec < options_.scanIntervalSec) {
                continue;
            }
            lastScanSec = nowSec;

            auto refreshed = refreshNow();
            if (refreshed.isFail()) {
                LOG_WARN("QuotaRefresher: 刷新轮失败: " + refreshed.errorMessage());
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR(std::string("QuotaRefresher: tick 线程异常退出: ") + e.what());
    } catch (...) {
        LOG_ERROR("QuotaRefresher: tick 线程未知异常退出");
    }
}

QuotaRefresher::ProviderResolver makeRegistryProviderResolver() {
    // 直接薄包一层，**零 cast**（评审 m2）：`fetch()` 已是 const 成员
    // （quota_provider.h），而 findProvider 返回 `const QuotaProvider*`——
    // 两者不再矛盾。曾经这里有个 const_cast，理由写的是「G0 用例钉死了 const
    // 返回」，那是事实错误（评审实测零钉死，去掉 const 照常编译）；而且
    // `create` 是裸函数指针、类型系统**不保证**工厂返回单例，const_cast 会把
    // 「返回的不是单例」这个类型错误洗成运行时 UB。
    return [](const std::string& id) { return findProvider(id); };
}

}  // namespace module::claude::quota
