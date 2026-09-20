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
        c.textPrimary = eui::Color("#1F1D1A");
        c.textSecondary = eui::Color("#6B6660");
        c.textTertiary = eui::Color("#9A948A");
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
}

const Tokens& current() { return g_current; }

void setDark() { g_current = darkTokens(); }
void setLight() { g_current = lightTokens(); }

std::string currentThemeName() {
    return g_current.isDark ? "Dark" : "Light";
}

} // namespace core::theme