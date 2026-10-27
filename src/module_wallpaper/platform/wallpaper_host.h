// src/module_wallpaper/platform/wallpaper_host.h
// 桌面壁纸宿主接口（平台无关）。Windows 实现 = WorkerW 下的宿主/视频/图片子窗口；
// 其它平台返回 stub（start 必失败并给出中文原因，模块工厂已在平台层判 supported）。
//
// 线程契约：所有方法允许任意线程调用，实现内部把窗口操作串行化到宿主线程；
//   handle() 返回最近一次成功创建后的快照（未启动/已停止时全空）。
// 生命周期：start/stop 均幂等，stop 会 join 宿主线程；stop 之后可再次 start。
#pragma once

#include <memory>
#include <string>

namespace module::wallpaper {

struct HostHandle {
    void* videoChild = nullptr;  // mpv --wid 目标（HWND）；未就绪为 nullptr
    void* imageChild = nullptr;  // 图片层（3b 使用）
    int width = 0;               // 物理像素
    int height = 0;
};

class WallpaperHost {
public:
    virtual ~WallpaperHost() = default;

    // 建消息窗 + 找 WorkerW + 建宿主/子窗口（全部在宿主线程完成）。失败填中文 error。
    virtual bool start(std::string* error) = 0;
    // 销毁宿主/消息窗并 join 宿主线程；幂等。
    virtual void stop() = 0;
    virtual bool show(std::string* error) = 0;
    virtual void hide() = 0;
    // 最近一次成功创建的子窗口句柄与尺寸；未启动/已停止返回全空。
    virtual HostHandle handle() const = 0;
    // Explorer 重启/分辨率变化后重建（销毁旧窗口 → 重新找 WorkerW → 建新窗口）。
    // 重建期间保持原可见性；失败时 handle() 变空并填中文 error。
    virtual void rebuild(std::string* error) = 0;
};

// Windows 返回真实实现；其它平台返回 stub。
std::unique_ptr<WallpaperHost> createWallpaperHost();

} // namespace module::wallpaper