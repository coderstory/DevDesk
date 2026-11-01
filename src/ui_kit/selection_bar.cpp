// src/ui_kit/selection_bar.cpp — 列表选中态 helper 实现（S2.5）。
// 色值全部走 core::theme::Tokens 主槽位，禁裸 hex。
// 既有页面选中态调用不动；本 helper 供 W3/W4 逐页切换调用。
#include <ui_kit/selection_bar.h>

namespace ui_kit {

void selectionBar(eui::Ui& ui, const std::string& id, const std::string& label,
                  float x, float y, float w, float h,
                  const core::theme::Tokens& t, float accentOffsetX) {

    // 选中 = 白卡胶囊（surface + border + 圆角）+ 左侧 3×18 圆角竖条 + primaryText 文字。
    // 底保持白卡（H5）：侧栏底 #EBF6EF 与 primarySoftBg #E8F8EE 只差 3 个色阶，
    // 软底压在绿底上等于看不见——「选中更亮」在绿底上只能反过来用白卡。
    // 文字用 primaryText（on 白 5.05:1）而非 primary（2.38:1）——主色作文字不达 AA。
    const float accentH = h < kSelectionBarAccentHeight ? h : kSelectionBarAccentHeight;
    const float accentY = (h - accentH) * 0.5f;

    ui.stack(id)
        .position(x, y)
        .size(w, h)
        .content([&] {
            // 底色 rect：surface 白卡 + border 描边 + 圆角（原型 .nav-item 的 border-radius）
            ui.rect(id + ".bg")
                .size(w, h)
                .color(t.colors.surface)
                .border(1.0f, t.colors.borderDefault)
                .radius(t.sizes.radiusSmall)
                .build();

            // 竖条：3×18、垂直居中、两端圆角。原型 .nav-item.active::before 把它摆在
            // 胶囊**外侧**的栏边留白里，故调用方可用 accentOffsetX 把它移出去；
            // 贴胶囊左缘通高那版读起来像表格行高亮，不像导航选中。
            ui.rect(id + ".accent")
                .position(accentOffsetX, accentY)
                .size(kSelectionBarAccentWidth, accentH)
                .color(t.colors.primary)
                .radius(kSelectionBarAccentWidth * 0.5f)
                .build();

            // 文字：primaryText（#157E4E）+ 14px + 左对齐 + 垂直居中
            ui.text(id + ".text")
                .position(kSelectionBarTextPadX, 0.0f)
                .size(w - kSelectionBarTextPadX, h)
                .text(label)
                .fontSize(kSelectionBarFontSize)
                .horizontalAlign(eui::HorizontalAlign::Left)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(t.colors.primaryText)
                .build();
        })
        .build();
}

} // namespace ui_kit
