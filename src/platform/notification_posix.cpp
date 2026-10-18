// src/platform/notification_posix.cpp
// 其它 POSIX 平台：无原生系统通知后端，诚实降级为 BackendUnavailable。
// 与 file_open_posix / tray_icon_posix 同一策略：不伪造成功，上层据此决定是否
// 回退到应用内 toast。
#include "notification.h"

namespace platform::notify {
namespace {

class UnsupportedBackend final : public Backend {
public:
    core::Result<Status> send(const Notification&) override {
        return core::Result<Status>::ok(Status::BackendUnavailable);
    }

    Authorization authorizationStatus() override {
        return Authorization::Unknown;   // 无法查询 ≠ 未决定
    }

    void requestAuthorization() override {}
};

} // namespace

Backend& defaultBackend() {
    static UnsupportedBackend backend;
    return backend;
}

} // namespace platform::notify
