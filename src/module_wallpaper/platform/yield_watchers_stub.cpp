// src/module_wallpaper/platform/yield_watchers_stub.cpp
// 非 Windows 让路事件源 stub：没有 WTS 会话通知 / 电源状态查询的统一抽象，
// start 失败并给出中文原因；事件通道永不回调、轮询恒 false（宁可不让路）。
// 模块工厂在 supported()==false 的平台已不注册壁纸页；这里只保证接口符号不缺失。
#include "module_wallpaper/platform/yield_watchers.h"

namespace module::wallpaper {
namespace {

constexpr const char* kUnsupportedReason =
    "让路 watcher 仅支持 Windows（当前平台没有 WTS 会话通知）";

class YieldWatchersStub final : public YieldWatchers {
public:
    void setHoldCallback(HoldCallback callback) override { (void)callback; }

    void setDisplayChangedCallback(DisplayChangedCallback callback) override {
        (void)callback;
    }

    bool start(std::string* error) override {
        if (error != nullptr) *error = kUnsupportedReason;
        return false;
    }

    void stop() override {}

    bool pollOnBattery() override { return false; }

    bool pollFullscreenOtherProcess() override { return false; }
};

} // namespace

std::unique_ptr<YieldWatchers> createYieldWatchers() {
    return std::make_unique<YieldWatchersStub>();
}

} // namespace module::wallpaper
