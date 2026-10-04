// src/module_claude_proxy/quota/quota_refresh.h
// 余量的后台定时刷新。
// 用真后台线程而非 UI onTimer：onTimer 挂 UI 帧回调，窗口隐藏 / 挂起时整帧不进 compose → tick 静默停止。
// sleep 切片 tick：一轮 = slicesPerRound × sliceMs，片间查 stopFlag_。stopAll() 睡眠态最多等一片（默认 200ms），
//   在途请求态要等那轮跑完（最坏 ≈ N × 15s）；无 std::terminate 风险（析构必 join）。
// 三条不变量：1. 先落库、后就绪唤醒（顺序反了 UI 抢跑消费唤醒位，结果不上屏）；
//   2. 不引入第二把数据库连接；3. 时钟与 tick 间隔可注入（用例不等真 15 分钟）。
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

#include "core/result.h"
#include "quota_credential.h"
#include "quota_provider.h"
#include "quota_store.h"

namespace module::claude::quota {

// 后台扫描间隔的生产默认值——全项目唯一的那个 60。
// 两处消费者共用它：QuotaRefreshOptions::scanIntervalSec 默认值 + 保存后的 toast 文案
//   「后台将在 N 秒内自动查询」。各写一个字面量会「改了 A 忘了 B」。改此值属产品决策，
//   须同步改 test_claude_quota_store 的 production_scan_interval_is_single_source。
inline constexpr int kQuotaProductionScanIntervalSec = 60;

// 一轮刷新的产出（供调用方 / 用例断言「这一轮刷了谁」）。
struct RefreshRoundResult {
    int64_t profileId = 0;
    QuotaError error = QuotaError::None;   // 透出 provider 的原始分类
    bool skipped = false;                  // 未启用 / 未到点 / 未配置 → 没发请求
    std::string skipReason;
    bool persisted = false;                // 结果真落库了
};

// tick 参数（默认值 = 生产值；用例注入毫秒级）。定义在类外：嵌套 struct 做默认实参时 GCC 会报错（clang 不报）。
struct QuotaRefreshOptions {
    // 一轮 = slices 片 × sliceMs 毫秒。停机最多等一片。
    int slicesPerRound = 10;
    int sliceMs = 20;
    // 扫描间隔（秒）：真实 60 秒一轮（取 kQuotaProductionScanIntervalSec，与 toast 文案同源，别另写字面量）；用例设 0 → 每片都扫。
    int scanIntervalSec = kQuotaProductionScanIntervalSec;
};

class QuotaRefresher {
public:
    // 可注入时钟（epoch 秒）。不传 = 真实系统时钟。
    using NowFn = std::function<int64_t()>;

    // providerResolver：provider_id → provider。注入而非直连全局注册表（用例塞假 provider → 零真实网络）；返回 const 指针，见 makeRegistryProviderResolver。
    using ProviderResolver = std::function<const QuotaProvider*(const std::string&)>;

    QuotaRefresher(QuotaStore& store, QuotaCredentialSource& credentials,
                   ProviderResolver resolver, HttpSendFn http,
                   NowFn now = {}, QuotaRefreshOptions options = {});
    ~QuotaRefresher();

    QuotaRefresher(const QuotaRefresher&) = delete;
    QuotaRefresher& operator=(const QuotaRefresher&) = delete;

    // 起后台线程（幂等；已停则不重启，与 Scheduler::start 同口径）。
    void start();
    // 停线程并 join（幂等）。等待上界：睡眠态 ≤ 一个 sleep 片；在途请求态 ≤ 该轮剩余耗时。
    void stopAll();

    // 同步跑一轮（不经线程）。manual = true（首页 [刷新] 按钮）不等 interval、不看 auto_refresh，但仍尊重 enabled 与 provider_id 非空。
    // 返回这一轮每个 profile 的产出（顺序 = profile_id 升序）。
    core::Result<std::vector<RefreshRoundResult>> refreshNow(bool manual = false);

    // 到期判定（可单测）：enabled && auto_refresh && 距上次尝试 >= interval 分钟；lastFetchedAt = 0 恒到期。
    // manual = true 时只保留 enabled 与 provider_id 两道。
    static bool isDue(const QuotaConfigRow& config, int64_t nowSec,
                      int64_t lastFetchedAt, bool manual = false);

    private:
    void tickThreadLoop();
    // 单个 profile 的完整一轮：查配置 → 到点则取凭据查询 → 落库 → 唤醒 UI。
    RefreshRoundResult refreshProfile(int64_t profileId, int64_t nowSec,
                                      bool manual);

    QuotaStore& store_;
    QuotaCredentialSource& credentials_;
    ProviderResolver resolver_;
    HttpSendFn http_;
    NowFn now_;
    QuotaRefreshOptions options_;

    std::atomic<bool> stopFlag_{false};
    std::thread tickThread_;
    // 本类发出的唤醒次数（用例断言「刷完确实请求了 UI 收割」）。计数而非布尔：sawReady 恒为真，只有「发了几次」真会变。
    std::atomic<int64_t> wakeCount_{0};
};

// 生产用：从全局注册表解析 provider 的适配器。空 provider_id / 未命中 → nullptr（调用方按 NotConfigured 走）。
QuotaRefresher::ProviderResolver makeRegistryProviderResolver();

// 进程级缝的读取口（用例用；返回累计唤醒次数）。⚠ 曾有 quotaRefresherWakeSawReadyForTest()，因断言的量恒为 true 已整条删除。
int64_t quotaRefresherWakeCountForTest();
void quotaRefresherResetTestSeams();

}  // namespace module::claude::quota
