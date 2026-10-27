// src/module_wallpaper/platform/mpv_player.h
// PlayerBackend → MpvController 的薄适配。服务只认识 PlayerBackend；
// 进程/管道细节留在控制器层。所有方法快速返回（stop 例外：需等进程退出）。
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "module_wallpaper/platform/mpv_controller.h"
#include "module_wallpaper/platform/player_backend.h"

namespace module::wallpaper {

class MpvPlayer final : public PlayerBackend {
public:
    MpvPlayer();
    ~MpvPlayer() override;
    MpvPlayer(const MpvPlayer&) = delete;
    MpvPlayer& operator=(const MpvPlayer&) = delete;

    void start(std::uintptr_t windowId) override;
    void load(const std::string& path) override;
    void pause(bool paused) override;
    void seek(float seconds) override;
    void setRate(float rate) override;
    void setVolume(float volume) override;
    void setMute(bool muted) override;
    void stop() override;
    float positionSec() const override;
    bool takeError(std::string* error) override;

    // 诊断（桌面探针用）：本次 mpv 进程是否仍存活 / 进程号（0 = 未启动）。
    bool processAlive() const;
    std::uint32_t processId() const;

private:
    MpvController controller_;
};

std::unique_ptr<PlayerBackend> createMpvPlayer();

} // namespace module::wallpaper