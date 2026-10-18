// src/platform/notification_win.cpp
// Windows 系统通知：经 EUI-NEO 的托盘通知 API（Shell_NotifyIconW + NIF_INFO）。
//
// 行为边界（改动前先读）：
//   * Accepted 只代表**已交给 Shell**（EUI-NEO 入队并 PostMessage 到托盘窗口），
//     不代表用户看到了横幅。Windows 11 上是临时横幅，不进通知中心；Focus Assist
//     / 通知设置 / 「勿扰」都可能静默抑制，且不会回调告诉我们。
//   * 托盘未初始化、队列满、PostMessage 失败、内容不是合法 UTF-8 → EUI 返回 false，
//     本后端返回 BackendUnavailable（不假装成功）。调用方据此决定是否回退应用内 toast。
//   * 可从任意线程调用：EUI-NEO 内部封送到 UI 线程，本文件直接在当前线程调用即可，
//     **不要**自建线程 / 窗口 / SetTimer。
//   * 63/255 UTF-16 码元的截断由 facade 统一完成；EUI-NEO 侧再做防御性拷贝，
//     本后端不重复截断。
//   * Windows 没有应用级通知授权模型：authorizationStatus() 恒 Authorized（Focus
//     Assist 属于系统级抑制，不是「用户拒绝授权」），requestAuthorization() 为空。
//   * Notification::id 与 sound 在现有 EUI 桥无对应能力（NIF_INFO 没有通知 ID、
//     无声音开关）；dedupe 用的 effectiveId 由 facade 维护，不依赖本后端。
#include "notification.h"

#include <eui/platform.h>

namespace platform::notify {
namespace {

// EUI 的 TrayNotificationLevel 只有 Info/Warning/Error；Success 归入 Info
//（气泡没有「成功」图标语义，别为它发明映射）。
eui::platform::TrayNotificationLevel toEuiLevel(Level level) {
    switch (level) {
        case Level::Warning: return eui::platform::TrayNotificationLevel::Warning;
        case Level::Error: return eui::platform::TrayNotificationLevel::Error;
        case Level::Info:
        case Level::Success: return eui::platform::TrayNotificationLevel::Info;
    }
    return eui::platform::TrayNotificationLevel::Info;
}

class WinBackend final : public Backend {
public:
    core::Result<Status> send(const Notification& notification) override {
        // facade 已保证非空；这里再拦一次，保证直接持有 Backend& 的调用方
        // 也不会把空通知交给 Shell。
        if (notification.title.empty() || notification.body.empty()) {
            return core::Result<Status>::ok(Status::BackendUnavailable);
        }

        const bool accepted = eui::platform::showTrayNotification(
            notification.title, notification.body, toEuiLevel(notification.level));
        if (!accepted) {
            return core::Result<Status>::ok(Status::BackendUnavailable);
        }
        return core::Result<Status>::ok(Status::Accepted);
    }

    Authorization authorizationStatus() override {
        // Windows 无应用级授权三态；能走到这里就代表系统允许应用使用托盘气泡。
        return Authorization::Authorized;
    }

    void requestAuthorization() override {
        // Windows 没有授权弹窗；保留空实现以对齐接口。
    }

    void initialize() override {}
};

} // namespace

Backend& defaultBackend() {
    static WinBackend backend;
    return backend;
}

} // namespace platform::notify
