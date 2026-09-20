// src/core/theme.h
// 设计 token（基于方案 B 温暖专业）
#pragma once

#include <string>

// 前向声明 EUI-NEO 的 Color
namespace eui { struct Color; }

namespace core::theme {

// 颜色 token（Dark Mode）
struct ColorTokens {
    eui::Color background;
    eui::Color surface;
    eui::Color surfaceElevated;
    eui::Color hover;
    eui::Color active;
    eui::Color primary;
    eui::Color primaryHover;
    eui::Color primaryPressed;
    eui::Color primarySoftBg;
    eui::Color textPrimary;
    eui::Color textSecondary;
    eui::Color textTertiary;
    eui::Color textDisabled;
    eui::Color borderDefault;
    eui::Color borderMuted;
    eui::Color success;
    eui::Color warning;
    eui::Color error;
    eui::Color info;
};

// 尺寸 token（**唯一权威定义**，详见 spec §6.5 + §6.5.2）
struct SizeTokens {
    float spacing;             // 8.0f
    float spacingSmall;        // 4.0f
    float spacingLarge;        // 16.0f
    float spacingXLarge;       // 24.0f
    float spacingXXLarge;      // 40.0f
    float radius;              // 10.0f
    float radiusSmall;         // 6.0f
    float radiusLarge;         // 14.0f
    float radiusPill;          // 999.0f
    float fontSizeSmall;       // 13.0f
    float fontSizeBody;        // 15.0f
    float fontSizeLarge;       // 18.0f
    float fontSizeTitle;       // 24.0f
    float fontSizePage;        // 32.0f
    float fontSizeDisplay;     // 48.0f
};

struct Tokens {
    ColorTokens colors;
    SizeTokens sizes;
    bool isDark;
};

// 当前主题（默认深色 = 方案 B Dark）
const Tokens& current();

// 切换主题
void setDark();
void setLight();

// 获取当前主题名
std::string currentThemeName();

} // namespace core::theme