// src/ui_kit/card.cpp
// 卡片底板绘制（R6 自 settings_module.cpp 的私有 cardFrame 逐字搬出）。
#include <ui_kit/card.h>

namespace ui_kit {

void cardFrame(eui::Ui& ui, const std::string& id, float w, float h,
               const core::theme::Tokens& t) {
    ui.rect(id + ".bg")
        .position(0.0f, 0.0f)
        .size(w, h)
        .color(t.colors.surface)
        .radius(t.sizes.radiusLarge)
        .border(1.0f, t.colors.borderDefault)
        .build();
}

} // namespace ui_kit
