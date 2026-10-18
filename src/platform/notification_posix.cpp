// src/platform/notification_posix.cpp
// 其它 POSIX 平台：无原生系统通知后端，诚实降级为 NotImplemented。
// 与 file_open_posix / tray_icon_posix 同一策略：不伪造成功，上层据此决定是否
// 回退到应用内 toast。
#include "notification.h"

namespace platform::notify {
namespace {

class UnsupportedBackend final : public Backend {
public:
    core::Result<void> send(const Notification&) override {
        return core::Result<void>::fail(
            core::ErrorCode::NotImplemented,
            "当前平台不支持系统通知（未提供原生通知后端）");
    }

    Authorization authorizationStatus() override {
        return Authorization::NotDetermined;
    }

    void requestAuthorization() override {}
};

} // namespace

Backend& defaultBackend() {
    static UnsupportedBackend backend;
    return backend;
}

} // namespace platform::notify
