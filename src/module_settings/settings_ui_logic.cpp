// src/module_settings/settings_ui_logic.cpp
// R6：settings 模块纯函数实现（原 settings_module.cpp 的匿名 ns / 私有纯成员）。
#include "settings_ui_logic.h"

#include <core/theme.h>

#include <cstdlib>

namespace module::settings {

int nearestIndex(int value, const int* presets, int count) {
    int best = 0;
    int bestDist = 1 << 30;
    for (int i = 0; i < count; ++i) {
        const int d = std::abs(presets[i] - value);
        if (d < bestDist) {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

float fs(float base) { return base * ::core::theme::fontScale(); }

bool dropdownPopupOverflows(float cardTopY, float fieldOffsetY,
                            float fieldHeight, int itemCount, float screenH) {
    // 字段绝对底边 = 卡顶 + 卡内 y 偏移 + 字段高
    const float fieldBottomY = cardTopY + fieldOffsetY + fieldHeight;
    const float popupH =
        static_cast<float>(itemCount) * kDropdownItemHeight +
        kDropdownPopupPadding;
    return fieldBottomY + kDropdownPopupGap + popupH >
           screenH - kWindowBottomMargin;
}

std::string retentionLabel(int days) {
    return days == 0 ? "永久保留" : std::to_string(days) + " 天";
}

} // namespace module::settings
