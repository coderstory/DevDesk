// src/module_terminal/terminal_page.h
// 页面构图（Phase 2）：单根 stack 绝对坐标布局——深底 rect 铺底层 + 工具栏 44px 带 +
// 网格体 + 选区 rects + 悬浮退出钮（全屏态）+ 覆盖卡。拖选→Cmd+C（F-04 硬验收）、
// imeRect 锚光标 cell（POC-5 收尾）、全屏态经 bridge 缝控侧栏（F-05）。
#pragma once

#include <functional>
#include <string>

#include <eui/dsl.h>

#include "terminal_grid_widget.h"
#include "terminal_session_controller.h"

namespace terminal {

class TerminalPage {
public:
    // 装配整页。onKeyBytes=键编码产物；onTextInput=IME 提交/粘贴；onCopyText=选区文本
    //（Cmd+C 时回调，module 接剪贴板）；placeholderReason 非空=占位页（Windows）。
    GridLayout compose(eui::Ui& ui, const eui::Screen& screen, SessionController& controller,
                       const std::function<void(const uint8_t*, int32_t)>& onKeyBytes,
                       const std::function<void(const std::string&, bool paste)>& onTextInput,
                       const std::function<void(const std::string&)>& onCopyText,
                       const std::string& placeholderReason);

    bool wantsResize() const { return pendingRows_ > 0; }
    int pendingRows() const { return pendingRows_; }
    int pendingCols() const { return pendingCols_; }
    void clearPendingResize() { pendingRows_ = 0; pendingCols_ = 0; }

    // 全屏态（env 注入口 DEVDESK_TERMINAL_FULLSCREEN=1 初始化；悬浮钮双向切换）。
    // 返回值变化时 module 调 sidebar 折叠缝。
    bool fullscreen() const { return fullscreen_; }
    void toggleFullscreen() { fullscreen_ = !fullscreen_; }
    // 拖选范围（module Cmd+C 经 controller.selectionText 提取）
    bool hasSelection() const { return hasSelection_; }
    int selARow() const { return selARow_; }
    int selACol() const { return selACol_; }
    int selHRow() const { return selHRow_; }
    int selHCol() const { return selHCol_; }

private:
    float fontSize_ = 13.0f;
    TerminalGridWidget grid_;
    int scrollOffset_ = 0;
    float scrollRemainder_ = 0.0f;
    int pendingRows_ = 0;
    int pendingCols_ = 0;
    bool fullscreen_ = false;
    bool fullscreenInit_ = false;
    // 拖选（可视网格行列）
    bool selecting_ = false;
    bool hasSelection_ = false;
    int selARow_ = 0, selACol_ = 0, selHRow_ = 0, selHCol_ = 0;
};

} // namespace terminal
