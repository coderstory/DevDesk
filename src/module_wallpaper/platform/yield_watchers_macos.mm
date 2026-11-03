// src/module_wallpaper/platform/yield_watchers_macos.mm
// macOS 让路事件源：锁屏 / 熄屏 / 系统睡眠走系统通知，电池走 IOKit 事件源 + 轮询，
// 全屏走 CGWindowList 覆盖率采样。
//
// 与 Windows 版的差别：没有「消息窗 + 自建线程」这套——macOS 的通知中心、IOKit
// run loop source、CoreGraphics 重配置回调都只能挂在主线程的 run loop 上，而 DevDesk
// 的主循环（GLFW/NSApplication）本来就在主线程转，所以直接注册到主 run loop。
//
// 线程契约（与 yield_watchers.h 一致）：
//   - 回调在主线程触发；本实现内部再转到 queue_ 串行队列上调用回调体，避免回调里
//     做耗时工作阻塞 UI 帧。
//   - start/stop 幂等；stop 后不再有任何回调。
//   - 轮询方法任意线程可调，快速返回（只读系统状态，不取锁）。
//
// 信号口径（沿用参考实现实跑结论，改动前先读）：
//   * 锁屏：通知只当触发器，收到后一律重读会话字典的 CGSSessionScreenIsLocked——
//     投递方不可信。绝不往系统通知名投合成事件，同机其它壁纸 app 会一起被暂停。
//   * 熄屏：每次重读 CGDisplayIsAsleep，不靠「边沿」记忆。
//   * 系统睡眠：只由 willSleep / didWake 这一对通知切换（无采样可混用）。
//   * 电池：读失败不许静默折值——折成「在 AC 上」会让用户在电池上白耗电，
//     折成「在电池」会让本该播放的壁纸停掉；两个方向后果相反。
//   * 全屏：覆盖率 ≥ 1.0（原生全屏与最大化一视同仁），按 pid 排除本进程。
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <IOKit/ps/IOPowerSources.h>
#import <IOKit/ps/IOPSKeys.h>

#include "module_wallpaper/platform/fullscreen_geometry.h"
#include "module_wallpaper/platform/yield_watchers.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <unistd.h>  // getpid

#include "core/logging.h"

// C 回调不能带 C++ 上下文：用一张进程级单槽表把回调转回来。IOKit 的电源事件源与
// CoreGraphics 的重配置回调本来就都是进程级注册，单槽表正好对上真实语义。
namespace {

std::mutex g_trampolineMutex;
std::function<void()> g_powerHandler;
std::function<void()> g_displayReconfiguredHandler;

void powerTrampoline(void* /*context*/) {
    std::function<void()> handler;
    {
        std::lock_guard<std::mutex> lock(g_trampolineMutex);
        handler = g_powerHandler;
    }
    if (handler) handler();
}

void displayReconfiguredTrampoline(CGDirectDisplayID display,
                                   CGDisplayChangeSummaryFlags flags,
                                   void* /*context*/) {
    (void)display;
    (void)flags;
    std::function<void()> handler;
    {
        std::lock_guard<std::mutex> lock(g_trampolineMutex);
        handler = g_displayReconfiguredHandler;
    }
    if (handler) handler();
}

} // namespace

namespace module::wallpaper {

class YieldWatchersMac final : public YieldWatchers {
public:
    YieldWatchersMac()
        : queue_(dispatch_queue_create("com.devdesk.wallpaper.yield",
                                       DISPATCH_QUEUE_SERIAL)) {}

    ~YieldWatchersMac() override { stop(); }

    YieldWatchersMac(const YieldWatchersMac&) = delete;
    YieldWatchersMac& operator=(const YieldWatchersMac&) = delete;

    void setHoldCallback(HoldCallback callback) override {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        holdCallback_ = std::move(callback);
    }

    void setDisplayChangedCallback(DisplayChangedCallback callback) override {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        displayChangedCallback_ = std::move(callback);
    }

