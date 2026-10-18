// src/platform/notification.h
// 平台系统通知抽象（macOS UNUserNotificationCenter / Windows 托盘 NIF_INFO / 其它 POSIX 不支持）。
// facade（notification.cpp）负责清洗/截断/去重/限速/授权状态；平台后端只负责投递。
//
// 契约要点（调用方与工具层都要按这条理解，别把 Accepted 当成「用户看到了」）：
//   * Accepted 只代表系统受理了请求（macOS 是 addNotificationRequest 入队成功），
//     Focus/勿扰/通知中心关闭都可能静默抑制且无法检测。
//   * send() 可从任意线程调用（AgentLoop 工作线程直调）；平台亲和由各后端自己处理。
//   * 不抛 C++ 异常；环境类失败一律 ok(Receipt.status)，Result::fail 只留给
//     清洗后 title/body 为空（ValidationError）与 facade 内部不变量（Internal）。
#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <string>

#include <core/result.h>

namespace platform::notify {

enum class Level { Info, Success, Warning, Error };

// Unknown       = 无法查询（裸 bundle / 后端未就绪 / 查询超时）
// NotDetermined = macOS 尚未询问用户
// Denied        = 用户已拒绝
// Authorized    = 已允许（macOS Provisional 归入此档）
enum class Authorization { Unknown, NotDetermined, Denied, Authorized };

// Deduped/RateLimited 只由 facade 产生；其余五种由后端产生。
enum class Status {
    Accepted,              // 系统受理；不保证用户看到（Focus/勿扰可能抑制）
    Deduped,               // 相同 id+内容在 2s 内重复
    RateLimited,           // 超过 3 条突发 / 1 条每秒
    NotBundled,            // macOS 裸二进制，没有有效 .app bundle
    PermissionDenied,      // macOS 用户拒绝通知授权
    NotDetermined,         // macOS 尚未决定；已异步发起授权请求
    BackendUnavailable,    // 托盘未就绪 / NIM_MODIFY 失败 / 平台未接入
};

struct Notification {
    std::string id;        // 可选；空 = 由内容派生稳定 id（devdesk-<fnv1a64 十六进制>）
    std::string title;     // 必填，单行（facade 清洗换行）
    std::string body;      // 必填，可多行
    bool sound = true;
    Level level = Level::Info;
};

struct Receipt {
    Status status = Status::BackendUnavailable;
    std::string detail;        // 中文说明，给工具/日志
    std::string effectiveId;   // 清洗/派生后的实际 id（去重键 + macOS identifier）
    bool titleTruncated = false;
    bool bodyTruncated = false;
    int retryAfterMs = 0;      // 仅 RateLimited 有意义
};

const char* statusToString(Status status);
const char* authorizationToString(Authorization auth);

// 主线程调用一次；幂等。macOS 安装 delegate/激活观察者、异步查授权并在
// NotDetermined 时异步请求（首次启动弹窗）。Windows/POSIX no-op。
// 非主线程调用只 LOG_WARN，不崩、仍执行。
void initialize();

// 任意线程；异步、幂等。macOS 内部封送主队列；Windows/POSIX no-op。
void requestAuthorization();

// 任意线程；macOS 可能阻塞 ≤1s 查询设置（有缓存），Windows/POSIX 读常量。
Authorization authorizationStatus();

// 任意线程。处理顺序固定：清洗/截断 → 授权检查 → 去重 → 限速 → 后端。
// 授权在最前（Denied 比 RateLimited 更有信息量）；去重在限速前（重复内容不消耗令牌）。
core::Result<Receipt> send(const Notification& notification);

// ---- 平台后端接口（测试可整对象替换，见 setBackendForTest）----
struct Backend {
    virtual ~Backend() = default;
    virtual void initialize() {}
    // 后端只应返回 Accepted / NotBundled / PermissionDenied /
    // NotDetermined / BackendUnavailable；Deduped/RateLimited 由 facade 产生。
    // Result::fail 只表示后端内部错误，facade 会映射成 BackendUnavailable。
    virtual core::Result<Status> send(const Notification&) = 0;
    virtual Authorization authorizationStatus() = 0;
    virtual void requestAuthorization() = 0;
};

// 默认后端 = 平台真实实现（由 notification_macos.mm / notification_win.cpp /
// notification_posix.cpp 各自定义）。返回进程级单例，永不为 nullptr，与进程同寿。
Backend& defaultBackend();

// 当前后端槽位。原子指针：后台线程可能正在发通知，测试换后端不应是数据竞争
//（生产路径只读）。静态局部量的初始化会调用 defaultBackend()，后者本身是
// 函数局部 static——无静态初始化顺序问题。
inline std::atomic<Backend*>& backendSlot() {
    static std::atomic<Backend*> slot{&defaultBackend()};
    return slot;
}

inline Backend& backend() {
    return *backendSlot().load(std::memory_order_relaxed);
}

// 测试注入：安装 b 并返回旧后端（便于 RAII 复位）。b == nullptr 等价于恢复默认后端。
// 仅测试调用；安装的对象必须活得比使用期长（不要传栈上临时对象的地址后立刻离开作用域）。
inline Backend* setBackendForTest(Backend* b) {
    return backendSlot().exchange(b ? b : &defaultBackend(),
                                  std::memory_order_relaxed);
}

// 测试复位：恢复默认后端（不清限速/去重/时钟，全量复位用 resetForTest）。
inline void resetBackendForTest() {
    backendSlot().store(&defaultBackend(), std::memory_order_relaxed);
}

// ---- 测试缝（仅测试调用；全部在 resetForTest 一并复位）----
using NowFn = std::function<std::chrono::steady_clock::time_point()>;

// 覆盖 facade 时钟（去重窗口/令牌桶用）；空 = steady_clock::now。
void setClockForTest(NowFn now);

// 清限速/去重/时钟覆盖、复位 initialize 幂等标记并恢复默认后端。
void resetForTest();

} // namespace platform::notify
