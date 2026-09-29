// src/module_todo/todo_ui_common.h
// Todo UI 两个绘制单元（todo_ui_board.cpp / todo_ui_dialogs.cpp）共用的渲染小件。
//
// 为什么在本模块而不是 ui_kit：它画的是**本模块领域**的东西（chips 的字体档位
// 来自 card 尺度的既有取值），不是可跨模块复用的通用组件；ui_kit 的收编对象见
// god-class spec §5.2 的判据（「只需要 tokens + 值类型才算跨模块」）。
// 纯绘制接线：不读 DB、不持状态、不捕获任何宿主对象。
#pragma once

#include <core/theme.h>
#include <eui/dsl.h>

#include <string>

#include "todo_ui_logic.h"  // kChipHeight / kChipFontInset（与 chipWidth 同源）

namespace module::todo::ui {

// 小标签（pill）——看板卡片（优先级/截止/标签）与详情弹窗标签区共用。
// 几何与字体档位逐字沿用拆分前 todo_module.cpp 的 drawChip；UI 整改 18 起
// 字号/高走 todo_ui_logic.h 的 kChipFontInset / kChipHeight（与 chipWidth
// 估宽同一份常量，改一处即两处同步）。
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
        .color(fg)
        .fontWeight(600)
        .opacity(opacity)
        .build();
}

} // namespace module::todo::ui
