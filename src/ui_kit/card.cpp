// src/ui_kit/card.cpp — 卡片底板绘制。
#include <ui_kit/card.h>

#include <core/component_theme.h>

#include <components/theme.h>

namespace ui_kit {

void cardFrame(eui::Ui& ui, const std::string& id, float w, float h,
               const core::theme::Tokens& t) {
    ui.rect(id + ".bg")
        .position(0.0f, 0.0f)
        .size(w, h)
        .color(t.colors.surface)
        .radius(t.sizes.radiusLarge)
        .border(1.0f, t.colors.borderDefault)
        // 阴影 token 与 home 卡同款：settings 卡填充 == 主体底顶色，无阴影时光凭 1px
        // 边框不分层（浅色档整卡隐形）。
        .shadow(components::theme::panelShadow(
            ::core::theme::componentTokensWeChat(t)))
        .build();
}

} // namespace ui_kit
