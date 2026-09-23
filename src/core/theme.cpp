#include "theme.h"

#include <eui/types.h>

namespace core::theme {

namespace {
    Tokens darkTokens() {
        Tokens t;
        t.isDark = true;
        auto& c = t.colors;
        c.background = eui::Color("#1A1816");
        c.surface = eui::Color("#232120");
        c.surfaceElevated = eui::Color("#2C2A28");
        c.hover = eui::Color("#353231");
        c.active = eui::Color("#3E3B39");
        c.primary = eui::Color("#D97757");
        c.primaryHover = eui::Color("#E0896B");
        c.primaryPressed = eui::Color("#B85F3F");
        c.primarySoftBg = eui::Color("#3A2620");
        // 深色档 primary 作文字达标（5.67:1 on background），与 primary 同值
        c.primaryText = eui::Color("#D97757");
        c.onPrimary = eui::Color("#FFFFFF");
        c.textPrimary = eui::Color("#FAF9F5");
        c.textSecondary = eui::Color("#C9C5BD");
        c.textTertiary = eui::Color("#9A948A");
        c.textDisabled = eui::Color("#6B6660");
        c.borderDefault = eui::Color("#3A3735");
        c.borderMuted = eui::Color("#2A2826");
        c.success = eui::Color("#788C5D");
        c.warning = eui::Color("#C29243");
        c.error = eui::Color("#CF2D56");
        c.info = eui::Color("#6A9BCC");

        auto& s = t.sizes;
        s.spacing = 8.0f;
        s.spacingSmall = 4.0f;
        s.spacingLarge = 16.0f;
        s.spacingXLarge = 24.0f;
        s.spacingXXLarge = 40.0f;
        s.radius = 10.0f;
        s.radiusSmall = 6.0f;
        s.radiusLarge = 14.0f;
        s.radiusPill = 999.0f;
        s.fontSizeSmall = 13.0f;
        s.fontSizeBody = 15.0f;
        s.fontSizeLarge = 18.0f;
        s.fontSizeTitle = 24.0f;
        s.fontSizePage = 32.0f;
        s.fontSizeDisplay = 48.0f;
        return t;
    }

    Tokens lightTokens() {
        Tokens t;
        t.isDark = false;
        auto& c = t.colors;
        c.background = eui::Color("#FAF9F5");
        c.surface = eui::Color("#FFFFFF");
        c.surfaceElevated = eui::Color("#FFFFFF");
        c.hover = eui::Color("#F5F2EC");
        c.active = eui::Color("#EBE7DF");
        c.primary = eui::Color("#D97757");
        c.primaryHover = eui::Color("#E0896B");
        c.primaryPressed = eui::Color("#B85F3F");
        c.primarySoftBg = eui::Color("#F5E6DE");
        // 审计 [27]：主色 #D97757 直接当文字在浅底 2.96:1 / primarySoftBg
        // 2.57:1 全不达标，文字档加深到深一档 clay（5.27 / 4.57，WCAG AA）
        c.primaryText = eui::Color("#A64F2E");
        c.onPrimary = eui::Color("#FFFFFF");
        c.textPrimary = eui::Color("#1F1D1A");
        // 审计 [27]：旧值 #6B6660 与新 textTertiary（原 #9A948A 在浅底
        // 仅 2.86:1，已加深到 #6B665E=5.41:1）几乎同色、层级坍塌；secondary
        // 相应再深一档保住两级辅助灰（7.49:1）
        c.textSecondary = eui::Color("#55514A");
        c.textTertiary = eui::Color("#6B665E");
        c.textDisabled = eui::Color("#B5B0A8");
        c.borderDefault = eui::Color("#E8E6DC");
        c.borderMuted = eui::Color("#F0EEE6");
        c.success = eui::Color("#5E7048");
        c.warning = eui::Color("#9A6B30");
        c.error = eui::Color("#B82850");
        c.info = eui::Color("#4A7AAB");

        auto& s = t.sizes;
        s.spacing = 8.0f;
        s.spacingSmall = 4.0f;
        s.spacingLarge = 16.0f;
        s.spacingXLarge = 24.0f;
        s.spacingXXLarge = 40.0f;
        s.radius = 10.0f;
        s.radiusSmall = 6.0f;
        s.radiusLarge = 14.0f;
        s.radiusPill = 999.0f;
        s.fontSizeSmall = 13.0f;
        s.fontSizeBody = 15.0f;
        s.fontSizeLarge = 18.0f;
        s.fontSizeTitle = 24.0f;
        s.fontSizePage = 32.0f;
        s.fontSizeDisplay = 48.0f;
        return t;
    }

    Tokens g_current = darkTokens();

    // 全局字号缩放（默认 1.0）；setDark/setLight 重建 token 后统一施加
    float g_fontScale = 1.0f;

    // 只缩放字号，间距/圆角不动（字号是可读性需求，布局度量保持稳定）
    void applyFontScale(Tokens& t) {
        auto& s = t.sizes;
        s.fontSizeSmall *= g_fontScale;
        s.fontSizeBody *= g_fontScale;
        s.fontSizeLarge *= g_fontScale;
        s.fontSizeTitle *= g_fontScale;
        s.fontSizePage *= g_fontScale;
        s.fontSizeDisplay *= g_fontScale;
    }

    // 按当前 isDark + g_fontScale 重建全局 token（单一来源，避免缩放叠加）
    void rebuild() {
        g_current = g_current.isDark ? darkTokens() : lightTokens();
        applyFontScale(g_current);
    }

    ChangeHandler g_changeHandler;

    void notifyThemeChanged(bool isDark) {
        if (g_changeHandler) {
            g_changeHandler(isDark);
        }
    }
}

void setChangeHandler(ChangeHandler handler) {
    g_changeHandler = std::move(handler);
}

const Tokens& current() { return g_current; }

void setDark() {
    g_current = darkTokens();
    applyFontScale(g_current);
    notifyThemeChanged(true);
}

void setLight() {
    g_current = lightTokens();
    applyFontScale(g_current);
    notifyThemeChanged(false);
}

void setFontScale(float scale) {
    // 钳制到 [0.5, 2.0]：低于 0.5 文字不可读，高于 2.0 布局必然破裂
    g_fontScale = scale < 0.5f ? 0.5f : (scale > 2.0f ? 2.0f : scale);
    rebuild();
}

float fontScale() { return g_fontScale; }

namespace {
    // 编辑器字号全局通道（spec 缩水 B）；默认 15 = fontSizeBody token 基准
    float g_editorFontSize = 15.0f;
}

void setEditorFontSize(float px) {
    g_editorFontSize = px < 10.0f ? 10.0f : (px > 32.0f ? 32.0f : px);
}

float editorFontSize() { return g_editorFontSize; }

std::string currentThemeName() {
    return g_current.isDark ? "Dark" : "Light";
}

} // namespace core::theme