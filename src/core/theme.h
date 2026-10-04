// 设计 token（温暖专业配色）
#pragma once

#include <eui/types.h>

#include <functional>
#include <string>

namespace core::theme {

// 主题深浅切换回调：core 层不可调 EUI-NEO 运行时行为，标题栏联动由 main.cpp 启动时注入；
// setDark/setLight 每次切换都通知（幂等重放也通知）
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
    // 主色作文字色的专用 token：#D97757 直接当 11-13px 小字在浅底上仅 2.96:1，低于 WCAG AA 4.5:1；
    // 深色档 = primary，浅色档 = 深一档 clay。链接/chip 文字用本 token；图标/边框/背景继续用 primary
    eui::Color primaryText;
    // 主色背景上的前景色（主按钮文字/ACTIVE 徽标；两主题均为白）
    eui::Color onPrimary;
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

// 尺寸 token（唯一权威定义）
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

// 当前主题（默认深色）
const Tokens& current();

void setDark();
void setLight();

// 全局字号缩放（1.0 = 原始尺寸）：作用于 SizeTokens 全部 fontSize* 字段；硬编码字面量不在缩放范围
void setFontScale(float scale);
float fontScale();

// Markdown 编辑器字号（px）：「core 存值、app/settings 写、消费方读」全局通道
//（markdown 不可 include module_settings）。默认 15.0f，set 钳制 [10, 32]
void setEditorFontSize(float px);
float editorFontSize();

std::string currentThemeName();

} // namespace core::theme