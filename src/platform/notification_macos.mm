// src/platform/notification_macos.mm
// macOS 系统通知：UNUserNotificationCenter（UserNotifications.framework）。
// 状态语义按 spec §5：send 只返回 Accepted / NotBundled / PermissionDenied /
// NotDetermined / BackendUnavailable（Deduped/RateLimited 由 facade 产生）；
// 环境类失败一律 ok(Status)，Result::fail 只留给后端内部错误。
//
// 关键约束（改动前先读）：
//   * 裸二进制（build/DevDesk、devdesk_tests）**没有有效 .app bundle**。此时触碰
//     [UNUserNotificationCenter currentNotificationCenter] 会抛
//     NSInternalInconsistencyException（不是可捕获的 C++ 异常）。所有入口必须先经
//     hasUsableAppBundle() 守卫，守卫失败返回 NotBundled / Unknown。
//   * 前台展示需要 delegate：UNUserNotificationCenter.delegate 是 weak，必须由本文件
//     的强引用单例持有，否则 delegate 立刻释放、前台通知静默不展示。willPresent 必须
//     及时调用 completion 并返回 Banner|List|Sound。
//   * requestAuthorization 应在主线程发起（API 本身异步）；从后台线程调用时封送到
//     main queue。send 可从任意线程调用（UN API 线程安全），后台路径只做 UN 调用与
//     日志，不碰 UI / 全局 app 状态。
//   * authorizationStatus 需要同步返回，查询设置用 completion + ≤1s 信号量。**不要在
//     UI 主线程依赖它做阻塞式决策**：极端情况下会阻塞到 1 秒；超时返回缓存值。
//   * 本文件单独用 -fobjc-arc 编译（见 platform/CMakeLists.txt），不要手动 retain/release。
#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <UserNotifications/UserNotifications.h>

#include "notification.h"
#include "core/logging.h"

#include <dispatch/dispatch.h>

#include <atomic>
#include <string>

namespace platform::notify {
namespace {

bool hasUsableAppBundle() {
    NSBundle* mainBundle = [NSBundle mainBundle];
    NSString* bundleIdentifier = [mainBundle bundleIdentifier];
    NSURL* bundleURL = [mainBundle bundleURL];
    return bundleIdentifier.length > 0 &&
           [[bundleURL pathExtension] isEqualToString:@"app"];
}

std::string nsToUtf8(NSString* value) {
    if (value == nil) return {};
    const char* utf8 = [value UTF8String];
    return utf8 ? std::string(utf8) : std::string();
}

// UTF-8 → NSString；非法字节返回 nil（不能把乱码塞进系统通知）。
NSString* toNSString(const std::string& value) {
    return [[NSString alloc] initWithBytes:value.data()
                                    length:value.size()
                                  encoding:NSUTF8StringEncoding];
}

} // namespace
} // namespace platform::notify

// UNUserNotificationCenterDelegate 强引用单例：center.delegate 是 weak，必须有人长期持有。
// ObjC 声明必须在全局作用域（不能放进 C++ namespace）。
@interface DevDeskNotificationDelegate : NSObject <UNUserNotificationCenterDelegate>
@end

@implementation DevDeskNotificationDelegate

- (void)userNotificationCenter:(UNUserNotificationCenter*)center
       willPresentNotification:(UNNotification*)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions options))completionHandler {
    // 前台也展示：banner + 通知中心列表 + 声音。缺一不可的场景由调用方决定是否发通知，
    // 平台层不替业务静默降级（否则用户完全看不到）。
    completionHandler(UNNotificationPresentationOptionBanner |
                      UNNotificationPresentationOptionList |
                      UNNotificationPresentationOptionSound);
}

- (void)userNotificationCenter:(UNUserNotificationCenter*)center
    didReceiveNotificationResponse:(UNNotificationResponse*)response
             withCompletionHandler:(void (^)(void))completionHandler {
    // 本任务不做点击回调路由，只留痕；必须调 completion，否则系统会一直挂着。
    LOG_INFO("系统通知被点击: id=" +
             platform::notify::nsToUtf8(response.notification.request.identifier));
    completionHandler();
}

