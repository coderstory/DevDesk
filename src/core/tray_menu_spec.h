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
    ShowMainWindow,  // app::requestShow（隐藏到托盘/最小化/失焦三态统一唤起）
    NavigateHome,
    NavigateMarkdown,
    NavigateTodo,
    NavigateClaude,
    Exit,            // app::requestExit
};

struct TrayMenuSpecItem {
    std::string text;       // "-" = 分隔线（action 置 None，回调忽略）
    int iconCodepoint = 0;  // FontAwesome 码点，仅 styled 菜单消费
    TrayMenuAction action = TrayMenuAction::None;
};

// 顺序语义 = styled 菜单 path {i} 的映射基础：{0}显示主窗口 / {1}退出
inline std::vector<TrayMenuSpecItem> trayMenuSpec() {
    return {
        {"显示主窗口", 0xF2D2, TrayMenuAction::ShowMainWindow},  // fa-window-restore
        {"退出 DevDesk", 0xF011, TrayMenuAction::Exit},           // fa-power-off
    };
}

} // namespace core::tray