    bool start(std::string* error) override {
        std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
        if (started_) return true;  // 幂等
        stopRequested_.store(false);
        started_ = true;

        // 注册必须在主线程（通知中心 / run loop source / 显示重配置回调都绑主 run loop）。
        // start 由服务线程调用，所以同步投递过去。
        if (!runOnMainThreadSync([this] { registerOnMainThread(); })) {
            started_ = false;
            if (error != nullptr) *error = "让路 watcher 启动超时（主线程无响应）";
            return false;
        }
        // 启动即重算一次：锁屏/熄屏/睡眠都只在跃迁时投递，启动后等不到。
        //
        // **必须异步**：服务在 start() 里持着自己的 mu_ 调用本方法，而回调链会走到
        // WallpaperService::applySystemReason() 再取同一把 mu_ —— 同步重算就是自死锁
        //（2026-11-03 真机探针实测卡死，主线程停在 psynch_mutexwait）。投递到自己的
        // 串行队列后，回落在 start() 返回之后，锁已释放。
        dispatch_async(queue_, ^{
            reevaluateAll();
        });
        return true;
    }

    void stop() override {
        {
            std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
            if (!started_) return;
            started_ = false;
            stopRequested_.store(true);
        }
        // 注销同样在主线程；stop 是收尾路径，等不到也不能崩，不检查结果。
        runOnMainThreadSync([this] { unregisterOnMainThread(); });
    }

    bool pollOnBattery() override {
        // 读失败：不让路（契约：查询失败一律 false，不因接口故障把壁纸错误暂停）。
        return isOnBattery(/*reportFailure=*/false);
    }

    bool pollFullscreenOtherProcess() override {
        return sampleFullscreen();
    }

private:
    // 主队列同步投递（与宿主同款有界等待）。
    template <typename Fn>
    static bool runOnMainThreadSync(Fn&& fn) {
        if ([NSThread isMainThread]) {
            fn();
            return true;
        }
        auto done = std::make_shared<bool>(false);
        auto mutex = std::make_shared<std::mutex>();
        auto cv = std::make_shared<std::condition_variable>();
        dispatch_async(dispatch_get_main_queue(), ^{
            fn();
            {
                std::lock_guard<std::mutex> lock(*mutex);
                *done = true;
            }
            cv->notify_all();
        });
        std::unique_lock<std::mutex> lock(*mutex);
        return cv->wait_for(lock, std::chrono::milliseconds(3000),
                            [&] { return *done; });
    }

    void registerOnMainThread() {
        // ── 锁屏 / 解锁：分布式通知只当触发器，收到后重读会话字典 ──
        const char* lockName = "com.apple.screenIsLocked";
        const char* unlockName = "com.apple.screenIsUnlocked";
        for (const char* rawName : {lockName, unlockName}) {
            id token = [[NSDistributedNotificationCenter defaultCenter]
                addObserverForName:[NSString stringWithUTF8String:rawName]
                            object:nil
                             queue:nil
                        usingBlock:^(NSNotification* notification) {
                            (void)notification;
                            dispatch_async(queue_, ^{
                                reevaluateLock();
                            });
                        }];
            tokens_.push_back(token);
        }

        // ── 熄屏 / 系统睡眠：NSWorkspace 通知 ──
        NSNotificationCenter* workspaceCenter = [[NSWorkspace sharedWorkspace] notificationCenter];
        id sleepToken = [workspaceCenter
            addObserverForName:NSWorkspaceWillSleepNotification
                         object:nil
                          queue:nil
                     usingBlock:^(NSNotification* notification) {
                         (void)notification;
                         systemSleeping_ = true;
                         reevaluateAll();
                     }];
        tokens_.push_back(sleepToken);
        id wakeToken = [workspaceCenter
            addObserverForName:NSWorkspaceDidWakeNotification
                         object:nil
                          queue:nil
                     usingBlock:^(NSNotification* notification) {
                         (void)notification;
                         systemSleeping_ = false;
                         reevaluateAll();
                     }];
        tokens_.push_back(wakeToken);

        // ── 电池：IOKit 事件源（拔插电源即触发），回调只当触发器，状态现读 ──
        {
            std::lock_guard<std::mutex> lock(g_trampolineMutex);
            g_powerHandler = [this] {
                dispatch_async(queue_, ^{
                    reevaluateBattery();
                });
            };
        }
        CFTypeRef source = IOPSNotificationCreateRunLoopSource(powerTrampoline, nullptr);
        if (source != nullptr) {
            CFRunLoopAddSource(CFRunLoopGetMain(), (CFRunLoopSourceRef)source,
                               kCFRunLoopDefaultMode);
            powerSource_ = source;
        } else {
            // 注册失败只失去电池让路（轮询通道仍可用），不作致命处理。
            LOG_WARN("壁纸: IOKit 电源事件源注册失败（电池让路退化为轮询）");
        }

        // ── 换屏：显示重配置回调 ──
        {
            std::lock_guard<std::mutex> lock(g_trampolineMutex);
            g_displayReconfiguredHandler = [this] {
                dispatch_async(queue_, ^{
                    DisplayChangedCallback callback;
                    {
                        std::lock_guard<std::mutex> callbackLock(callbackMutex_);
                        callback = displayChangedCallback_;
                    }
                    if (callback) callback();
                });
            };
        }
        CGDisplayRegisterReconfigurationCallback(displayReconfiguredTrampoline, nullptr);
        displayReconfigurationRegistered_ = true;
    }

