// src/module_todo/todo_ui_common.h
// Todo UI 两个绘制单元（todo_ui_board.cpp / todo_ui_dialogs.cpp）共用的渲染小件。
// 在本模块而非 ui_kit：画的是本模块领域的东西（chips 字体档位来自 card 尺度
// 既有取值），不是可跨模块复用的通用组件。
// 纯绘制接线：不读 DB、不持状态、不捕获任何宿主对象。
#pragma once

#include <core/theme.h>
#include <eui/dsl.h>

#include <string>

#include "todo_ui_logic.h"  // kChipHeight / kChipFontInset（与 chipWidth 同源）

namespace module::todo::ui {

// 小标签（pill）——看板卡片（优先级/截止/标签）与详情弹窗标签区共用。
// 字号/高走 kChipFontInset / kChipHeight（与 chipWidth 估宽同一份常量，
// 改一处即两处同步）。
inline void drawChip(eui::Ui& ui,
                     const std::string& id,
                     float x,
                     float y,
                     float w,
                     const core::theme::Tokens& t,
                     const std::string& text,
                     const eui::Color& fg,
                     const eui::Color& bg,
                     float opacity = 1.0f) {
    ui.rect(id + ".bg")
        .position(x, y)
        .size(w, kChipHeight)
        .color(bg)
        .radius(t.sizes.radiusPill)
        .opacity(opacity)
        .build();
    ui.text(id + ".text")
        .position(x, y)
        .size(w, kChipHeight)
        .text(text)
        .fontSize(t.sizes.fontSizeSmall - kChipFontInset)
        .lineHeight(kChipHeight)
        .horizontalAlign(eui::HorizontalAlign::Center)
        // 文本盒 == 胶囊高，须显式垂直居中——缺它 ink 从行框顶沿起排 → 字偏上
        //（本 helper 是卡片/弹窗 chip 的唯一绘制点，改这里 = 三处一起修）
        .verticalAlign(eui::VerticalAlign::Center)
        .color(fg)
        .fontWeight(600)
        .opacity(opacity)
        .build();
}

} // namespace module::todo::ui