@end

namespace platform::notify {
namespace {

DevDeskNotificationDelegate* sharedDelegate() {
    static DevDeskNotificationDelegate* delegate = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        delegate = [[DevDeskNotificationDelegate alloc] init];
    });
    return delegate;
}

// 只在 hasUsableAppBundle() 之后调用。
void installDelegate() {
    UNUserNotificationCenter* center = [UNUserNotificationCenter currentNotificationCenter];
    if (center.delegate != sharedDelegate()) {
        center.delegate = sharedDelegate();
    }
}

class MacBackend final : public Backend {
public:
    void initialize() override;
    core::Result<Status> send(const Notification& notification) override;
    Authorization authorizationStatus() override;
    void requestAuthorization() override;

private:
    // 授权结果缓存：requestAuthorization 的回调与 initialize 的查询写入；
    // authorizationStatus 优先读它，避免每次查询都走一次可能阻塞的 settings 请求。
    std::atomic<Authorization> cached_{Authorization::NotDetermined};

    void resetAuthCache() { cached_.store(Authorization::NotDetermined, std::memory_order_relaxed); }
};

void MacBackend::initialize() {
    if (!hasUsableAppBundle()) {
        LOG_WARN(std::string("跳过通知初始化：当前进程没有有效的 .app bundle"));
        return;
    }
    // 幂等由 facade 的 g_initialized 保证；这里的 dispatch_once 兜底直接持有
    // Backend& 的调用方（不经过 facade）。
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        MacBackend* backend = this;
        // 用户可能运行中改系统设置：激活时把缓存置回 NotDetermined，下次
        // authorizationStatus() 重新查询；不自动再弹授权框（系统也不会再弹）。
        [[NSNotificationCenter defaultCenter]
            addObserverForName:NSApplicationDidBecomeActiveNotification
                        object:nil
                         queue:nil
                    usingBlock:^(NSNotification*) {
                        backend->resetAuthCache();
                    }];
        // 异步查询授权；尚未决定则在主线程发起授权请求（应用先完成启动）。
        [[UNUserNotificationCenter currentNotificationCenter]
            getNotificationSettingsWithCompletionHandler:^(
                UNNotificationSettings* settings) {
                switch (settings.authorizationStatus) {
                    case UNAuthorizationStatusAuthorized:
                    case UNAuthorizationStatusProvisional:
                        backend->cached_.store(Authorization::Authorized,
                                               std::memory_order_relaxed);
                        break;
                    case UNAuthorizationStatusDenied:
                        backend->cached_.store(Authorization::Denied,
                                               std::memory_order_relaxed);
                        break;
                    default:
                        dispatch_async(dispatch_get_main_queue(), ^{
                            backend->requestAuthorization();
                        });
                        break;
                }
            }];
    });
}

core::Result<Status> MacBackend::send(const Notification& notification) {
    // 守卫必须最先执行：裸二进制里 currentNotificationCenter 会直接抛 ObjC 异常。
    if (!hasUsableAppBundle()) {
        return core::Result<Status>::ok(Status::NotBundled);
    }
    // facade 已保证非空/良构；这里是直接持有 Backend& 的调用方的防御边界。
    if (notification.title.empty() || notification.body.empty()) {
        return core::Result<Status>::ok(Status::BackendUnavailable);
    }

    NSString* title = toNSString(notification.title);
    NSString* body = toNSString(notification.body);
    NSString* identifier = toNSString(notification.id);
    if (title == nil || body == nil || identifier == nil) {
        return core::Result<Status>::ok(Status::BackendUnavailable);
    }

    @autoreleasepool {
        UNMutableNotificationContent* content = [[UNMutableNotificationContent alloc] init];
        content.title = title;
        content.body = body;
        if (notification.sound) {
            content.sound = [UNNotificationSound defaultSound];
        }
        // 为将来点击路由留字段；v1 不消费。
        content.userInfo = @{@"level": @(static_cast<int>(notification.level))};
        // trigger = nil → 立即投递；同 id 会替换系统里的同 id 通知。
        UNNotificationRequest* request =
            [UNNotificationRequest requestWithIdentifier:identifier
                                                 content:content
                                                 trigger:nil];
        // 异步入队：返回 ok 只代表系统受理；真正的错误在 completion 里，只记日志
        //（接口不承诺「用户看到」，也不把异步失败伪装成同步失败）。
        [[UNUserNotificationCenter currentNotificationCenter]
            addNotificationRequest:request
                 withCompletionHandler:^(NSError* error) {
                     if (error != nil) {
                         LOG_WARN("系统通知入队失败: " +
                                  nsToUtf8([error localizedDescription]));
                     }
                 }];
    }
    return core::Result<Status>::ok(Status::Accepted);
}

