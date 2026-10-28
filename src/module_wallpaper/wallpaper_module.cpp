// src/module_wallpaper/wallpaper_module.cpp
#include "wallpaper_module.h"

#include "core/logging.h"
#include "platform/folder_picker.h"
#include "platform/wallpaper_capability.h"

namespace module::wallpaper {

::core::PageId WallpaperModule::pageId() const { return ::core::PageId::Wallpaper; }

std::string WallpaperModule::displayName() const { return "壁纸"; }

std::string WallpaperModule::shortName() const { return "壁纸"; }

// fa-image (U+F03E) 的 UTF-8 字节；须与 app_registry 表行 iconGlyph 列同值。
std::string WallpaperModule::iconGlyph() const { return "\xEF\x80\xBE"; }

// 生命周期结构契约：onStart 起服务（设置加载 + 后台扫描 + 计时），onStop 必须先收服务。
void WallpaperModule::onStart() {
    LOG_INFO("壁纸模块已启用：启动服务（扫描/轮换；播放后端为 Null）");
    service_.start();
}

void WallpaperModule::onStop() {
    LOG_INFO("壁纸模块已停止：收束服务线程");
    service_.stop();
}

// 页面只读快照、写命令：快照在 compose 一帧内取一次，UI 回调经 UiActions 回到服务。
// 会话状态（Tab/折叠/来源草稿/滚动位置）留在 uiState_，不持久化。
void WallpaperModule::compose(eui::Ui& ui, const eui::Screen& screen) {
    const WallpaperSnapshot snapshot = service_.snapshot();

    UiActions actions;
    actions.setKind = [this](WallpaperKind kind) { service_.setKind(kind); };
    actions.setMode = [this](PlayMode mode) { service_.setMode(mode); };
    actions.setInterval = [this](int seconds) {
        service_.setRotationIntervalSec(seconds);
    };
    actions.setRate = [this](float rate) { service_.setRate(rate); };
    actions.setVolume = [this](float volume) { service_.setVolume(volume); };
    actions.setMuted = [this](bool muted) { service_.setMuted(muted); };
    actions.setPauseOnBattery = [this](bool pauseOnBattery) {
        service_.setPauseOnBattery(pauseOnBattery);
    };
    actions.setImageMinPixels = [this](int minPixels) {
        service_.setImageMinPixels(minPixels);
    };
    actions.setImageFit = [this](ImageFit fit) { service_.setImageFit(fit); };
    actions.selectFolder = [this] {
        const WallpaperSnapshot current = service_.snapshot();
        const std::string title = current.kind == WallpaperKind::Image
                                      ? "选择图片文件夹"
                                      : "选择壁纸文件夹";
        auto chosen = ::platform::folderpicker::pickerSlot()(
            title, current.folderPath);
        if (chosen.isFail()) return;  // 用户取消：静默返回
        std::string error;
        if (!service_.selectFolder(chosen.value(), &error)) {
            LOG_WARN("壁纸: 选择文件夹失败: " + error);
        }
    };
    actions.rescan = [this] { service_.rescan(); };
    actions.togglePause = [this] { service_.toggleManualPause(); };
    actions.next = [this] { service_.next(); };

    composeWallpaperUi(ui, screen, snapshot, actions, uiState_);
}

} // namespace module::wallpaper

// 能力不支持时返回 nullptr（装配层按空实例跳过，页面不出现）；业务侧零平台宏。
extern "C" ::core::Module* create_wallpaper_module() {
    if (!::platform::wallpaper::supported()) return nullptr;
    return new module::wallpaper::WallpaperModule();
}