    void unregisterOnMainThread() {
        for (id token : tokens_) {
            [[NSDistributedNotificationCenter defaultCenter] removeObserver:token];
            [[[NSWorkspace sharedWorkspace] notificationCenter] removeObserver:token];
        }
        tokens_.clear();
        {
            std::lock_guard<std::mutex> lock(g_trampolineMutex);
            g_powerHandler = nullptr;
            g_displayReconfiguredHandler = nullptr;
        }
        if (powerSource_ != nullptr) {
            CFRunLoopRemoveSource(CFRunLoopGetMain(), (CFRunLoopSourceRef)powerSource_,
                                  kCFRunLoopDefaultMode);
            CFRelease(powerSource_);
            powerSource_ = nullptr;
        }
        if (displayReconfigurationRegistered_) {
            CGDisplayRemoveReconfigurationCallback(displayReconfiguredTrampoline, nullptr);
            displayReconfigurationRegistered_ = false;
        }
    }

    // ── 重算（全部在 queue_ 上）──

    void reevaluateLock() { fireHold(HoldReason::ScreenLocked, isScreenLocked()); }

    void reevaluateBattery() { fireHold(HoldReason::Battery, isOnBattery(true)); }

    void reevaluateAll() {
        const bool asleep = CGDisplayIsAsleep(CGMainDisplayID()) != 0;
        fireHold(HoldReason::ScreenLocked, isScreenLocked());
        fireHold(HoldReason::DisplayAsleep, asleep);
        fireHold(HoldReason::SystemSleeping, systemSleeping_.load());
        fireHold(HoldReason::Battery, isOnBattery(true));
    }

    void fireHold(HoldReason reason, bool active) {
        if (stopRequested_.load()) return;
        HoldCallback callback;
        {
            std::lock_guard<std::mutex> lock(callbackMutex_);
            callback = holdCallback_;
        }
        if (callback) callback(reason, active);
    }

    // 锁屏状态：读会话字典的 CGSSessionScreenIsLocked。读不到按「未锁」处理且
    // 不臆造信号（编一个值会把「读不到」与「没锁」抹成同一件事）。
    static bool isScreenLocked() {
        NSDictionary* session = CFBridgingRelease(CGSessionCopyCurrentDictionary());
        if (session == nil) return false;
        id raw = [session objectForKey:@"CGSSessionScreenIsLocked"];
        if (raw == nil) return false;
        if ([raw isKindOfClass:[NSNumber class]]) return [(NSNumber*)raw intValue] != 0;
        return false;
    }

    // 当前是否在电池上。reportFailure=true 时读失败落一行日志（事件路径），
    // false 时静默（轮询路径：每 250ms 一拍，不能刷屏）。
    static bool isOnBattery(bool reportFailure) {
        bool onBattery = false;
        bool failed = true;
        CFTypeRef blob = IOPSCopyPowerSourcesInfo();
        if (blob != nullptr) {
            CFArrayRef list = IOPSCopyPowerSourcesList(blob);
            if (list != nullptr) {
                const CFIndex count = CFArrayGetCount(list);
                if (count > 0) {
                    CFTypeRef source = CFArrayGetValueAtIndex(list, 0);
                    // IOPSGetPowerSourceDescription 返回 **unretained** 指针，随
                    // IOPSCopyPowerSourcesInfo 那次 retain 一起释放——只能 __bridge。
                    NSDictionary* description = (__bridge NSDictionary*)IOPSGetPowerSourceDescription(
                        blob, source);
                    id state = description != nil
                                   ? [description objectForKey:@kIOPSPowerSourceStateKey]
                                   : nil;
                    if ([state isKindOfClass:[NSString class]]) {
                        // 判据比对 SDK 常量本身，不手抄字符串。
                        if ([(NSString*)state isEqualToString:@kIOPSBatteryPowerValue]) {
                            onBattery = true;
                            failed = false;
                        } else if ([(NSString*)state isEqualToString:@kIOPSACPowerValue]) {
                            onBattery = false;
                            failed = false;
                        }
                    }
                }
                CFRelease(list);
            }
            CFRelease(blob);
        }
        if (failed && reportFailure) {
            // 显式上报，不静默折值：折哪个方向都是用户可见的后果变化。
            LOG_WARN("壁纸: 电源状态读取失败（本次不让路）");
        }
        return onBattery;
    }