Authorization MacBackend::authorizationStatus() {
    if (!hasUsableAppBundle()) {
        return Authorization::Unknown;   // 无法查询 ≠ 未决定
    }
    const Authorization cached = cached_.load(std::memory_order_relaxed);
    if (cached != Authorization::NotDetermined) {
        return cached;
    }

    // 同步查询（接口是同步的）：completion 在系统队列执行，最多等 1 秒。
    // 超时返回缓存值（此刻就是 NotDetermined），不把「查不到」当成「未授权」。
    dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
    __block Authorization resolved = Authorization::NotDetermined;
    [[UNUserNotificationCenter currentNotificationCenter]
        getNotificationSettingsWithCompletionHandler:^(UNNotificationSettings* settings) {
            switch (settings.authorizationStatus) {
                case UNAuthorizationStatusAuthorized:
                case UNAuthorizationStatusProvisional:
                    resolved = Authorization::Authorized;
                    break;
                case UNAuthorizationStatusDenied:
                    resolved = Authorization::Denied;
                    break;
                default:
                    resolved = Authorization::NotDetermined;
                    break;
            }
            dispatch_semaphore_signal(semaphore);
        }];

    const long timedOut = dispatch_semaphore_wait(
        semaphore, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1 * NSEC_PER_SEC)));
    if (timedOut != 0) {
        LOG_WARN("查询通知授权状态超时（>1s），返回缓存值");
        return cached;
    }

    cached_.store(resolved, std::memory_order_relaxed);
    return resolved;
}

void MacBackend::requestAuthorization() {
    if (!hasUsableAppBundle()) {
        LOG_WARN(std::string("跳过通知授权请求：当前进程没有有效的 .app bundle"));
        return;
    }

    // backend 是进程级单例，捕获 this 安全。结果只在成功回调里更新缓存；
    // 错误（如系统服务不可用）不伪装成「用户拒绝」。
    MacBackend* backend = this;
    void (^requestBlock)(void) = ^{
        installDelegate();
        [[UNUserNotificationCenter currentNotificationCenter]
            requestAuthorizationWithOptions:(UNAuthorizationOptionAlert |
                                             UNAuthorizationOptionSound)
                          completionHandler:^(BOOL granted, NSError* error) {
                              if (error != nil) {
                                  LOG_WARN("通知授权请求失败: " +
                                           nsToUtf8([error localizedDescription]));
                                  return;
                              }
                              backend->cached_.store(granted ? Authorization::Authorized
                                                             : Authorization::Denied,
                                                     std::memory_order_relaxed);
                              LOG_INFO(granted ? "通知授权：已允许" : "通知授权：已拒绝");
                          }];
    };

    // 授权请求应在主线程发起；后台调用封送过去，不阻塞调用方。
    if ([NSThread isMainThread]) {
        requestBlock();
    } else {
        dispatch_async(dispatch_get_main_queue(), requestBlock);
    }
}

} // namespace

Backend& defaultBackend() {
    static MacBackend backend;
    return backend;
}

} // namespace platform::notify
