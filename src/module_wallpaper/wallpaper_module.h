// src/module_wallpaper/wallpaper_module.h
// 壁纸页模块：Router 页面契约实现；能力判断只在平台层（create_wallpaper_module 消费）。
// 持有 WallpaperService（扫描/轮换/让路）与页面会话 UI 状态；compose 只做快照/命令
// 接线，页面元素树由 wallpaper_ui 的纯函数组装。
#pragma once

#include <string>

#include "core/module_interface.h"
#include "wallpaper_service.h"
#include "wallpaper_ui.h"

namespace module::wallpaper {

class WallpaperModule final : public ::core::Module {
public:
    ::core::PageId pageId() const override;
    std::string displayName() const override;
    std::string shortName() const override;
    std::string iconGlyph() const override;
    void compose(eui::Ui& ui, const eui::Screen& screen) override;
    void onStart() override;
    void onStop() override;

    // 托盘桥装配出口（main 装配段消费）：私有服务的只读转发。服务命令/快照均
    // 线程安全（见 wallpaper_service.h 线程模型），托盘回调线程调用合法；不得
    // 挪作页面装配/测试之外的用途（测试走 setPlayerForTest 一族缝）。
    WallpaperService& service() { return service_; }

private:
    WallpaperService service_;
    UiState uiState_;
};

} // namespace module::wallpaper