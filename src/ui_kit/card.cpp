// src/ui_kit/card.cpp
// 卡片底板绘制（R6 自 settings_module.cpp 的私有 cardFrame 逐字搬出）。
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
        // 卡阴影与 home 卡（EUI-NEO CardStyle panelShadow）同款 token 数值：
        // blur 24 / dy 8 / 深色 α0.28 · 浅色 α0.12。综合审查根因——settings
        // 9 卡填充 == 主体渐变底（settings.body.bg）顶色（同为 surface），
        // 无阴影时光凭 1px 细边框与底不分层（浅色档 top 位置整卡隐形），
        // 用户感知「设置页没修」；补阴影后两档主题卡都浮于主体之上。
        .shadow(components::theme::panelShadow(
            ::core::theme::componentTokens(t)))
        .build();
}

} // namespace ui_kit
