// src/module_wallpaper/platform/yield_watchers_macos.mm
// macOS 让路事件源：锁屏 / 熄屏 / 系统睡眠走系统通知，电池走 IOKit 事件源 + 轮询，
// 全屏走「workspace 事件（Space 切换 / 应用激活·失活）+ 服务轮询」双驱动。
//
// 与 Windows 版的差别：没有「消息窗 + 自建线程」这套——macOS 的通知中心、IOKit
// run loop source、CoreGraphics 重配置回调都只能挂在主线程的 run loop 上，而 DevDesk
// 的主循环（GLFW/NSApplication）本来就在主线程转，所以直接注册到主 run loop。
//
// 线程契约（与 yield_watchers.h 一致）：
//   - 事件块把重算投递到 state->queue 串行队列，回调体在队列上调，不阻塞投递方。
//   - start/stop 幂等；stop 后不再有任何回调（stopRequested 关闸）。
//   - 轮询方法任意线程可调，快速返回（只读系统状态，不取锁）。
//
// **生命周期安全（2026-11-04 审计 F4 根治）**：所有跨线程块（通知块、主队列注册/
// 注销块、全局 trampoline 里的 handler）只持 `shared_ptr<YieldState>`，**零裸 this**。
// 同步投递超时（主队列堵死 >3s）后晚到的块照样安全执行——state 随块存活，事件
// 全部被 stopRequested 拦成 no-op；绝不触已析构的 watcher 对象。
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

// 共享状态体：注册产物 + 回调 + 开关。块只捕它，watcher 对象生死与块无关。
struct YieldState {
    dispatch_queue_t queue = nullptr;

    std::atomic<bool> stopRequested{false};
    std::atomic<bool> systemSleeping{false};

    std::mutex callbackMutex;
    YieldWatchers::HoldCallback holdCallback;
    YieldWatchers::DisplayChangedCallback displayChangedCallback;

    // 注册/注销产物——只在主队列块里读写（注册与注销都投主队列）。
    std::vector<id> tokens;
    CFTypeRef powerSource = nullptr;
    bool displayReconfigurationRegistered = false;
};

// ── 系统状态读取（纯读，任意线程可调）──

// 锁屏状态：读会话字典的 CGSSessionScreenIsLocked。读不到按「未锁」处理且
// 不臆造信号（编一个值会把「读不到」与「没锁」抹成同一件事）。
bool isScreenLocked() {
    NSDictionary* session = CFBridgingRelease(CGSessionCopyCurrentDictionary());
    if (session == nil) return false;
    id raw = [session objectForKey:@"CGSSessionScreenIsLocked"];
    if (raw == nil) return false;
    if ([raw isKindOfClass:[NSNumber class]]) return [(NSNumber*)raw intValue] != 0;
    return false;
}

