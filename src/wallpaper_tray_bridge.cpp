// wallpaper_tray_bridge 的实现缝。独立 TU：main.cpp 消费、app_assembly.cpp 不引用，
// 探针/单测按需单独编入链接集（先例同 module_toggle_bridge.cpp）。缝本体零 app/EUI/
// 壁纸模块依赖。
#include "wallpaper_tray_bridge.h"

#include "core/logging.h"

namespace app::tray {

namespace {
WallpaperTrayStateProvider g_stateProvider = nullptr;
WallpaperTrayCommandHandler g_commandHandler = nullptr;
}  // namespace

void setWallpaperTrayBridge(WallpaperTrayStateProvider state,
                            WallpaperTrayCommandHandler command) {
    g_stateProvider = state;
    g_commandHandler = command;
}

WallpaperTrayState wallpaperTrayState() {
    if (g_stateProvider == nullptr) {
        return {false, false};
    }
    return g_stateProvider();
}

void dispatchWallpaperTrayCommand(WallpaperTrayCommand command) {
    if (g_commandHandler == nullptr) {
        LOG_WARN("壁纸托盘桥未装配（装配前/测试环境），命令丢弃");
        return;
    }
    g_commandHandler(command);
}

}  // namespace app::tray
