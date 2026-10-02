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
// stopFlag_。**stopAll() 的等待上界（评审 m4 更正，别再写成「最多一片」）**：
//   - 线程**睡眠中** → 最多等一片（默认 10 × 20ms = 200ms）
//   - 线程**正在 refreshNow() 里发真实请求** → join 会等到那一轮跑完；
//     各 provider timeoutMs = 15000ms（火山一次查询两次串行请求），N 个
//     到期 profile 最坏 ≈ N × 15s。
// 切片只解决「睡眠态停得下」，**不**给在途 HTTP 设上限。无 std::terminate
// 风险（析构必 join），但调用方要在退出路径上给足时间预算。
//
// 三条本类自带的不变量：
//
//   1. **先就绪、后唤醒**。落库完成 → **读回证明可读** → 才 requestUiUpdate。
//      唤醒跑在结果可读之前，UI 抢跑消费掉唤醒位、脏标记已清 → 结果躺在
//      内存里没人再上屏（批次 D 那个间歇复现的 bug）。本类复用
//      module::claude::wakeAfterPublish（出口内**先探一次真实就绪**再唤醒）。
//      ⚠ 就绪信号由**落库事实**驱动，不是紧挨着唤醒手动 set_value——
//      后者是同线程相邻两行，恒真，断言的只是源码顺序（见 wakeSawReady 注释）。
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
    // 返回 **const 指针**：`fetch()` 已是 const 成员（评审 m2），注册表的
    // const 返回与之不再矛盾，消费方不需要任何 cast。
    using ProviderResolver = std::function<const QuotaProvider*(const std::string&)>;

    QuotaRefresher(QuotaStore& store, QuotaCredentialSource& credentials,
                   ProviderResolver resolver, HttpSendFn http,
                   NowFn now = {}, QuotaRefreshOptions options = {});
    ~QuotaRefresher();

    QuotaRefresher(const QuotaRefresher&) = delete;
    QuotaRefresher& operator=(const QuotaRefresher&) = delete;

    // 起后台线程（幂等；已停则不重启，与 Scheduler::start 同口径）。
    void start();
    // 停线程并 join（幂等）。等待上界：睡眠态 ≤ 一个 sleep 片；**在途请求态
    // ≤ 该轮剩余耗时**（N 个到期 profile 最坏 ≈ N × provider timeoutMs）。
    void stopAll();

    // 同步跑一轮（不经线程）。用例直调它注入时钟；生产只有测试与手动刷新用。
    // 返回这一轮每个 profile 的产出（顺序 = profile_id 升序）。
    core::Result<std::vector<RefreshRoundResult>> refreshNow();

    // 到期判定（可单测的纯逻辑）：enabled && auto_refresh && 距上次尝试
    // >= interval 分钟。lastFetchedAt = 0（从未查过）→ 恒到期。
    static bool isDue(const QuotaConfigRow& config, int64_t nowSec,
                      int64_t lastFetchedAt);

    // 就绪缝：最近一次唤醒发出时，结果是否**已经从库里读得回来**。
    //
    // 就绪信号由**落库事实**驱动（一次真实 `loadResult` 读回），不是紧挨着
    // 唤醒手动 `set_value()`（评审 M1：旧写法两行相邻、无可交错点 → 恒为 true，
    // 断言的其实是「源码里 set_value 写在前面」而不是「结果已就绪」）。
    // 因此它抓的是**落库失败 / 行读不回来**，不是「唤醒跑到了落库前面」——
    // 后者是同线程相邻语句的静态次序，编译器保证，不需要用例来盯。
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
