// src/module_wallpaper/platform/wallpaper_host_stub.cpp
// 非 Windows 宿主 stub：没有 WorkerW 桌面层，start 必然失败并给出中文原因。
// 模块工厂在 supported()==false 的平台已不注册壁纸页；这里只保证接口符号不缺失、
// 误用时不崩且错误可展示（macOS 第二阶段 ObjC++ 实现之前的行为）。
#include "module_wallpaper/platform/wallpaper_host.h"

namespace module::wallpaper {
namespace {

constexpr const char* kUnsupportedReason =
    "壁纸宿主仅支持 Windows（当前平台没有 WorkerW 桌面层）";

class WallpaperHostStub final : public WallpaperHost {
public:
    bool start(std::string* error) override {
        if (error != nullptr) *error = kUnsupportedReason;
        return false;
    }

    void stop() override {}

    bool show(std::string* error) override {
        if (error != nullptr) *error = kUnsupportedReason;
        return false;
    }

    void hide() override {}

    HostHandle handle() const override { return HostHandle{}; }

    void rebuild(std::string* error) override {
        if (error != nullptr) *error = kUnsupportedReason;
    }
};

} // namespace

std::unique_ptr<WallpaperHost> createWallpaperHost() {
    return std::make_unique<WallpaperHostStub>();
}

} // namespace module::wallpaper