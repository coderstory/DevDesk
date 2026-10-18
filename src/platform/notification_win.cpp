// src/platform/notification_win.cpp
// Windows 系统通知：**仅占位 stub**（本任务不接入真实托盘/Toast 实现）。
//
// 后续接入时注意接口契约：send() 可从任意线程调用，Windows 后端负责把真正创建
// 通知窗口/托盘气泡的调用封送到 UI 线程（本 stub 无需考虑）。在真实实现落地前，
// 一律诚实返回 NotImplemented，绝不假装成功（否则上层无法区分「已受理」与「没做」）。
#include "notification.h"

namespace platform::notify {
namespace {

class StubBackend final : public Backend {
public:
    core::Result<void> send(const Notification&) override {
        return core::Result<void>::fail(
            core::ErrorCode::NotImplemented,
            "当前平台暂未接入系统通知（Windows 后端待实现）");
    }

    Authorization authorizationStatus() override {
        return Authorization::NotDetermined;
    }

    void requestAuthorization() override {}
};

} // namespace

Backend& defaultBackend() {
    static StubBackend backend;
    return backend;
}

} // namespace platform::notify
