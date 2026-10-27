// src/module_wallpaper/wallpaper_module.cpp
#include "wallpaper_module.h"

#include <algorithm>

#include "core/logging.h"
#include "core/theme.h"
#include "platform/wallpaper_capability.h"

#include <eui/dsl.h>

namespace module::wallpaper {

::core::PageId WallpaperModule::pageId() const { return ::core::PageId::Wallpaper; }

std::string WallpaperModule::displayName() const { return "壁纸"; }

std::string WallpaperModule::shortName() const { return "壁纸"; }

// fa-image (U+F03E) 的 UTF-8 字节；须与 app_registry 表行 iconGlyph 列同值。
std::string WallpaperModule::iconGlyph() const { return "\xEF\x80\xBE"; }

// 生命周期结构契约：onStart 起服务（设置加载 + 后台扫描 + 计时），onStop 必须先收服务。
void WallpaperModule::onStart() {
    LOG_INFO("壁纸模块已启用：启动服务骨架（扫描/轮换；播放后端为 Null）");
    service_.start();
}

void WallpaperModule::onStop() {
    LOG_INFO("壁纸模块已停止：收束服务线程");
    service_.stop();
}

void WallpaperModule::compose(eui::Ui& ui, const eui::Screen& screen) {
    const auto& t = core::theme::current();
    constexpr float kTitleH = 32.0f;
    constexpr float kHintH = 24.0f;
    // 页内边距与其余页面同一口径（spacingLarge × 2 = 32），内容宽度随窗口弹性收缩。
    const float pagePad = t.sizes.spacingLarge * 2.0f;
    const float contentW = std::max(0.0f, screen.width - pagePad * 2.0f);

    ui.text("wallpaper.title")
        .position(pagePad, 0.0f)
        .size(contentW, kTitleH)
        .text("壁纸")
        .fontSize(t.sizes.fontSizeTitle)
        .lineHeight(kTitleH)
        .fontWeight(700)
        .color(t.colors.textPrimary)
        .build();

    ui.text("wallpaper.hint")
        .position(pagePad, kTitleH + t.sizes.spacingLarge)
        .size(contentW, kHintH)
        .text("壁纸模块已启用，片库与播放将在后续版本接入")
        .fontSize(t.sizes.fontSizeBody)
        .lineHeight(kHintH)
        .color(t.colors.textSecondary)
        .build();
}

} // namespace module::wallpaper

// 能力不支持时返回 nullptr（装配层按空实例跳过，页面不出现）；业务侧零平台宏。
extern "C" ::core::Module* create_wallpaper_module() {
    if (!::platform::wallpaper::supported()) return nullptr;
    return new module::wallpaper::WallpaperModule();
}