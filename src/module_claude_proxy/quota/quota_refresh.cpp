// src/module_claude_proxy/quota/quota_refresh.cpp
// 后台定时刷新实现（契约见 quota_refresh.h 头注释，尤其「先就绪、后唤醒」）。
#include "quota_refresh.h"

#include <chrono>
#include <exception>
#include <memory>
#include <system_error>
#include <utility>

#include <core/platform/platform.h>   // core::platform::requestUiUpdate

#include "core/logging.h"
#include "module_claude_proxy/claude_ui_logic.h"   // wakeAfterPublish（唯一唤醒出口）
#include "quota_home_bridge.h"   // 手动刷新请求位（中立桥，[刷新] 按钮）
#include "quota_registry.h"

namespace module::claude::quota {

namespace {

int64_t systemNowSec() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

// 进程级唤醒计数缝（用例读）：唤醒在后台线程、断言在用例线程，没有稳定对象引用可传。
std::atomic<int64_t>& globalWakeCount() {
    static std::atomic<int64_t> count{0};
    return count;
}

// 手动刷新请求位。进程级而非成员级：请求方（UI 线程）与消费方（tick 线程）间没有稳定对象引用可传。
// 静态存储期 = 进程结束即析构，不与刷新器生命周期赛跑（存裸指针跨线程解引用才是真的赛跑）。
std::atomic<bool>& globalManualRefreshRequest() {
    static std::atomic<bool> flag{false};
    return flag;
}

}  // namespace

void requestQuotaImmediateRefresh() {
    globalManualRefreshRequest().store(true, std::memory_order_release);
}

bool consumeQuotaImmediateRefreshRequest() {
    return globalManualRefreshRequest().exchange(false,
                                                 std::memory_order_acquire);
}

int64_t quotaRefresherWakeCountForTest() {
    return globalWakeCount().load(std::memory_order_relaxed);
}
void quotaRefresherResetTestSeams() {
    globalWakeCount().store(0, std::memory_order_relaxed);
    globalManualRefreshRequest().store(false, std::memory_order_relaxed);
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
    // 幂等但不早退：stopFlag_ 可反复置位，每次调用都必须 join 干净——留 joinable thread 即析构时 std::terminate。
    stopFlag_.store(true);
    if (tickThread_.joinable()) tickThread_.join();
}

bool QuotaRefresher::isDue(const QuotaConfigRow& config, int64_t nowSec,
                           int64_t lastFetchedAt, bool manual) {
    if (!config.enabled) return false;
    if (config.providerId.empty()) return false;   // 未配置：不该发请求
    // 手动刷新：不等 interval、不看 auto_refresh，但仍受上面两道闸（没打开的档 / 未配置的 profile 不强行查）。
    if (manual) return true;
    if (!config.autoRefresh) return false;
    if (lastFetchedAt <= 0) return true;          // 从未查过 → 立即到点
    // interval <= 0 = 到点即刷（每次扫描都算到期）；生产写 15，用例不必造极大 interval。
    const int64_t intervalSec =
        config.intervalMinutes > 0
            ? static_cast<int64_t>(config.intervalMinutes) * 60
            : 0;
    return nowSec - lastFetchedAt >= intervalSec;
}

RefreshRoundResult QuotaRefresher::refreshProfile(int64_t profileId,
                                                  int64_t nowSec, bool manual) {
    RefreshRoundResult out;
    out.profileId = profileId;

    auto cfg = store_.loadConfig(profileId);
    if (cfg.isFail()) {
        out.skipped = true;
        out.skipReason = "未配置余量查询";
        return out;
    }
    // 距上次尝试多久（fetched_at 成败都写）：用它判到期而非上次成功时间，否则持续失败会每片重试（重试风暴）。
    int64_t lastFetchedAt = 0;
    auto prev = store_.loadResult(profileId);
    if (prev.isOk()) lastFetchedAt = prev.value().fetchedAt;

    if (!isDue(cfg.value(), nowSec, lastFetchedAt, manual)) {
        out.skipped = true;
        out.skipReason = manual ? "已关闭自动刷新且未开手动" : "未启用 / 未到点";
        return out;
    }

    const QuotaProvider* provider = resolver_ ? resolver_(cfg.value().providerId)
                                              : nullptr;
    if (provider == nullptr) {
        // 未注册的 provider：按 NotConfigured 落库（引导去配置），不静默跳过。
        QuotaResult r;
        r.error = QuotaError::NotConfigured;
        r.errorMessage = "未注册的余量供应商：" + cfg.value().providerId;
        out.error = QuotaError::NotConfigured;
        out.persisted = store_.applyFetchResult(profileId, r, nowSec).isOk();
        return out;
    }

    QuotaResult result = credentials_.resolveAndFetch(profileId, *provider, http_);
    out.error = result.error;

    // ① 先落库（DB 行已可读）。落库失败 → 不唤醒（唤醒了也读不到东西）。
    auto persisted = store_.applyFetchResult(profileId, result, nowSec);
    if (persisted.isFail()) {
        LOG_WARN("QuotaRefresher: 落库失败: " + persisted.errorMessage());
        return out;
    }
    out.persisted = true;

    // ② 就绪之后才唤醒（唯一出口）。次序反了 = UI 抢跑消费唤醒位 → 结果不上屏。
    auto promise = std::make_shared<std::promise<void>>();
    std::shared_future<void> published = promise->get_future().share();
    promise->set_value();
    std::atomic<bool> sawReady{false};
    wakeAfterPublish(published, sawReady);
    // sawReady 在本实现下恒为 true，不断言；仅因是 wakeAfterPublish 的必要入参而保留。
    (void)sawReady;
    globalWakeCount().fetch_add(1, std::memory_order_relaxed);
    wakeCount_.fetch_add(1, std::memory_order_relaxed);
    return out;
}

core::Result<std::vector<RefreshRoundResult>> QuotaRefresher::refreshNow(
    bool manual) {
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
            out.push_back(refreshProfile(cfg.profileId, nowSec, manual));
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
    // 首轮标记用独立 bool，不用 lastScanSec != INT64_MIN：now_ 可注入，一旦恰好返回 INT64_MIN，首轮会被误判成「已扫过」而跳过。
    bool hasScanned = false;
    int64_t lastScanSec = 0;
    try {
        while (!stopFlag_.load()) {
            // sleep 切片：每片间查 stopFlag_ → stopAll 最多等一片（~20ms），而非一次睡到下一轮。
            // 手动刷新：每片先查请求位再睡，置位后最坏等一片（默认 200ms）就开跑，不必等扫描节流；
            // 请求位只消费一次（exchange=false），重复点两次不会连刷两轮。
            bool manual = false;
            for (int i = 0; i < options_.slicesPerRound && !stopFlag_.load(); ++i) {
                if (consumeQuotaImmediateRefreshRequest()) {
                    manual = true;
                    break;
                }
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(options_.sliceMs));
            }
            if (stopFlag_.load()) break;

            const int64_t nowSec = now_ ? now_() : systemNowSec();
            // 扫描节流：两次扫描至少隔 scanIntervalSec（注入时钟设 0 时每片都扫，生产 60s 一轮）；手动刷新不过节流。
            if (!manual && hasScanned && options_.scanIntervalSec > 0 &&
                nowSec - lastScanSec < options_.scanIntervalSec) {
                continue;
            }
            lastScanSec = nowSec;
            hasScanned = true;

            auto refreshed = refreshNow(manual);
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
    // 薄包一层、零 cast：fetch() 已是 const 成员，findProvider 的 const 返回不再矛盾；const_cast 会把「返回的不是单例」洗成运行时 UB。
    return [](const std::string& id) { return findProvider(id); };
}

}  // namespace module::claude::quota