    // 全屏采样：枚举 layer 0、alpha > 0、且**不属于本进程**的窗口矩形。
    // 按 pid 认领而不是 owner 名：同机可能有别的 app 来自同一个可执行文件。
    // 字段白名单 pid / layer / alpha / bounds——绝不读窗口标题（可能含用户文件名）。
    static bool sampleFullscreen() {
        NSScreen* screen = [NSScreen mainScreen];
        if (screen == nil) return false;  // 读不到屏幕就读不到几何：判「没满覆盖」
        const NSRect visible = screen.visibleFrame;
        const NSRect frame = screen.frame;

        CFArrayRef rawWindows = CGWindowListCopyWindowInfo(
            kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
        if (rawWindows == nullptr) return false;

        const long long selfPid = (long long)getpid();
        std::vector<fullscreen_geometry::WindowSample> samples;
        const CFIndex count = CFArrayGetCount(rawWindows);
        for (CFIndex i = 0; i < count; ++i) {
            CFDictionaryRef window = (CFDictionaryRef)CFArrayGetValueAtIndex(rawWindows, i);
            CFNumberRef layerNumber = (CFNumberRef)CFDictionaryGetValue(
                window, kCGWindowLayer);
            CFNumberRef pidNumber = (CFNumberRef)CFDictionaryGetValue(
                window, kCGWindowOwnerPID);
            if (layerNumber == nullptr || pidNumber == nullptr) continue;
            int layer = 0;
            long long pid = 0;
            if (!CFNumberGetValue(layerNumber, kCFNumberIntType, &layer)) continue;
            if (!CFNumberGetValue(pidNumber, kCFNumberLongLongType, &pid)) continue;
            if (layer != 0) continue;      // 只认常规窗口层（桌面/状态栏都在别的层）
            if (pid == selfPid) continue;  // 本进程的窗口不挡自己

            double alpha = 1.0;
            CFNumberRef alphaNumber = (CFNumberRef)CFDictionaryGetValue(
                window, kCGWindowAlpha);
            if (alphaNumber != nullptr) {
                CFNumberGetValue(alphaNumber, kCFNumberDoubleType, &alpha);
            }
            if (alpha <= 0) continue;

            CFDictionaryRef bounds = (CFDictionaryRef)CFDictionaryGetValue(
                window, kCGWindowBounds);
            if (bounds == nullptr) continue;
            CGRect rect = CGRectZero;
            if (!CGRectMakeWithDictionaryRepresentation(bounds, &rect)) continue;

            fullscreen_geometry::WindowSample sample;
            sample.pid = pid;
            sample.raw.x = rect.origin.x;
            sample.raw.y = rect.origin.y;
            sample.raw.w = rect.size.width;
            sample.raw.h = rect.size.height;
            samples.push_back(sample);
        }
        CFRelease(rawWindows);

        fullscreen_geometry::Rect visibleRect;
        visibleRect.x = visible.origin.x;
        visibleRect.y = visible.origin.y;
        visibleRect.w = visible.size.width;
        visibleRect.h = visible.size.height;

        const fullscreen_geometry::CoverageResult coverage =
            fullscreen_geometry::aggregate(samples, visibleRect, frame.size.height,
                                           fullscreen_geometry::measuredInset());
        return fullscreen_geometry::covers(coverage);
    }

    dispatch_queue_t queue_;
    std::atomic<bool> started_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> systemSleeping_{false};

    std::mutex lifecycleMutex_;
    std::mutex callbackMutex_;
    HoldCallback holdCallback_;
    DisplayChangedCallback displayChangedCallback_;

    std::vector<id> tokens_;
    CFTypeRef powerSource_ = nullptr;
    bool displayReconfigurationRegistered_ = false;
};

std::unique_ptr<YieldWatchers> createYieldWatchers() {
    return std::make_unique<YieldWatchersMac>();
}

} // namespace module::wallpaper
