// src/module_terminal/terminal_page.h
// 页面构图 v2（need241）：单根 stack 绝对坐标——容器卡 glassFill（TAB 栏带 44 + 网格区
// 两段式）+ 多会话 TAB 栏（状态点/关闭钮独立命中）+ 右动作组（分隔线/设置/全屏或退出
// 胶囊/+新建）+ 逐 run 网格 + 选区/IME/光标（真实 cell 度量）+ 覆盖卡 + 空态。
// 全屏 = 侧栏 0 宽全隐（TAB 栏保留，gridTop 恒 44）；拖选→Ctrl/Cmd+C（F-04）；IBeam 光标
//（bug243）；IME 锚光标 cell（bug245）；block 字符逐列定位（bug244）。
#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include <eui/dsl.h>

#include "terminal_fullscreen_bridge.h"
#include "terminal_grid_widget.h"
#include "terminal_metrics.h"
#include "terminal_session_controller.h"
#include "terminal_settings.h"

namespace terminal {

class TerminalPage {
public:
    // 装配整页。onKeyBytes=键编码产物；onTextInput=IME 提交/粘贴；onCopyText=选区文本
    //（Ctrl/Cmd+C 复制时回调，module 接剪贴板）。settings=面板单一数据源（module 持有
    // + 持久化，need250）；onSettingsChanged=面板任意改动回调（module 落库 + 同步
    // controller 新会话参数）。设置钮 = 页内模态面板（openSettings），不再外跳导航。
    GridLayout compose(eui::Ui& ui, const eui::Screen& screen, SessionController& controller,
                       const std::function<void(const uint8_t*, int32_t)>& onKeyBytes,
                       const std::function<void(const std::string&, bool paste)>& onTextInput,
                       const std::function<void(const std::string&)>& onCopyText,
                       TerminalSettings& settings,
                       const std::function<void()>& onSettingsChanged);

    // 设置面板开合（齿轮钮 / Esc / × / 完成 三通道关闭；点 scrim 不关，设计 §2.1）
    void openSettings() { settingsOpen_ = true; }
    void closeSettings() { settingsOpen_ = false; }
    bool settingsOpen() const { return settingsOpen_; }

    // bug248：量化 resize 去抖 100ms——拖拽窗口时 layout 每帧变化，原实现每帧直接
    // resizeActive（ResizePseudoConsole + CancelIoEx 打断在飞 ReadFile），放大输入
    // 输出抖动；合并为 pending 静默 ≥100ms 后一次应用（首会话冷启动同样延迟 100ms，
    // 无感：ConPTY 侧另有 firstOutput 门控）。
    bool wantsResize() const {
        if (pendingRows_ <= 0) return false;
        const auto elapsed = std::chrono::steady_clock::now() - pendingSince_;
        return std::chrono::duration<float>(elapsed).count() >= 0.100f;
    }
    int pendingRows() const { return pendingRows_; }
    int pendingCols() const { return pendingCols_; }
    void clearPendingResize() {
        pendingRows_ = 0;
        pendingCols_ = 0;
        pendingTimer_ = false;
    }

    // 全屏态（env 注入口 DEVDESK_TERMINAL_FULLSCREEN=1 初始化；同槽图标钮切换）。
    bool fullscreen() const { return fullscreen_; }
    // bug252 根修（clue92 终裁：全屏 = 侧栏 0 宽全隐）：翻转的**当帧**打桥置位
    // g_sidebarFsHidden。原先桥在 module compose 尾部（drawSidebar 已按旧宽画完、
    // 页面已按旧 pageW 布局之后）才触发 → 过渡帧 =「侧栏旧宽 + 页已全屏」不一致态
    //（qa v2-04 截图侧栏仍完整可见即抓到该帧），侧栏比页面晚一帧折叠。toggle 即时
    // 打桥后，同一次 compose 里 drawSidebar/pageW 与页面同读同态，一帧对齐。
    // 未装配 handler（单测/无头 compose）时桥为默认桩静默丢弃，安全。
    void toggleFullscreen() {
        fullscreen_ = !fullscreen_;
        app::terminal_fullscreen::setCollapsed(fullscreen_);
    }
    // 拖选范围（module Cmd+C 经 controller.activeSelectionText 提取）
    bool hasSelection() const { return hasSelection_; }
    int selARow() const { return selARow_; }
    int selACol() const { return selACol_; }
    int selHRow() const { return selHRow_; }
    int selHCol() const { return selHCol_; }

private:
    TerminalCellMetrics metrics_{};  // 每帧刷新（(fontFamily,fontSize) 复合键缓存）
    TerminalGridWidget grid_;
    float scrollRemainder_ = 0.0f;
    int pendingRows_ = 0;
    int pendingCols_ = 0;
    std::chrono::steady_clock::time_point pendingSince_{}; // bug248：resize 去抖计时
    bool pendingTimer_ = false;
    std::vector<std::string> tabTitles_; // bug248：TAB 标题缓存（count 变化才重建）
    bool fullscreen_ = false;
    bool fullscreenInit_ = false;
    bool settingsOpen_ = false;      // need250 设置面板开合
    float panelScroll_ = 0.0f;       // 面板内容区滚动偏移（超高内滚）
    int lastActiveIndex_ = -2;       // 会话切换侦测（清选区/滚动余量）
    // 拖选（可视网格行列）
    bool selecting_ = false;
    bool hasSelection_ = false;
    int selARow_ = 0, selACol_ = 0, selHRow_ = 0, selHCol_ = 0;
    // TAB 栏交互态
    float tabScroll_ = 0.0f;         // TAB 区横滚偏移（溢出时 >0；栏视口态，切会话保位）
    int hoveredTabIndex_ = -1;       // 非激活 TAB hover（关闭钮淡入）
    int hoveredCloseIndex_ = -1;     // 关闭钮自身 hover（红警示面，P2-5）
    int lastTabCount_ = 0;           // TAB 数变化侦测（关 TAB 清 hover 记忆，P3-6）
};

} // namespace terminal
