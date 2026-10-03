// src/module_claude_proxy/quota/quota_refresh.cpp
// 后台定时刷新实现（契约见 quota_refresh.h 头注释，尤其「先就绪、后唤醒」）。
#include "quota_refresh.h"

#include <chrono>
#include <utility>

#include <core/platform/platform.h>   // core::platform::requestUiUpdate

#include "core/logging.h"
#include "module_claude_proxy/claude_ui_logic.h"   // wakeAfterPublish（唯一唤醒出口）
#include "quota_home_bridge.h"   // 手动刷新请求位（中立桥，G7 [刷新] 按钮）
#include "quota_registry.h"

namespace module::claude::quota {

namespace {

int64_t systemNowSec() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

// 进程级次序缝的**计数**（用例读）。为什么是进程级而不是成员：唤醒发生在
// 后台线程、断言发生在用例线程，两者之间没有「同一个对象」的稳定引用可传
// ——批次 D 的 claude_calls_query_wake_saw_ready_for_test() 同款。
// ⚠ 这里**只有计数，没有 sawReady**：见 refreshProfile ② 段的注释——
// 「落库后能不能读回来」在当前实现下恒为真（同一 SqliteDb + 同一把锁 + 中间
// 无写者），是一条结构上不可能变红的量。静态审计 M1 两次变异实测：
// 硬编码成 false（缝永远红）时 quota 167 例**仍然全绿**，故删除而非保留。
std::atomic<int64_t>& globalWakeCount() {
    static std::atomic<int64_t> count{0};
    return count;
}

// 手动刷新请求位（G7）。进程级而非成员级：请求方（首页 UI 线程）与消费方
// （后台 tick 线程）之间没有「同一个对象」的稳定引用可传 —— 与上面两个
// 进程级缝同款理由。静态存储期 = 进程结束即析构，不与刷新器的生命周期赛跑
// （存一个裸 QuotaProvider* 再跨线程解引用才是真的赛跑）。
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

// ⚠ **这里刻意没有「次序缝」**（静态审计 M1 的实测结论，见 refreshProfile ②）。
// 曾经存在一个 quotaRefresherWakeSawReadyForTest()，已被**整条删除**——删函数
// 是有意的：留一个恒返 true 的桩，任何漏改的调用点仍会「断言通过」，
// 即恒真的假守卫原样留在原地。编译器报红才是我们要的。
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
    // 幂等但**不早退**：stopFlag_ 可反复置位，而每次调用都必须 join 干净
    // ——留 joinable thread 即析构时 std::terminate（Scheduler:70-75 同款）。
    stopFlag_.store(true);
    if (tickThread_.joinable()) tickThread_.join();
}

bool QuotaRefresher::isDue(const QuotaConfigRow& config, int64_t nowSec,
                           int64_t lastFetchedAt, bool manual) {
    if (!config.enabled) return false;
    if (config.providerId.empty()) return false;   // 未配置：不该发请求
    // 手动刷新：不等 interval、不看 auto_refresh（手动刷新档的唯一途径就是
    // 它）。仍受上面两道闸——没打开的档、未配置的 profile 不该被强行查询。
    if (manual) return true;
    if (!config.autoRefresh) return false;
    if (lastFetchedAt <= 0) return true;          // 从未查过 → 立即到点
    // interval <= 0 = 「到点即刷」（每次扫描都算到期）。生产写 15；
    // 这条让「手动刷新档」与用例不必造一个极大 interval。
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
    // 距上次**尝试**多久（fetched_at 成败都写）：用它判到期，而不是用配置里的
    // 上次成功时间——否则一次持续失败会每片都重试（重试风暴）。
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
    //    ⚠⚠ **这里没有「次序缝」，且是刻意的**（静态审计 M1 的实测结论）。
    //    G5 曾在这里加过一条缝：读完库证明可读才 set_value，并断言「缝恒为 true」。
    //    两次变异证明它是**恒真的假守卫**：
    //      · 退回无条件 set_value        → quota 167 例全绿
    //      · rowReadable **硬编码 false**（缝永远红）→ **仍然全绿**
    //    原因在结构上：`applyFetchResult` 刚写完行，紧接着 `loadResult` 走的是
    //    同一个 SqliteDb、同一把 recursive_mutex、中间没有别的写者 → **必然**读得回。
    //    这条缝比原来那条「恒真 set_value」还隐蔽，因为它的注释明确声称自己不是恒真的。
    //    故**整条删除**（连同 wakeSawReady_ / globalWakeSawReady() /
    //    quotaRefresherWakeSawReadyForTest() 与那条断言）。
    //
    //    「先落库后唤醒」由**同一线程的语句次序**保证：上两行 `applyFetchResult`
    //    是 return-before-wake 的直线代码，中间没有分支能让唤醒跑到落库前面，
    //    编译器也不允许重排有 data dependence 的语句。这里**没有可交错的点**，
    //    因此不需要缝，也写不出让缝变红的场景。
    //
    //    批次 D 那个 bug 的真正成因不是「次序」，而是**落库失败路径仍在发唤醒**
    //    ——那一支由上面的 `if (persisted.isFail()) return out;`（**不发唤醒**）
    //    单独覆盖，且**有**测试：`applyFetchResult` 失败时推进 lastSuccessAt
    //    那条变异实测会让 quota 用例红（静态审计变异 6）。
    auto promise = std::make_shared<std::promise<void>>();
    std::shared_future<void> published = promise->get_future().share();
    promise->set_value();
    std::atomic<bool> sawReady{false};
    wakeAfterPublish(published, sawReady);
    // sawReady 在本实现下恒为 true（就绪信号与唤醒同线程相邻发出），**不断言它**
    // —— 断言一个结构上不可能为假的量，就是造一条恒真的假守卫。
    // 保留这次 store 是因为 wakeCount_ 旁边需要它成对，且它是 wakeAfterPublish
    // 的必要入参。
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
    // 首轮标记用**独立的 bool**，不用 `lastScanSec != INT64_MIN` 特判（静态审计
    // m2）：now_ 是可注入的（用例能返回任意值），一旦它恰好返回 INT64_MIN，
    // 首轮会被误判成「已扫过」→ 整首轮被 continue 跳过。用哨兵值表示「没有
    // 上一轮时间」在时钟可注入时是个陷阱：那个值在取值域之内。
    bool hasScanned = false;
    int64_t lastScanSec = 0;
    try {
        while (!stopFlag_.load()) {
            // sleep 切片：每片之间查 stopFlag_ → stopAll 最多等一片（~20ms），
            // 不是「一次睡到下一轮」（那会让退出等最多 scanIntervalSec）。
            //
            // G7 手动刷新：每片**先**查一次请求位再睡，置位后最坏等一片
            //（默认 10×20ms = 200ms）就开跑一轮，不必等 scanIntervalSec 的
            // 扫描节流。请求位只在本文件消费一次（exchange=false），重复点
            // 两次不会连刷两轮。
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
            // 扫描节流：两次扫描之间至少隔 scanIntervalSec。注入时钟设 0 时
            // 每片都扫（用例用），生产 60 秒一轮。手动刷新**不过节流**。
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
    // 直接薄包一层，**零 cast**（评审 m2）：`fetch()` 已是 const 成员
    // （quota_provider.h），而 findProvider 返回 `const QuotaProvider*`——
    // 两者不再矛盾。曾经这里有个 const_cast，理由写的是「G0 用例钉死了 const
    // 返回」，那是事实错误（评审实测零钉死，去掉 const 照常编译）；而且
    // `create` 是裸函数指针、类型系统**不保证**工厂返回单例，const_cast 会把
    // 「返回的不是单例」这个类型错误洗成运行时 UB。
    return [](const std::string& id) { return findProvider(id); };
}

}  // namespace module::claude::quota
