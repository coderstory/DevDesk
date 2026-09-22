// src/core/theme.h
// 设计 token（基于方案 B 温暖专业）
#pragma once

#include <eui/types.h>

#include <functional>
#include <string>

namespace core::theme {

// 主题深浅切换回调。core 层不能依赖 EUI-NEO（架构分层铁律），标题栏主题
// 联动（app::setTitleBarAppearance，磨砂设计 Phase A）等由 main.cpp 在启动
// 时注入回调实现；setDark/setLight 每次切换都会通知（幂等重放也通知）
using ChangeHandler = std::function<void(bool isDark)>;
void setChangeHandler(ChangeHandler handler);

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

// 全局字号缩放（1.0 = 原始尺寸；作用于 SizeTokens 的全部 fontSize* 字段，
// 即 token 读取层——所有经 t.sizes.fontSize* 取字号的模块零改动受益。
// 各模块内硬编码的字面量字号（如 fontSize(12.0f)）不经过 token，不在缩放范围）
void setFontScale(float scale);
float fontScale();

// 获取当前主题名
std::string currentThemeName();

} // namespace core::theme