// src/core/theme.h
// 设计 token（基于方案 B 温暖专业）
#pragma once

#include <eui/types.h>

#include <functional>
#include <string>

namespace core::theme {

// 主题深浅切换回调：core 层不可调 EUI-NEO 运行时行为，标题栏主题联动等由 main.cpp 启动时
// 注入回调实现；setDark/setLight 每次切换都通知（幂等重放也通知）
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
    // 主色作文字色时的专用 token：#D97757 直接当 11-13px 小字在浅底上仅 2.96:1，低于 WCAG AA
    // 4.5:1。深色档 = primary；浅色档 = 深一档 clay（浅底/primarySoftBg 上 ≥4.5:1）。
    // 链接/标签 chip 文字等用本 token；图标/边框/背景继续用 primary
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

// 切换主题
void setDark();
void setLight();

// 全局字号缩放（1.0 = 原始尺寸）：作用于 SizeTokens 全部 fontSize* 字段，经 t.sizes.fontSize*
// 取字号的模块零改动受益；硬编码的字面量字号不经过 token，不在缩放范围
void setFontScale(float scale);
float fontScale();

// Markdown 编辑器字号（px）：与 fontScale 同型的「core 存值、app/settings 写、消费方读」全局
// 通道（SettingsConfig 在 module_settings，markdown 不可 include，故运行时值下沉这里）。默认 15.0f，
// set 钳制 [10, 32]（低于 10 不可读、高于 32 编辑器布局必裂）
void setEditorFontSize(float px);
float editorFontSize();

// 获取当前主题名
std::string currentThemeName();

} // namespace core::theme