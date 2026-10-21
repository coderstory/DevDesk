// src/ui_kit/form_row.cpp — 表单栅格 helper 实现（spec §4.15）。
// label 定宽 110px + control 填满剩余宽；垂直居中。色值走 wechat 令牌（D-03）。
#include <ui_kit/form_row.h>

#include <algorithm>

namespace ui_kit {

void formRow(eui::Ui& ui, const std::string& id, const std::string& label,
             float x, float y, float totalW, float totalH,
             const ControlComposeFn& composeControl,
             const core::theme::Tokens& t) {
    const auto& wc = t.colors.wechat;
    const float gap = t.sizes.spacing;  // 8px（§3.1 8px 网格），label 与 control 间距
    const float labelW = kFormRowLabelWidth;  // 110px（§4.15 区间中值）
    const float controlX = x + labelW + gap;
    const float controlW = std::max(0.0f, totalW - labelW - gap);

    // label：左 110px 定宽，垂直居中，左对齐（标签贴左缘）
    ui.text(id + ".label")
        .position(x, y)
        .size(labelW, totalH)
        .text(label)
        // §3.3 主文字；token 体系无 14 档，钉 spec 值 × 全局字号档（与 settings
        // 的 fs() 同式 base×fontScale；ui_kit 不能 include module_settings，直乘
        // core::theme::fontScale()）。行盒由消费方 roundFs(36) 派生、随档缩放。
        .fontSize(14.0f * core::theme::fontScale())
        .horizontalAlign(eui::HorizontalAlign::Left)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textPrimary)
        .build();

    // control 区：填满剩余宽，垂直居中；由消费方在回调里画具体控件
    if (composeControl) {
        composeControl(ui, controlX, y, controlW, totalH);
    }
}

} // namespace ui_kit
