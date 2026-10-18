// src/platform/notification_win.cpp
// Windows 系统通知：经 EUI-NEO 的托盘通知 API（Shell_NotifyIconW + NIF_INFO）。
//
// 行为边界（改动前先读）：
//   * send() 成功只代表**已交给 Shell**（EUI-NEO 入队并 PostMessage 到托盘窗口），
//     不代表用户看到了横幅。Windows 11 上是临时横幅，不进通知中心；Focus Assist
//     / 通知设置 / 「勿扰」都可能静默抑制，且不会回调告诉我们。
//   * 托盘未初始化、队列满、PostMessage 失败、内容不是合法 UTF-8 → EUI 返回 false，
//     本后端如实失败（不假装成功）。楼层调用方据此决定是否回退应用内 toast。
//   * 可从任意线程调用：EUI-NEO 内部用 SRWLOCK 队列 + PostMessageW 封送到 UI 线程，
//     本文件直接在当前线程调用即可，**不要**自建线程 / 窗口 / SetTimer。
//   * 标题/正文上限 63/255 个 UTF-16 码元，由 EUI-NEO 严格校验 UTF-8 并安全截断
//     （不切开代理对）；本后端不再重复截断。
//   * Windows 没有应用级通知授权模型：authorizationStatus() 恒 Authorized（Focus
//     Assist 属于系统级抑制，不是「用户拒绝授权」），requestAuthorization() 为空。
//   * Notification::id 与 sound 在 v1 无对应能力：NIF_INFO 没有通知 ID（不伪造去重），
//     也没有声音开关（Shell 默认播提示音，sound=false 被忽略）。
#include "notification.h"

#include <eui/platform.h>

namespace platform::notify {
namespace {

class WinBackend final : public Backend {
public:
    core::Result<void> send(const Notification& notification) override {
        // 入口函数已拦空串；后端边界再拦一次，保证直接持有 Backend& 的调用方
        // 也拿到同样的中文错误，而不是把空通知交给 Shell。
        if (notification.title.empty()) {
            return core::Result<void>::fail(core::ErrorCode::ValidationError,
                                            "通知标题（title）不能为空");
        }
        if (notification.body.empty()) {
            return core::Result<void>::fail(core::ErrorCode::ValidationError,
                                            "通知正文（body）不能为空");
        }

        // id / sound 按文件头说明忽略；通知级别固定 Info（业务层当前没有错误级别语义）。
        const bool accepted = eui::platform::showTrayNotification(
            notification.title, notification.body,
            eui::platform::TrayNotificationLevel::Info);
        if (!accepted) {
            return core::Result<void>::fail(
                core::ErrorCode::Internal,
                "托盘通知不可用（托盘未初始化、队列已满、内容不是合法 UTF-8 或系统拒绝受理）");
        }
        return core::Result<void>::ok();
    }

    Authorization authorizationStatus() override {
        // Windows 无应用级授权三态；能走到这里就代表系统允许应用使用托盘气泡。
        return Authorization::Authorized;
    }

    void requestAuthorization() override {
        // Windows 没有授权弹窗；保留空实现以对齐接口。
    }
};

} // namespace

Backend& defaultBackend() {
    static WinBackend backend;
    return backend;
}

} // namespace platform::notify
