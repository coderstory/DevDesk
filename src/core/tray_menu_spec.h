// 托盘菜单的唯一数据源：styled 独立菜单窗（主路径）与原生托盘菜单（降级路径）
// 都从本 spec 生成，杜绝文案/顺序漂移。纯 std 数据结构（core 不依赖 EUI-NEO
// 运行时，iconCodepoint 为裸 int），headless 断言菜单结构走本文件。
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace core::tray {

enum class TrayMenuAction {
    None,
    ShowMainWindow,       // app::requestShow（隐藏到托盘/最小化/失焦三态统一唤起）
    NavigateHome,
    NavigateMarkdown,
    NavigateTodo,
    NavigateClaude,
    NavigateWallpaper,    // 打开壁纸页（pageForTrayAction 映射；未注册时回首页+toast）
    WallpaperTogglePause, // 壁纸播放/暂停（壁纸托盘桥 → service.toggleManualPause）
    WallpaperNext,        // 壁纸下一张（壁纸托盘桥 → service.next）
    WallpaperStop,        // 停止壁纸（壁纸托盘桥 → service.stop；恢复 = 设置页重拨模块开关）
    Exit,                 // app::requestExit
};

struct TrayMenuSpecItem {
    std::string text;       // "-" = 分隔线（action 置 None，回调忽略）
    int iconCodepoint = 0;  // FontAwesome 码点，仅 styled 菜单消费
    TrayMenuAction action = TrayMenuAction::None;
};

// 顺序语义 = styled 菜单 path {i} 的映射基础：{0}显示主窗口 / {1}退出。
// 壁纸组（分隔线 + 播放暂停/下一张/停止/打开页）仅当 wallpaperModulePresent 为 true
// 才追加在尾部；前两位下标在任何开关组合下不变。
// 播放/暂停文案与图标口径 = wallpaperManualPaused（与壁纸页状态卡暂停钮同源，
// wallpaper_ui 状态卡），刻意不用 paused（held）驱动：系统让路下 held=true 而桥命令
// （toggleManualPause）只切手动位、不解除系统让路，held 驱动会让「继续」字样配出
// 「落暂停位」的反向效果。
inline std::vector<TrayMenuSpecItem> trayMenuSpec(bool wallpaperModulePresent,
                                                  bool wallpaperManualPaused) {
    std::vector<TrayMenuSpecItem> items = {
        {"显示主窗口", 0xF2D2, TrayMenuAction::ShowMainWindow},  // fa-window-restore
        {"退出 DevDesk", 0xF011, TrayMenuAction::Exit},           // fa-power-off
    };
    if (!wallpaperModulePresent) {
        return items;
    }
    items.push_back({"-", 0, TrayMenuAction::None});
    items.push_back({wallpaperManualPaused ? "继续壁纸" : "暂停壁纸",
                     wallpaperManualPaused ? 0xF04B : 0xF04C,
                     TrayMenuAction::WallpaperTogglePause});  // fa-play / fa-pause
    items.push_back({"下一张", 0xF051, TrayMenuAction::WallpaperNext});      // fa-step-forward
    items.push_back({"停止壁纸", 0xF04D, TrayMenuAction::WallpaperStop});    // fa-stop
    items.push_back({"打开壁纸页", 0xF03E, TrayMenuAction::NavigateWallpaper});  // fa-image
    return items;
}

// 缺省形状 = 无壁纸组（原生降级菜单在装配前一次性构建、headless 断言基线）
inline std::vector<TrayMenuSpecItem> trayMenuSpec() {
    return trayMenuSpec(false, false);
}

} // namespace core::tray