// 当前是否在电池上。reportFailure=true 时读失败落一行日志（事件路径），
// false 时静默（轮询路径：1s 一拍，仍不该刷屏）。
bool isOnBattery(bool reportFailure) {
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
bool sampleFullscreen() {
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

// ── 重算与回调派发（全部在 state->queue 串行队列上执行）──

void fireHold(const std::shared_ptr<YieldState>& state, HoldReason reason, bool active) {
    if (state->stopRequested.load()) return;
    YieldWatchers::HoldCallback callback;
    {
        std::lock_guard<std::mutex> lock(state->callbackMutex);
        callback = state->holdCallback;
    }
    if (callback) callback(reason, active);
}

void reevaluateLock(const std::shared_ptr<YieldState>& state) {
    fireHold(state, HoldReason::ScreenLocked, isScreenLocked());
}

void reevaluateBattery(const std::shared_ptr<YieldState>& state) {
    fireHold(state, HoldReason::Battery, isOnBattery(true));
}

void reevaluateAll(const std::shared_ptr<YieldState>& state) {
    const bool asleep = CGDisplayIsAsleep(CGMainDisplayID()) != 0;
    fireHold(state, HoldReason::ScreenLocked, isScreenLocked());
    fireHold(state, HoldReason::DisplayAsleep, asleep);
    fireHold(state, HoldReason::SystemSleeping, state->systemSleeping.load());
    fireHold(state, HoldReason::Battery, isOnBattery(true));
}

// ── 注册 / 注销（只跑在主队列；只触 state，不触 watcher 对象）──

// 形参**必须按值**收 shared_ptr：块（以及块内 dispatch_async 的内层 block）捕获的是
// 形参本身，按引用收会把「栈上形参的引用」捕进块里——registerOnMainThread 返回后该
// 形参随 runOnMainThreadSync 的栈帧一起销毁，之后任何一次锁屏/睡眠/电源/换屏通知
// 触发块，读 shared_ptr 都是 stack-use-after-scope（ASan 实测复现；线上表现为
// SIGSEGV，堆栈落在块首行 dispatch_async）。按值收 = 块持有自己的强引用副本，
// 通知可能晚到（runOnMainThreadSync 超时后）也安全，与 start/stop 处
// `[state = state_]` 的写法同源。见 2026-11-04 macOS 关窗后崩溃取证。
void registerOnMainThread(std::shared_ptr<YieldState> state) {
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
                        dispatch_async(state->queue, ^{ reevaluateLock(state); });
                    }];
        state->tokens.push_back(token);
    }

    // ── 熄屏 / 系统睡眠：NSWorkspace 通知 ──
    // 标志位与重算一并投队列：与其它四路「触发器只投递」同一纪律，标志写入也被串行化。
    NSNotificationCenter* workspaceCenter = [[NSWorkspace sharedWorkspace] notificationCenter];
    id sleepToken = [workspaceCenter
        addObserverForName:NSWorkspaceWillSleepNotification
                    object:nil
                     queue:nil
                usingBlock:^(NSNotification* notification) {
                    (void)notification;
                    dispatch_async(state->queue, ^{
                        state->systemSleeping = true;
                        reevaluateAll(state);
                    });
                }];
    state->tokens.push_back(sleepToken);
    id wakeToken = [workspaceCenter
        addObserverForName:NSWorkspaceDidWakeNotification
                    object:nil
                     queue:nil
                usingBlock:^(NSNotification* notification) {
                    (void)notification;
                    dispatch_async(state->queue, ^{
                        state->systemSleeping = false;
                        reevaluateAll(state);
                    });
                }];
    state->tokens.push_back(wakeToken);

    // ── 电池：IOKit 事件源（拔插电源即触发），回调只当触发器，状态现读 ──
    {
        std::lock_guard<std::mutex> lock(g_trampolineMutex);
        // std::function 吃 C++ lambda（捕 state）；内层 dispatch_async 必须是 block。
        g_powerHandler = [state] {
            dispatch_async(state->queue, ^{ reevaluateBattery(state); });
        };
    }
    CFTypeRef source = IOPSNotificationCreateRunLoopSource(powerTrampoline, nullptr);
    if (source != nullptr) {
        CFRunLoopAddSource(CFRunLoopGetMain(), (CFRunLoopSourceRef)source,
                           kCFRunLoopDefaultMode);
        state->powerSource = source;
    } else {
        // 注册失败只失去电池让路（轮询通道仍可用），不作致命处理。
        LOG_WARN("壁纸: IOKit 电源事件源注册失败（电池让路退化为轮询）");
    }

    // ── 全屏：事件源（即时响应）+ 轮询（兜底）双驱动 ──
    //
    // 为什么两者都要：Space 切换、切前台 app 会产生事件，事件能即时响应；但
    // 「最大化 / 还原 / 拖动窗口」**不产生**任何事件，只能靠轮询捕捉。缺轮询
    // 会让「窗口已经盖满屏了却还在播」悬到下一次事件；只靠轮询则每秒要枚举
    // 几十次窗口列表（费电）。两者合起来才是 pic 已验证的形状。
    {
        NSNotificationCenter* workspaceCenter2 =
            [[NSWorkspace sharedWorkspace] notificationCenter];
        NSArray<NSNotificationName>* names = @[
            NSWorkspaceActiveSpaceDidChangeNotification,
            NSWorkspaceDidActivateApplicationNotification,
            NSWorkspaceDidDeactivateApplicationNotification,
        ];
        for (NSNotificationName name in names) {
            id token = [workspaceCenter2
                addObserverForName:name
                            object:nil
                             queue:nil
                        usingBlock:^(NSNotification* notification) {
                            (void)notification;
                            // 全屏状态变化不是「让路事件」而是「立即重判一次」：
                            // 走与轮询同一条路径，结果一致。
                            // 推一次结论，让服务不必等下一拍轮询：与轮询走同一个
                            // 判据函数，两条路径结论必然一致。
                            dispatch_async(state->queue, ^{
                                fireHold(state, HoldReason::Fullscreen,
                                         sampleFullscreen());
                            });
                        }];
            state->tokens.push_back(token);
        }
    }

    // ── 换屏：显示重配置回调 ──
    {
        std::lock_guard<std::mutex> lock(g_trampolineMutex);
        g_displayReconfiguredHandler = [state] {
            dispatch_async(state->queue, ^{
                YieldWatchers::DisplayChangedCallback callback;
                {
                    std::lock_guard<std::mutex> callbackLock(state->callbackMutex);
                    callback = state->displayChangedCallback;
                }
                if (callback) callback();
            });
        };
    }
    CGDisplayRegisterReconfigurationCallback(displayReconfiguredTrampoline, nullptr);
    state->displayReconfigurationRegistered = true;
}

