// src/module_wallpaper/platform/mpv_player.cpp
// PlayerBackend 到 MpvController 的委托实现（跨平台编译，行为随控制器平台实现）。
#include "module_wallpaper/platform/mpv_player.h"

#include <utility>

namespace module::wallpaper {

MpvPlayer::MpvPlayer() = default;

MpvPlayer::~MpvPlayer() = default;

void MpvPlayer::start(std::uintptr_t windowId) { controller_.start(windowId); }

void MpvPlayer::load(const std::string& path) { controller_.load(path); }

void MpvPlayer::pause(bool paused) { controller_.pause(paused); }

void MpvPlayer::seek(float seconds) { controller_.seek(seconds); }

void MpvPlayer::setRate(float rate) { controller_.setRate(rate); }

void MpvPlayer::setVolume(float volume) { controller_.setVolume(volume); }

void MpvPlayer::setMute(bool muted) { controller_.setMute(muted); }

void MpvPlayer::stop() { controller_.stop(); }

float MpvPlayer::positionSec() const { return controller_.positionSec(); }

bool MpvPlayer::takeError(std::string* error) {
    const std::string message = controller_.takeError();
    if (message.empty()) return false;
    if (error != nullptr) *error = message;
    return true;
}

bool MpvPlayer::processAlive() const { return controller_.processAlive(); }

std::uint32_t MpvPlayer::processId() const { return controller_.processId(); }

std::unique_ptr<PlayerBackend> createMpvPlayer() { return std::make_unique<MpvPlayer>(); }

} // namespace module::wallpaper