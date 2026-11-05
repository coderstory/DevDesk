// src/module_terminal/terminal_page.h
// 页面构图 v2（need241）：单根 stack 绝对坐标——容器卡 glassFill（TAB 栏带 44 + 网格区
// 两段式）+ 多会话 TAB 栏（状态点/关闭钮独立命中）+ 右动作组（分隔线/设置/全屏或退出
// 胶囊/+新建）+ 逐 run 网格 + 选区/IME/光标（真实 cell 度量）+ 覆盖卡 + 空态。
// 全屏 = 侧栏 0 宽全隐（TAB 栏保留，gridTop 恒 44）；拖选→Ctrl/Cmd+C（F-04）；IBeam 光标
//（bug243）；IME 锚光标 cell（bug245）；block 字符逐列定位（bug244）。
#pragma once

#include <functional>
#include <string>

#include <eui/dsl.h>

#include "terminal_grid_widget.h"
#include "terminal_metrics.h"
#include "terminal_session_controller.h"

namespace terminal {

class TerminalPage {
public:
    // 装配整页。onKeyBytes=键编码产物；onTextInput=IME 提交/粘贴；onCopyText=选区文本
    //（Ctrl/Cmd+C 复制时回调，module 接剪贴板）；onOpenSettings=设置钮回调（module
    // 接 core::appRouter 导航，module 间零依赖）。
    GridLayout compose(eui::Ui& ui, const eui::Screen& screen, SessionController& controller,
                       const std::function<void(const uint8_t*, int32_t)>& onKeyBytes,
                       const std::function<void(const std::string&, bool paste)>& onTextInput,
                       const std::function<void(const std::string&)>& onCopyText,
                       const std::function<void()>& onOpenSettings = nullptr);

    bool wantsResize() const { return pendingRows_ > 0; }
    int pendingRows() const { return pendingRows_; }
    int pendingCols() const { return pendingCols_; }
    void clearPendingResize() { pendingRows_ = 0; pendingCols_ = 0; }

    // 全屏态（env 注入口 DEVDESK_TERMINAL_FULLSCREEN=1 初始化；两套视觉钮切换）。
    // 返回值变化时 module 调 sidebar 折叠缝。
    bool fullscreen() const { return fullscreen_; }
    void toggleFullscreen() { fullscreen_ = !fullscreen_; }
    // 拖选范围（module Cmd+C 经 controller.activeSelectionText 提取）
    bool hasSelection() const { return hasSelection_; }
    int selARow() const { return selARow_; }
    int selACol() const { return selACol_; }
    int selHRow() const { return selHRow_; }
    int selHCol() const { return selHCol_; }

private:
    float fontSize_ = 13.0f;
    TerminalCellMetrics metrics_{};  // 每帧刷新（fontSize 档缓存）
    TerminalGridWidget grid_;
    float scrollRemainder_ = 0.0f;
    int pendingRows_ = 0;
    int pendingCols_ = 0;
    bool fullscreen_ = false;
    bool fullscreenInit_ = false;
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
