// src/module_settings/settings_ui_logic.cpp
// settings 模块纯函数实现。
#include "settings_ui_logic.h"

#include <core/theme.h>

#include <algorithm>
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

float estimatedTextWidth(const std::string& text, float fontSize) {
    // 逐字节扫 UTF-8：>= 0x80 的多字节序列首字节（0xC0-0xFF）各计一次全角宽度。
    float em = 0.0f;
    for (const unsigned char c : text) {
        if (c < 0x80u) {
            em += kAsciiWidthEm;
        } else if ((c & 0xC0u) != 0x80u) {
            em += kFullWidthEm;  // 多字节字符首字节
        }
    }
    return em * fontSize;
}

std::string fitControlLabel(const std::string& text, float availableWidth,
                            float fontSize) {
    if (availableWidth <= 0.0f || fontSize <= 0.0f || text.empty()) {
        return text;
    }
    if (estimatedTextWidth(text, fontSize) <= availableWidth) {
        return text;
    }
    // 预算换算成 em（省略号本身也占 1em，否则截断串仍超宽）。
    const float budgetEm = availableWidth / fontSize - kFullWidthEm;
    std::string out;
    float usedEm = 0.0f;
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        std::size_t charLen = 1;
        float em = kAsciiWidthEm;
        if (c >= 0x80u) {
            charLen = (c & 0xE0u) == 0xC0u ? 2u : ((c & 0xF0u) == 0xE0u ? 3u : 4u);
            em = kFullWidthEm;
        }
        // 越界 / 截断在字符边界上（不切断多字节序列）
        if (i + charLen > text.size()) {
            break;
        }
        if (usedEm + em > budgetEm && !out.empty()) {
            break;
        }
        usedEm += em;
        out.append(text, i, charLen);
        i += charLen;
        if (usedEm >= budgetEm) {
            break;
        }
    }
    return out + "\xE2\x80\xA6";  // U+2026 水平省略号
}

float petSkinFieldWidth(const std::vector<std::string>& skinNames,
                        float fontSize) {
    float widest = 0.0f;
    for (const std::string& name : skinNames) {
        widest = std::max(widest, estimatedTextWidth(name, fontSize));
    }
    const float needed = widest + kDropdownFieldChrome;
    return std::min(std::max(kPetSkinFieldMinWidth, needed),
                    kPetSkinFieldMaxWidth);
}

} // namespace module::settings
