// src/module_schedule/channel_registry.h
// 调度通知渠道层：统一 Channel::deliver(Delivery) -> Result<Receipt> 抽象 +
// 注册表按 channel_ids[] 分发（§4.9 渠道表）。渠道不得反向依赖具体动作——
// action 层只按 id 选渠道，渠道不认识任何动作类型。
//
// 并发契约（所有渠道与注册表共同遵守）：
//   * deliver() 可从任意线程调用（调度引擎/工作线程直调）。触 UI 的渠道
//     （全屏提醒、应用内 toast）deliver 只入队，绝不跨线程触 UI。
//   * 宿主主线程泵（ticker onTick）每 tick 调用 ChannelRegistry::
//     flushAllToMainThread()，这是入队内容唯一落 UI 的时机；该函数只许
//     在主线程调用。
//   * OS 通知渠道无队列：platform::notify::send 自身任意线程安全
//     （facade 契约），直接透传。
//   * 静默渠道无状态、无副作用。
//   * 注册表 deliver 与 flush 用同一把互斥量：dispatch 途中不会看到
//     半注册的渠道表。
#pragma once

#include <core/result.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace module::schedule {

// 内置渠道 id（注册表键；schedule_rule.channel_ids 存的就是这些字符串）。
// 不要改名：落库数据与 §4.9 渠道表按这些键引用。
inline constexpr const char* kChannelFullscreen = "fullscreen";
inline constexpr const char* kChannelToast = "toast";
inline constexpr const char* kChannelOs = "os";
inline constexpr const char* kChannelSilent = "silent";

// 一次调度触发要投递的通知载荷。level 语义：0 信息 / 1 成功 / 2 警告 /
// 3 错误（与 platform::notify::Level 枚举序对齐，映射由各渠道决定）。
struct Delivery {
    std::string title;
    std::string body;
    int level = 0;
    std::optional<std::string> deeplink;  // 页面/模块定位；OS 通知无点击动作不消费
    std::int64_t runId = 0;   // 关联 schedule_run.id（记录层写 schedule_delivery 用）
    std::string dedupeKey;    // (rule, planned_at) 去重键；渠道级去重一律以它为准
};

// 单渠道单次投递回执。status 是渠道各自定义的稳定状态串（测试断言与
// schedule_delivery.status 落库依赖其稳定性），detail 为中文说明进日志。
struct Receipt {
    std::string status = "unknown";
    std::string detail;
};

class Channel {
public:
    virtual ~Channel() = default;
    // 渠道稳定 id（注册表键，必须与 kChannel* 常量一致）
    virtual std::string id() const = 0;
    // 任意线程可调（见文件头并发契约）。Result::fail 只表示载荷非法或
    // 渠道内部不变量；环境类失败（系统通知被禁、托盘未就绪等）一律归入
    // Receipt.status，不返回 fail——投递失败必须与调度运行成功可区分。
    virtual core::Result<Receipt> deliver(const Delivery& d) = 0;
    // 主线程泵每 tick 调用一次（宿主 ticker onTick）；无 UI 队列的渠道空实现。
    // 只允许在主线程调用——实现会触 UI（开窗 / showAppNotice）。
    virtual void flushToMainThread() {}
};

// 分发报告中单个渠道的结局
struct DispatchEntry {
    std::string channelId;
    core::Result<Receipt> receipt;  // isFail = 该渠道未投递（未知 id / 渠道返回 fail）
};

struct DispatchReport {
    std::vector<DispatchEntry> entries;  // 顺序与入参 channel_ids 一致
    bool allOk() const {
        for (const auto& e : entries) {
            if (e.receipt.isFail()) return false;
        }
        return true;
    }
};

class ChannelRegistry {
public:
    // 注册渠道并接管所有权。重复 id → fail(AlreadyExists) 且不替换——
    // 渠道装配可以重跑，但已注册的实现不得被静默换掉。
    core::Result<void> registerChannel(std::unique_ptr<Channel> channel);

    // 只读查找；未知 id → nullptr
    const Channel* find(const std::string& id) const;

    // 按 channel_ids 顺序逐渠道投递同一 Delivery（任意线程）。
    // 契约：空 ids → ok(空 entries)，不是错误（无渠道 = 无投递）。
    // 未知 id → 该 entry 记 fail(NotFound)，不影响其余渠道，整体仍 ok(report)。
    // 单个渠道失败同样只记在对应 entry，不短路整条分发链。
    core::Result<DispatchReport> deliver(const std::vector<std::string>& channelIds,
                                         const Delivery& d);

    // 主线程泵：按注册顺序逐渠道 flushToMainThread()（仅主线程调用）
    void flushAllToMainThread();

    // 测试缝：清空全部注册
    void resetForTest();

private:
    mutable std::recursive_mutex mutex_;             // 保护 channels_；必须递归：deliver 持锁调渠道回调，
                                                     // 渠道实现里再入 registry API（含递归投递）是合法路径
    std::vector<std::unique_ptr<Channel>> channels_;  // 保序：分发/flush 顺序稳定
};

// 内置渠道工厂：id 只认 kChannel* 常量（S5 四渠道 + wecom），未知 id → nullptr。
// 返回的渠道带生产默认接线（真窗口 / app toast / platform::notify / 空投递 /
// 默认配置的 wecom webhook——key 未配置时投递回 missing_secret 回执，不发请求）。
std::unique_ptr<Channel> makeBuiltinChannel(const std::string& id);

// 标准装配：注册五个内置渠道。宿主模块 onStart 调用一次。
void registerBuiltinChannels(ChannelRegistry& registry);

// ---- 测试缝（仅测试使用；生产装配走 makeBuiltinChannel）----
// 全屏渠道开窗/刷新回调：items = 本次应显示的聚合列表；notifyClosed = 用户
// 关窗时由回调持有方调用（真实窗口 = onWindowDestroyed 路径，测试 = 手动
// 触发模拟关窗）。now = Unix 秒假时钟。
using ChannelWindowUpdater =
    std::function<void(const std::vector<Delivery>&, const std::function<void()>& notifyClosed)>;
using ChannelNowFn = std::function<std::int64_t()>;
// toast 渠道落点 sink（默认 app::showAppNotice）
using ChannelToastSink = std::function<void(const std::string&, const std::string&)>;

// 全屏渠道弹窗总开关（默认关，与 release「上线提醒弹窗」同口径独立）：关 =
// fullscreen 渠道投递被抑制（deliver 记日志、返回 ok Receipt，不弹窗、不入队）。
// 仅主线程调用；进程级状态（未来接设置项用，当前无设置入口恒为关）。
void setFullscreenPopupEnabled(bool on);
bool fullscreenPopupEnabled();

// 测试工厂：respectGlobalGate=false（默认）→ 不读总开关，行为与历史上线版一致；
// true 时 deliver 走与生产相同的开关抑制路径，供 OFF 路径守卫用例。
std::unique_ptr<Channel> makeFullscreenChannelForTest(ChannelWindowUpdater updater,
                                                      ChannelNowFn now,
                                                      bool respectGlobalGate = false);
std::unique_ptr<Channel> makeToastChannelForTest(ChannelToastSink sink);

}  // namespace module::schedule
