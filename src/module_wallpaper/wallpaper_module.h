// src/module_wallpaper/wallpaper_module.h
// 壁纸页模块：Router 页面契约实现；能力判断只在平台层（create_wallpaper_module 消费）。
#pragma once

#include <string>

#include "core/module_interface.h"

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
};

} // namespace module::wallpaper