// src/module_claude_proxy/quota/quota_refresh.h
// 余量的**后台定时刷新**（G5）
// （实施计划 §G5；选型依据 spec 2026-10-02-provider-quota-and-storage §5.3
//  「定时器选型（审计 F12）」）
//
// 为什么是真后台线程、不是 UI onTimer（审计 F12 的原话，本批的核心约束）：
// onTimer 挂在 UI 帧回调上，窗口隐藏 / 挂起时整帧不进 compose → tick 静默
// 停止。用户对「定时自动」的预期是「放着不管也会更新」，用 onTimer 实现出来
// 的是「我盯着屏幕它才更新」——而且这个退化**没有任何报错**。
// 故照 module_agent/scheduler.cpp:128-143 的 sleep 切片 tick 范式：
// 一轮 = kTickSlicesPerRound 个 kTickSleepSliceMs 的小睡片，每片之间查
// stopFlag_，于是 stopAll() 最多等一片（~200ms）就能 join，不像「一次睡到底」
// 那样停机要等整个周期。
//
// 三条本类自带的不变量：
//
//   1. **先就绪、后唤醒**。落库完成 → 结果落到**共享状态** → 才 requestUiUpdate。
//      次序反了就是批次 D 那个间歇复现的 bug：UI 抢跑消费掉唤醒位、脏标记已清
//      → 结果躺在内存里没人再上屏。本类复用 module::claude::wakeAfterPublish
//      （那个函数出口内**先探一次真实就绪**再唤醒），并留一条次序缝断言给用例。
//
//   2. **不引入第二把数据库连接**。构造收 QuotaStore&（它内部持全进程唯一那把
//      SqliteDb&），本类自己不碰句柄、只经 store 走。
//
//   3. **时钟可注入**（NowFn）+ **tick 间隔可注入**：用例把时钟拨到「到点」、
//      把间隔设成毫秒级，**绝不等真 15 分钟**。
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

// 一轮刷新的产出（供调用方 / 用例断言「这一轮刷了谁」）。
struct RefreshRoundResult {
    int64_t profileId = 0;
    QuotaError error = QuotaError::None;   // 透出 provider 的原始分类
    bool skipped = false;                  // 未启用 / 未到点 / 未配置 → 没发请求
    std::string skipReason;
    bool persisted = false;                // 结果真落库了
};

// tick 参数（默认值 = 生产值；用例注入毫秒级，见头注释不变量 3）。
//
// 定义在类**外面**（不是嵌套 struct）：嵌套类型在类内做默认实参时，
// GCC 会因「default member initializer needed within definition of enclosing
// class」报错（clang 不报 → 换编译器就炸的那种）。提到命名空间级最省。
struct QuotaRefreshOptions {
    // 一轮 = slices 片 × sliceMs 毫秒。停机最多等一片。
    int slicesPerRound = 10;
    int sliceMs = 20;
    // 扫描间隔（秒）：两次扫配置之间的最小间隔。真实 60 秒一轮；
    // 用例设 0 → 每片都扫。
    int scanIntervalSec = 60;
};

class QuotaRefresher {
public:
    // 可注入时钟（epoch 秒）。不传 = 真实系统时钟。
    using NowFn = std::function<int64_t()>;

    // providerResolver：provider_id → provider。**注入而非直连全局注册表**：
    // 用例塞假 provider（零真实网络 + 可断言调用次数），生产传
    // findProvider 的适配器（见 makeRegistryProviderResolver）。
    using ProviderResolver = std::function<QuotaProvider*(const std::string&)>;

    QuotaRefresher(QuotaStore& store, QuotaCredentialSource& credentials,
                   ProviderResolver resolver, HttpSendFn http,
                   NowFn now = {}, QuotaRefreshOptions options = {});
    ~QuotaRefresher();

    QuotaRefresher(const QuotaRefresher&) = delete;
    QuotaRefresher& operator=(const QuotaRefresher&) = delete;

    // 起后台线程（幂等；已停则不重启，与 Scheduler::start 同口径）。
    void start();
    // 停线程并 join（幂等）。**不挂死**：最多等一个 sleep 片。
    void stopAll();

    // 同步跑一轮（不经线程）。用例直调它注入时钟；生产只有测试与手动刷新用。
    // 返回这一轮每个 profile 的产出（顺序 = profile_id 升序）。
    core::Result<std::vector<RefreshRoundResult>> refreshNow();

    // 到期判定（可单测的纯逻辑）：enabled && auto_refresh && 距上次尝试
    // >= interval 分钟。lastFetchedAt = 0（从未查过）→ 恒到期。
    static bool isDue(const QuotaConfigRow& config, int64_t nowSec,
                      int64_t lastFetchedAt);

    // 次序缝：最近一次唤醒发出时，结果是否**已经就绪**（true = 先就绪后唤醒）。
    // 用例在唤醒那一刻读它，不是抄源码顺序（抄顺序的断言恒真）。
    bool wakeSawReady() const { return wakeSawReady_.load(std::memory_order_relaxed); }

private:
    void tickThreadLoop();
    // 单个 profile 的完整一轮：查配置 → 到点则取凭据查询 → 落库 → 唤醒 UI。
    RefreshRoundResult refreshProfile(int64_t profileId, int64_t nowSec);

    QuotaStore& store_;
    QuotaCredentialSource& credentials_;
    ProviderResolver resolver_;
    HttpSendFn http_;
    NowFn now_;
    QuotaRefreshOptions options_;

    std::atomic<bool> stopFlag_{false};
    std::thread tickThread_;
    // 本类自己发出的唤醒次数（用例用来断言「刷完确实请求了 UI 收割」，
    // 而不是靠读源码顺序）。
    std::atomic<int64_t> wakeCount_{0};
    std::atomic<bool> wakeSawReady_{false};
};

// 生产用：从全局注册表解析 provider 的适配器（findProvider 的薄包装）。
// 空 provider_id / 未命中 → nullptr（调用方按 NotConfigured 走）。
QuotaRefresher::ProviderResolver makeRegistryProviderResolver();

// 次序缝的进程级读取口（用例用；返回最近一次唤醒是否见到已就绪的结果）。
bool quotaRefresherWakeSawReadyForTest();
int64_t quotaRefresherWakeCountForTest();
void quotaRefresherResetTestSeams();

}  // namespace module::claude::quota
