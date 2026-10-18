// src/platform/notification.h
// 平台系统通知抽象（macOS UNUserNotificationCenter / Windows 托盘待接入 / 其它 POSIX 不支持）
//
// 契约要点（调用方与后续工具层都要按这条理解，别把 ok 当成「用户看到了」）：
//   * send() 返回成功**只代表系统受理了请求**（macOS 是 addNotificationRequest 入队成功），
//     不代表通知已展示、更不代表用户看过。授权被拒时系统也可能静默丢弃。
//   * send() 可从任意线程调用；Windows 后端负责把调用封送到 UI 线程（本层接口不要求
//     调用方切线程）。macOS 端 UNUserNotificationCenter 本身线程安全。
//   * 不抛 C++ 异常，失败走 core::Result<void>。
#pragma once

#include <atomic>
#include <string>

#include <core/result.h>

namespace platform::notify {

struct Notification {
    // 稳定标识：同 id 的再次发送会替换未展示/已展示的同 id 通知（系统语义）。
    // 为空时由后端生成唯一标识（macOS：UUID）。
    std::string id;
    std::string title;
    std::string body;
    bool sound = true;
};

// 与 UNUserNotificationCenter 的授权三态对齐；Windows 后端当前恒 NotDetermined。
enum class Authorization { NotDetermined, Denied, Authorized };

// 平台后端接口。虚析构 + 纯虚方法：测试可整对象替换（见下方 setBackendForTest）。
struct Backend {
    virtual ~Backend() = default;
    virtual core::Result<void> send(const Notification&) = 0;
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

// 测试复位：恢复默认后端。
inline void resetBackendForTest() {
    backendSlot().store(&defaultBackend(), std::memory_order_relaxed);
}

// 调用入口（转发到当前后端）。title/body 为空在**进入后端之前**拦掉：
// 这不是平台差异，而是「发一条没有内容的通知」本身无意义；工具层后续还会做长度限制。
inline core::Result<void> send(const Notification& notification) {
    if (notification.title.empty()) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError,
                                        "通知标题（title）不能为空");
    }
    if (notification.body.empty()) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError,
                                        "通知正文（body）不能为空");
    }
    return backend().send(notification);
}

inline Authorization authorizationStatus() {
    return backend().authorizationStatus();
}

// 异步发起授权请求；不阻塞调用方。重复调用由系统幂等处理（已决定时不会再弹框）。
inline void requestAuthorization() {
    backend().requestAuthorization();
}

} // namespace platform::notify
