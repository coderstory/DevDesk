// src/core/tray_menu_spec.h
// 托盘菜单唯一数据源（菜单美化 2026-09-23；审计 [7] 动态收缩 2026-09-24）：
// styled 独立菜单窗（主路径）与原生托盘菜单（降级路径）都从本 spec 生成，
// 杜绝两处文案/顺序漂移。
// 纯 std 数据结构（core 分层铁律：不依赖 EUI-NEO 运行时；iconCodepoint 是
// 裸 int，由消费方转 components::StyledMenuItem 的 FontAwesome 码点）。
// headless 断言菜单结构（项文本/图标/分隔线/动作路由/可用性收缩）走本文件。
//
// 审计 [7]：菜单项不再写死"恒全量"——消费方传入可用性谓词按模块实际
// 注册状态生成，与侧边栏（Router::modules() 动态渲染）收缩同源：
//   - trayMenuSpec(pred)：不可用的导航项保留但置 disabled、文本追加
//     「（已关闭）」说明（styled 路径置灰不可点，解释自明）；
//   - trayMenuSpecFiltered(pred)：不可用项整体移除（原生 TrayMenuItem
//     无置灰概念，降级路径只出不显示）。
// 无参 trayMenuSpec() = 全可用基线（行为与菜单美化批一致）。
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
    // true = 对应模块未注册/未启用：styled 渲染置灰不可选（文本已带说明
    // 后缀）；原生降级路径经 trayMenuSpecFiltered 过滤掉不显示
    bool disabled = false;
};

// 禁用项文本后缀（显示与测试共用单一真值）
inline const char* trayDisabledHintSuffix() { return "（已关闭）"; }

// 是否受模块开关门控的导航项（可用性谓词只对它生效；
// 首页恒注册但同样交给谓词裁决，spec 不内置"谁能关"的知识）
inline bool isNavigationAction(TrayMenuAction action) {
    return action == TrayMenuAction::NavigateHome ||
           action == TrayMenuAction::NavigateMarkdown ||
           action == TrayMenuAction::NavigateTodo ||
           action == TrayMenuAction::NavigateClaude;
}

// 顺序语义 = styled 菜单 path {i} 的映射基础：
// {0}显示主窗口 / {1}退出（2026-09-24 精简：去掉模块导航项与分隔线，
// 只留打开主窗口 + 退出两项——用户反馈导航走侧边栏即可，托盘只管开/关）
inline std::vector<TrayMenuSpecItem> trayMenuSpec() {
    return {
        {"显示主窗口", 0xF2D2, TrayMenuAction::ShowMainWindow},  // fa-window-restore
        {"退出 DevDesk", 0xF011, TrayMenuAction::Exit},           // fa-power-off
    };
}

// 按可用性谓词生成（styled 主路径数据源）：isAvailable 返回 false 的导航项
// 置 disabled 并在文本后追加「（已关闭）」——项保留、置灰、自带解释。
// 谓词为空 = 全可用（等价无参基线）。下标语义与基线一致，path {i} 映射不受
// 影响（禁用项仍占下标，styled 对 disabled 行不挂点击）。
inline std::vector<TrayMenuSpecItem> trayMenuSpec(
    const std::function<bool(TrayMenuAction)>& isAvailable) {
    std::vector<TrayMenuSpecItem> spec = trayMenuSpec();
    if (!isAvailable) {
        return spec;
    }
    for (auto& item : spec) {
        if (isNavigationAction(item.action) && !isAvailable(item.action)) {
            item.disabled = true;
            item.text += trayDisabledHintSuffix();
        }
    }
    return spec;
}

// 原生降级路径数据源：在谓词生成结果上移除禁用项（平台 TrayMenuItem 只有
// text+callback，无置灰概念）。全部导航项禁用时相邻分隔线合并，避免连续
// 两条 "-" 的观感噪声。
inline std::vector<TrayMenuSpecItem> trayMenuSpecFiltered(
    const std::function<bool(TrayMenuAction)>& isAvailable) {
    std::vector<TrayMenuSpecItem> out;
    for (const auto& item : trayMenuSpec(isAvailable)) {
        if (item.disabled) {
            continue;
        }
        // 合并相邻分隔线（首条保留，第二条起跳过）
        if (item.text == "-" && !out.empty() && out.back().text == "-") {
            continue;
        }
        out.push_back(item);
    }
    return out;
}

} // namespace core::tray
