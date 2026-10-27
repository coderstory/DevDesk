// src/module_wallpaper/platform/mpv_controller_stub.cpp
// 非 Windows mpv 控制器 stub：没有进程/命名管道实现，start 只记录中文错误，
// 其余命令 no-op、位置恒 0。保证接口符号跨平台不缺失（模块工厂在非 Windows
// 平台本就不注册壁纸页）。
#include "module_wallpaper/platform/mpv_controller.h"

#include <mutex>
#include <string>

namespace module::wallpaper {

struct MpvController::Impl {
    std::mutex mutex;
    std::string error;
};

MpvController::MpvController() : impl_(std::make_unique<Impl>()) {}

MpvController::~MpvController() = default;

void MpvController::start(std::uintptr_t /*windowId*/) {
    if (!impl_) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->error = "视频播放引擎仅支持 Windows（当前平台未实现 mpv 进程与命名管道 IPC）";
}

void MpvController::stop() {}

void MpvController::load(const std::string& /*path*/, float /*seekSeconds*/) {}

void MpvController::pause(bool /*paused*/) {}

void MpvController::seek(float /*seconds*/) {}

void MpvController::setRate(float /*rate*/) {}

void MpvController::setVolume(float /*volume*/) {}

void MpvController::setMute(bool /*muted*/) {}

float MpvController::positionSec() const { return 0.0f; }

bool MpvController::processAlive() const { return false; }

std::uint32_t MpvController::processId() const { return 0; }

std::string MpvController::takeError() {
    if (!impl_) return std::string();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::string out = impl_->error;
    impl_->error.clear();
    return out;
}

} // namespace module::wallpaper