void unregisterOnMainThread(const std::shared_ptr<YieldState>& state) {
    for (id token : state->tokens) {
        [[NSDistributedNotificationCenter defaultCenter] removeObserver:token];
        [[[NSWorkspace sharedWorkspace] notificationCenter] removeObserver:token];
    }
    state->tokens.clear();
    {
        std::lock_guard<std::mutex> lock(g_trampolineMutex);
        g_powerHandler = nullptr;
        g_displayReconfiguredHandler = nullptr;
    }
    if (state->powerSource != nullptr) {
        CFRunLoopRemoveSource(CFRunLoopGetMain(), (CFRunLoopSourceRef)state->powerSource,
                              kCFRunLoopDefaultMode);
        CFRelease(state->powerSource);
        state->powerSource = nullptr;
    }
    if (state->displayReconfigurationRegistered) {
        CGDisplayRemoveReconfigurationCallback(displayReconfiguredTrampoline, nullptr);
        state->displayReconfigurationRegistered = false;
    }
}

// 主队列同步投递（与宿主同款有界等待）。超时返回 false；调用方必须保证 fn 只触
// 堆上状态（shared_ptr），不捕栈引用/裸 this。
template <typename Fn>
bool runOnMainThreadSync(Fn&& fn) {
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

class YieldWatchersMac final : public YieldWatchers {
public:
    YieldWatchersMac() : state_(std::make_shared<YieldState>()) {
        state_->queue = dispatch_queue_create("com.devdesk.wallpaper.yield",
                                              DISPATCH_QUEUE_SERIAL);
    }

    ~YieldWatchersMac() override {
        // 先关闸再走 stop：析构后晚到的注册/注销/事件块一律 no-op（它们只持 state）。
        state_->stopRequested.store(true);
        stop();
    }

    YieldWatchersMac(const YieldWatchersMac&) = delete;
    YieldWatchersMac& operator=(const YieldWatchersMac&) = delete;

    void setHoldCallback(HoldCallback callback) override {
        std::lock_guard<std::mutex> lock(state_->callbackMutex);
        state_->holdCallback = std::move(callback);
    }

    void setDisplayChangedCallback(DisplayChangedCallback callback) override {
        std::lock_guard<std::mutex> lock(state_->callbackMutex);
        state_->displayChangedCallback = std::move(callback);
    }

    bool start(std::string* error) override {
        std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
        if (started_) return true;  // 幂等
        state_->stopRequested.store(false);
        started_ = true;

        // 注册必须在主线程（通知中心 / run loop source / 显示重配置回调都绑主 run loop）。
        // start 由服务线程调用，所以同步投递过去。块只持 state：超时后晚到的注册块
        // 也安全，其事件被 stopRequested 拦成 no-op。
        if (!runOnMainThreadSync([state = state_] { registerOnMainThread(state); })) {
            started_ = false;
            state_->stopRequested.store(true);
            if (error != nullptr) *error = "让路 watcher 启动超时（主线程无响应）";
            return false;
        }
        // 启动即重算一次：锁屏/熄屏/睡眠都只在跃迁时投递，启动后等不到。
        //
        // **必须异步**：服务在 start() 里持着自己的 mu_ 调用本方法，而回调链会走到
        // WallpaperService::applySystemReason() 再取同一把 mu_ —— 同步重算就是自死锁
        //（2026-11-03 真机探针实测卡死，主线程停在 psynch_mutexwait）。投递到自己的
        // 串行队列后，回落在 start() 返回之后，锁已释放。
        {
            // 局部拷贝再进 block：块隐式按值捕局部量，不捕 this。
            std::shared_ptr<YieldState> state = state_;
            dispatch_async(state_->queue, ^{ reevaluateAll(state); });
        }
        return true;
    }

    void stop() override {
        {
            std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
            if (!started_) return;
            started_ = false;
            state_->stopRequested.store(true);
        }
        // 注销同样在主队列；块只持 state——stop 是收尾路径，等不到也不能崩，
        // 不检查结果；晚到的注销块照样安全执行完毕。
        runOnMainThreadSync([state = state_] { unregisterOnMainThread(state); });
    }

    bool pollOnBattery() override {
        // 读失败：不让路（契约：查询失败一律 false，不因接口故障把壁纸错误暂停）。
        return isOnBattery(/*reportFailure=*/false);
    }

    bool pollFullscreenOtherProcess() override {
        return sampleFullscreen();
    }

private:
    std::shared_ptr<YieldState> state_;
    std::atomic<bool> started_{false};
    std::mutex lifecycleMutex_;
};

std::unique_ptr<YieldWatchers> createYieldWatchers() {
    return std::make_unique<YieldWatchersMac>();
}

} // namespace module::wallpaper
