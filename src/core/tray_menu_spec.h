// src/core/tray_menu_spec.h
// 托盘菜单唯一数据源（菜单美化 2026-09-23）：styled 独立菜单窗（主路径）
// 与原生托盘菜单（降级路径）都从本 spec 生成，杜绝两处文案/顺序漂移。
// 纯 std 数据结构（core 分层铁律：不依赖 EUI-NEO 运行时；iconCodepoint 是
// 裸 int，由消费方转 components::StyledMenuItem 的 FontAwesome 码点）。
// headless 断言菜单结构（项文本/图标/分隔线/动作路由）走本文件。
#pragma once

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

// 顺序语义 = styled 菜单 path {i} 的映射基础（分隔线占向量下标但不可选）：
// {0}显示主窗口 / {1}- / {2}首页 {3}笔记 {4}待办 {5}Claude / {6}- / {7}退出
inline std::vector<TrayMenuSpecItem> trayMenuSpec() {
    return {
        {"显示主窗口", 0xF2D2, TrayMenuAction::ShowMainWindow},  // fa-window-restore
        {"-", 0, TrayMenuAction::None},
        {"首页", 0xF015, TrayMenuAction::NavigateHome},           // fa-house
        {"笔记", 0xF249, TrayMenuAction::NavigateMarkdown},       // fa-note-sticky
        {"待办", 0xF0AE, TrayMenuAction::NavigateTodo},           // fa-list-check
        {"Claude", 0xF0E7, TrayMenuAction::NavigateClaude},       // fa-bolt
        {"-", 0, TrayMenuAction::None},
        {"退出 DevDesk", 0xF011, TrayMenuAction::Exit},           // fa-power-off
    };
}

} // namespace core::tray
