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
    WallpaperDeleteCurrent, // 删除当前壁纸素材（need356：先弹确认，不直接删）
    Exit,                 // app::requestExit
};

struct TrayMenuSpecItem {
    std::string text;       // "-" = 分隔线（action 置 None，回调忽略）
    int iconCodepoint = 0;  // FontAwesome 码点，仅 styled 菜单消费
    TrayMenuAction action = TrayMenuAction::None;
};

// 顺序语义 = styled 菜单 path {i} 的映射基础（need242，2026-11-05 两组整体对调）：
// 组开时壁纸组在上——{0}下一张 / {1}播放暂停 / {2}停止 / {3}打开壁纸页，
// need356：canDelete 时 {4}删除当前视频…/删除当前图片…（动态隐藏，见下），
// {5}分隔线，{6}显示主窗口 / {7}退出 DevDesk 垫尾（显示主窗口仍在退出上面，
// 常规托盘惯例）；组关时只有 {0}显示主窗口 / {1}退出 两项。壁纸组仅当
// wallpaperGroupOn 为 true 才出现；下标随组开关整体偏移，消费方一律走 spec
// 按值映射，禁写死下标。
// 删除项显隐：canDelete=false（默认）= 现 7 项逐字节不变，删除项不占位；
// canDelete=true = 8 项。文案按 currentIsImage 分派（视频/图片），两者共用
// fa-trash 0xF1F8。
// 播放/暂停文案与图标口径 = wallpaperManualPaused（与壁纸页状态卡暂停钮同源，
// wallpaper_ui 状态卡），刻意不用 paused（held）驱动：系统让路下 held=true 而桥命令
// （toggleManualPause）只切手动位、不解除系统让路，held 驱动会让「继续」字样配出
// 「落暂停位」的反向效果。
inline std::vector<TrayMenuSpecItem> trayMenuSpec(bool wallpaperGroupOn,
                                                  bool wallpaperManualPaused,
                                                  bool canDelete = false,
                                                  bool currentIsImage = false) {
    if (!wallpaperGroupOn) {
        return {
            {"显示主窗口", 0xF2D2, TrayMenuAction::ShowMainWindow},  // fa-window-restore
            {"退出 DevDesk", 0xF011, TrayMenuAction::Exit},           // fa-power-off
        };
    }
    std::vector<TrayMenuSpecItem> items = {
        {"下一张", 0xF051, TrayMenuAction::WallpaperNext},           // fa-step-forward
        {wallpaperManualPaused ? "继续壁纸" : "暂停壁纸",
         wallpaperManualPaused ? 0xF04B : 0xF04C,
         TrayMenuAction::WallpaperTogglePause},  // fa-play / fa-pause
        {"停止壁纸", 0xF04D, TrayMenuAction::WallpaperStop},         // fa-stop
        {"打开壁纸页", 0xF03E, TrayMenuAction::NavigateWallpaper},   // fa-image
    };
    if (canDelete) {
        items.push_back({currentIsImage ? "删除当前图片…" : "删除当前视频…",
                         0xF1F8, TrayMenuAction::WallpaperDeleteCurrent});  // fa-trash
    }
    items.push_back({"-", 0, TrayMenuAction::None});
    items.push_back({"显示主窗口", 0xF2D2, TrayMenuAction::ShowMainWindow});  // fa-window-restore
    items.push_back({"退出 DevDesk", 0xF011, TrayMenuAction::Exit});          // fa-power-off
    return items;
}

// 缺省形状 = 无壁纸组（原生降级菜单在装配前一次性构建、headless 断言基线）
inline std::vector<TrayMenuSpecItem> trayMenuSpec() {
    return trayMenuSpec(false, false);
}

} // namespace core::tray
