// src/ui_kit/selection_bar.cpp — 列表选中态 helper 实现（S2.5）。
// 色值全部走 core::theme::Tokens 主槽位，禁裸 hex。
// 既有页面选中态调用不动；本 helper 供 W3/W4 逐页切换调用。
#include <ui_kit/selection_bar.h>

namespace ui_kit {

void selectionBar(eui::Ui& ui, const std::string& id, const std::string& label,
                  float x, float y, float w, float h,
                  const core::theme::Tokens& t) {

    // S2.5 + H5：选中 = 白底 elevated 卡（surface + border）+ 左 3px primary 竖线 + primary 文字。
    // H5 把底从 primarySoftBg（#E6FBF2）改为 surface（#FFFFFF）加 border：
    // 淡绿侧栏底（#F0FDF7）上 primarySoftBg 分不清，白卡 + 描边对比成立。
    ui.stack(id)
        .position(x, y)
        .size(w, h)
        .content([&] {
            // 底色 rect：surface 白卡 + border 描边（elevated card 观感）
            ui.rect(id + ".bg")
                .size(w, h)
                .color(t.colors.surface)
                .border(1.0f, t.colors.borderDefault)
                .build();

            // 左 3px 竖线：primary（#36D399）
            ui.rect(id + ".accent")
                .position(0.0f, 0.0f)
                .size(kSelectionBarAccentWidth, h)
                .color(t.colors.primary)
                .build();

            // 文字：primary（#36D399）+ 14px + 左对齐 + 垂直居中
            ui.text(id + ".text")
                .position(kSelectionBarTextPadX, 0.0f)
                .size(w - kSelectionBarTextPadX, h)
                .text(label)
                .fontSize(kSelectionBarFontSize)
                .horizontalAlign(eui::HorizontalAlign::Left)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(t.colors.primary)
                .build();
        })
        .build();
}

} // namespace ui_kit
