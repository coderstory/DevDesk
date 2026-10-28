// 壁纸托盘桥：托盘菜单 ↔ 壁纸模块私有服务的函数指针缝（形态同 module_toggle_bridge）。
// main.cpp 不可见 WallpaperService 类型，经本缝中立化：app 装配段注入真实现
// （读快照 + 转发命令），测试注入 recording 桩；未装配时查询报 absent、命令丢弃。
#pragma once
#include <type_traits>

namespace app::tray {

enum class WallpaperTrayCommand {
    TogglePause,  // → service.toggleManualPause（只切手动位；系统让路不因此解除）
    Next,         // → service.next
    Stop,         // → service.stop（幂等；恢复 = 设置页重拨壁纸模块开关）
};

// present 语义 = 壁纸模块实例已装配（平台能力通过）；实例在但服务已 stop 时仍为
// true——组保留、命令由服务自身幂等守卫吞掉（toggleManualPause/next 有 started_ 守卫）。
struct WallpaperTrayState {
    bool present = false;
    bool manualPaused = false;  // 播放/暂停文案口径，与壁纸页暂停钮同源
};

using WallpaperTrayStateProvider = WallpaperTrayState (*)(void);
using WallpaperTrayCommandHandler = void (*)(WallpaperTrayCommand);

// 双槽一次注入；任一传 nullptr = 该槽回厂缺省（查询 absent / 命令 no-op+LOG_WARN）
void setWallpaperTrayBridge(WallpaperTrayStateProvider state,
                            WallpaperTrayCommandHandler command);
WallpaperTrayState wallpaperTrayState();  // 未装配 = {false, false}
void dispatchWallpaperTrayCommand(WallpaperTrayCommand command);

}  // namespace app::tray

static_assert(std::is_same_v<decltype(&app::tray::wallpaperTrayState),
                             app::tray::WallpaperTrayState (*)(void)>,
              "wallpaper_tray_bridge: wallpaperTrayState 签名漂移");
static_assert(
    std::is_same_v<decltype(&app::tray::dispatchWallpaperTrayCommand),
                   void (*)(app::tray::WallpaperTrayCommand)>,
    "wallpaper_tray_bridge: dispatchWallpaperTrayCommand 签名漂移");
static_assert(
    std::is_same_v<decltype(&app::tray::setWallpaperTrayBridge),
                   void (*)(app::tray::WallpaperTrayStateProvider,
                            app::tray::WallpaperTrayCommandHandler)>,
    "wallpaper_tray_bridge: setWallpaperTrayBridge 签名漂移");
