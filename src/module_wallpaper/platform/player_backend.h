// src/module_wallpaper/platform/player_backend.h
// 播放后端接口（平台无关）。P2 只有 Null 实现；P3 接 mpv 时实现同一接口，
// WallpaperService 不感知进程/命名管道细节。
//
// 线程约束：调用方在 WallpaperService 的 playerMu_ 内串行调用（可与状态锁 mu_ 嵌套，
// 顺序固定为 mu_ → playerMu_）。实现必须保证每个方法快速返回——P3 mpv 实现应把阻塞
// 工作丢给自己的控制线程，不得在调用点同步等管道。
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace module::wallpaper {

class PlayerBackend {
public:
    virtual ~PlayerBackend() = default;

    // windowId = 宿主窗口句柄（Windows 为 HWND 的整数值）。
    virtual void start(std::uintptr_t windowId) = 0;
    virtual void load(const std::string& path) = 0;
    virtual void pause(bool paused) = 0;
    virtual void seek(float seconds) = 0;
    virtual void setRate(float rate) = 0;
    virtual void setVolume(float volume) = 0;
    virtual void setMute(bool muted) = 0;
    virtual void stop() = 0;
    // 当前播放位置（秒）。未装载/未实现时返回 0。
    virtual float positionSec() const = 0;
};

// Null 后端：全部 no-op，只记录调用，供单测断言「服务确实发出了哪条命令」。
// 记录容器有内部锁，任意线程调用安全；calls() 返回快照拷贝。
class NullPlayerBackend final : public PlayerBackend {
public:
    struct Call {
        std::string op;    // start/load/pause/seek/setRate/setVolume/setMute/stop
        std::string path;  // load 的目标
        double number = 0; // seek 秒数 / rate / volume / windowId
        bool flag = false; // pause / mute
    };

    void start(std::uintptr_t windowId) override;
    void load(const std::string& path) override;
    void pause(bool paused) override;
    void seek(float seconds) override;
    void setRate(float rate) override;
    void setVolume(float volume) override;
    void setMute(bool muted) override;
    void stop() override;
    float positionSec() const override;

    std::vector<Call> calls() const;
    void clearCalls();

    // 测试可设置「当前播放位置」，用于锚点/恢复用例。
    void setPositionForTest(float seconds);

private:
    mutable std::mutex mutex_;
    std::vector<Call> calls_;
    float position_ = 0.0f;
};

} // namespace module::wallpaper