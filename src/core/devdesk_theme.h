#pragma once
#include <components/theme.h>
#include <components/widget_style.h>

namespace core::theme {

struct DevDeskTheme {
    static components::theme::ThemeColorTokens light();
    static components::theme::ThemeColorTokens dark();
    static constexpr const char* kFontStack = "Inter, \"Microsoft YaHei\", \"PingFang SC\", sans-serif";
    static void registerWidgetStyles();
};

// ============================================================================
// light() — v1.0 mint green light theme
// ============================================================================
inline components::theme::ThemeColorTokens DevDeskTheme::light() {
    using components::theme::color;

    components::theme::ThemeColorTokens t;
    t.dark = false;

    // Primary
    t.primary       = color(54.0f / 255.0f, 211.0f / 255.0f, 153.0f / 255.0f);   // #36D399
    t.primaryHover  = color(46.0f / 255.0f, 196.0f / 255.0f, 143.0f / 255.0f);   // #2EC48F
    t.primaryActive = color(35.0f / 255.0f, 176.0f / 255.0f, 127.0f / 255.0f);   // #23B07F
    t.primarySoftBg = color(230.0f / 255.0f, 251.0f / 255.0f, 242.0f / 255.0f);  // #E6FBF2

    // Text
    t.text          = color(31.0f / 255.0f, 41.0f / 255.0f, 55.0f / 255.0f);     // #1F2937
    t.textSecondary = color(75.0f / 255.0f, 85.0f / 255.0f, 99.0f / 255.0f);     // #4B5563
    t.textTertiary  = color(107.0f / 255.0f, 114.0f / 255.0f, 128.0f / 255.0f);  // #6B7280
    t.textDisabled  = color(156.0f / 255.0f, 163.0f / 255.0f, 175.0f / 255.0f);  // #9CA3AF

    // Surfaces
    t.background    = color(250.0f / 255.0f, 250.0f / 255.0f, 249.0f / 255.0f);  // #FAFAF9
    t.surface       = color(255.0f / 255.0f, 255.0f / 255.0f, 255.0f / 255.0f);  // #FFFFFF
    t.surfaceHover  = color(243.0f / 255.0f, 244.0f / 255.0f, 246.0f / 255.0f);  // #F3F4F6
    t.surfaceActive = color(229.0f / 255.0f, 231.0f / 255.0f, 235.0f / 255.0f);  // #E5E7EB

    // Border & Semantic
    t.border   = color(229.0f / 255.0f, 231.0f / 255.0f, 235.0f / 255.0f);  // #E5E7EB
    t.success  = color(16.0f / 255.0f, 185.0f / 255.0f, 129.0f / 255.0f);   // #10B981
    t.warning  = color(245.0f / 255.0f, 158.0f / 255.0f, 11.0f / 255.0f);   // #F59E0B
    t.error    = color(239.0f / 255.0f, 68.0f / 255.0f, 68.0f / 255.0f);    // #EF4444
    t.info     = color(59.0f / 255.0f, 130.0f / 255.0f, 246.0f / 255.0f);   // #3B82F6

    // Sidebar & Cards
    t.sidebarBg  = color(240.0f / 255.0f, 253.0f / 255.0f, 247.0f / 255.0f);  // #F0FDF7
    t.cardGreen  = color(230.0f / 255.0f, 251.0f / 255.0f, 242.0f / 255.0f);  // #E6FBF2
    t.cardBlue   = color(224.0f / 255.0f, 244.0f / 255.0f, 255.0f / 255.0f);  // #E0F4FF
    t.cardOrange = color(255.0f / 255.0f, 241.0f / 255.0f, 224.0f / 255.0f);  // #FFF1E0
    t.cardPurple = color(243.0f / 255.0f, 232.0f / 255.0f, 255.0f / 255.0f);  // #F3E8FF
    t.cardCyan   = color(223.0f / 255.0f, 250.0f / 255.0f, 246.0f / 255.0f);  // #DFFAF6

    return t;
}

// ============================================================================
// dark() — v1.0 derived dark mint theme
// ============================================================================
inline components::theme::ThemeColorTokens DevDeskTheme::dark() {
    using components::theme::color;

    components::theme::ThemeColorTokens t;
    t.dark = true;

    // Background stack
    t.background    = color(26.0f / 255.0f, 24.0f / 255.0f, 22.0f / 255.0f);   // #1A1816
    t.surface       = color(35.0f / 255.0f, 33.0f / 255.0f, 32.0f / 255.0f);   // #232120
    t.surfaceHover  = color(53.0f / 255.0f, 50.0f / 255.0f, 49.0f / 255.0f);   // #353231
    t.surfaceActive = color(62.0f / 255.0f, 59.0f / 255.0f, 57.0f / 255.0f);   // #3E3B39

    // Primary (same mint, bright enough for dark)
    t.primary       = color(54.0f / 255.0f, 211.0f / 255.0f, 153.0f / 255.0f);  // #36D399
    t.primaryHover  = color(62.0f / 255.0f, 219.0f / 255.0f, 166.0f / 255.0f);  // #3EDBA6
    t.primaryActive = color(46.0f / 255.0f, 196.0f / 255.0f, 143.0f / 255.0f);  // #2EC48F
    t.primarySoftBg = color(13.0f / 255.0f, 51.0f / 255.0f, 38.0f / 255.0f);   // #0D3326

    // Text (light on dark)
    t.text          = color(250.0f / 255.0f, 249.0f / 255.0f, 245.0f / 255.0f); // #FAF9F5
    t.textSecondary = color(201.0f / 255.0f, 197.0f / 255.0f, 189.0f / 255.0f); // #C9C5BD
    t.textTertiary  = color(154.0f / 255.0f, 148.0f / 255.0f, 138.0f / 255.0f); // #9A948A
    t.textDisabled  = color(107.0f / 255.0f, 102.0f / 255.0f, 96.0f / 255.0f);  // #6B6660

    // Border & Semantic
    t.border   = color(58.0f / 255.0f, 55.0f / 255.0f, 53.0f / 255.0f);   // #3A3735
    t.success  = color(52.0f / 255.0f, 211.0f / 255.0f, 153.0f / 255.0f);  // #34D399
    t.warning  = color(251.0f / 255.0f, 191.0f / 255.0f, 36.0f / 255.0f);  // #FBBF24
    t.error    = color(248.0f / 255.0f, 113.0f / 255.0f, 113.0f / 255.0f); // #F87171
    t.info     = color(96.0f / 255.0f, 165.0f / 255.0f, 250.0f / 255.0f);  // #60A5FA

    // Sidebar & Cards
    t.sidebarBg  = color(15.0f / 255.0f, 31.0f / 255.0f, 24.0f / 255.0f);   // #0F1F18
    t.cardGreen  = color(13.0f / 255.0f, 51.0f / 255.0f, 38.0f / 255.0f);   // #0D3326
    t.cardBlue   = color(12.0f / 255.0f, 35.0f / 255.0f, 48.0f / 255.0f);   // #0C2330
    t.cardOrange = color(45.0f / 255.0f, 31.0f / 255.0f, 14.0f / 255.0f);   // #2D1F0E
    t.cardPurple = color(31.0f / 255.0f, 24.0f / 255.0f, 48.0f / 255.0f);   // #1F1830
    t.cardCyan   = color(13.0f / 255.0f, 45.0f / 255.0f, 38.0f / 255.0f);   // #0D2D26

    return t;
}

// ============================================================================
// registerWidgetStyles() — WolfStyle 批量注册 S1 令牌总表控件样式
// ============================================================================
inline void DevDeskTheme::registerWidgetStyles() {
    using components::theme::color;
    using components::theme::withOpacity;
    using components::WidgetStyleEntry;
    using components::WidgetStyleRegistry;

    auto& reg = WidgetStyleRegistry::instance();

    // Use light tokens as the reference palette for style registration.
    const auto t           = light();
    const auto cWhite      = color(1.0f, 1.0f, 1.0f);
    const auto cTransparent = color(0.0f, 0.0f, 0.0f, 0.0f);

    // ---- Button:Filled -------------------------------------------------------
    reg.set("Button", "Filled", "default",
            WidgetStyleEntry{t.primary, cWhite, cTransparent, 8.0f, 14.0f, 40.0f});
    reg.set("Button", "Filled", "hover",
            WidgetStyleEntry{t.primaryHover, cWhite, cTransparent, 8.0f, 14.0f, 40.0f});
    reg.set("Button", "Filled", "pressed",
            WidgetStyleEntry{t.primaryActive, cWhite, cTransparent, 8.0f, 14.0f, 40.0f});
    reg.set("Button", "Filled", "disabled",
            WidgetStyleEntry{withOpacity(t.primary, 0.40f), withOpacity(cWhite, 0.60f),
                             cTransparent, 8.0f, 14.0f, 40.0f});

    // ---- Button:Outline -------------------------------------------------------
    reg.set("Button", "Outline", "default",
            WidgetStyleEntry{cWhite, t.primary, t.primary, 8.0f, 14.0f, 40.0f});
    reg.set("Button", "Outline", "hover",
            WidgetStyleEntry{t.primarySoftBg, t.primary, t.primary, 8.0f, 14.0f, 40.0f});
    reg.set("Button", "Outline", "pressed",
            WidgetStyleEntry{withOpacity(t.primaryActive, 0.15f), t.primaryActive,
                             t.primaryActive, 8.0f, 14.0f, 40.0f});
    reg.set("Button", "Outline", "disabled",
            WidgetStyleEntry{cWhite, t.textDisabled, t.textDisabled, 8.0f, 14.0f, 40.0f});

    // ---- Button:Ghost ---------------------------------------------------------
    reg.set("Button", "Ghost", "default",
            WidgetStyleEntry{cTransparent, t.primary, cTransparent, 8.0f, 14.0f, 32.0f});
    reg.set("Button", "Ghost", "hover",
            WidgetStyleEntry{t.primarySoftBg, t.primary, cTransparent, 8.0f, 14.0f, 32.0f});
    reg.set("Button", "Ghost", "pressed",
            WidgetStyleEntry{
                color(209.0f / 255.0f, 245.0f / 255.0f,
                      228.0f / 255.0f),  // primarySoftBg ~10% darker
                t.primaryActive, cTransparent, 8.0f, 14.0f, 32.0f});
    reg.set("Button", "Ghost", "disabled",
            WidgetStyleEntry{cTransparent, t.textDisabled, cTransparent, 8.0f, 14.0f, 32.0f});

    // ---- Button:Danger --------------------------------------------------------
    const auto cDangerBg       = color(254.0f / 255.0f, 226.0f / 255.0f, 226.0f / 255.0f);  // #FEE2E2
    const auto cDangerPressedBg = color(220.0f / 255.0f, 38.0f / 255.0f, 38.0f / 255.0f);   // #DC2626

    reg.set("Button", "Danger", "default",
            WidgetStyleEntry{cDangerBg, t.error, cTransparent, 8.0f, 14.0f, 40.0f});
    reg.set("Button", "Danger", "hover",
            WidgetStyleEntry{t.error, cWhite, cTransparent, 8.0f, 14.0f, 40.0f});
    reg.set("Button", "Danger", "pressed",
            WidgetStyleEntry{cDangerPressedBg, cWhite, cTransparent, 8.0f, 14.0f, 40.0f});
    reg.set("Button", "Danger", "disabled",
            WidgetStyleEntry{withOpacity(cDangerBg, 0.60f), withOpacity(t.error, 0.40f),
                             cTransparent, 8.0f, 14.0f, 40.0f});

    // ---- Button:Large ---------------------------------------------------------
    reg.set("Button", "Large", "default",
            WidgetStyleEntry{t.primary, cWhite, cTransparent, 8.0f, 14.0f, 44.0f});
    reg.set("Button", "Large", "hover",
            WidgetStyleEntry{t.primaryHover, cWhite, cTransparent, 8.0f, 14.0f, 44.0f});
    reg.set("Button", "Large", "pressed",
            WidgetStyleEntry{t.primaryActive, cWhite, cTransparent, 8.0f, 14.0f, 44.0f});

    // ---- Tab:Page -------------------------------------------------------------
    reg.set("Tab", "Page", "default",
            WidgetStyleEntry{cTransparent, t.textSecondary, cTransparent, 0.0f, 14.0f, 40.0f});
    reg.set("Tab", "Page", "hover",
            WidgetStyleEntry{t.surfaceHover, t.textSecondary, cTransparent, 8.0f, 14.0f, 40.0f});
    reg.set("Tab", "Page", "selected",
            WidgetStyleEntry{t.primarySoftBg, t.primary, cTransparent, 8.0f, 14.0f, 40.0f});

    // ---- Tab:Small ------------------------------------------------------------
    reg.set("Tab", "Small", "default",
            WidgetStyleEntry{cTransparent, t.textSecondary, cTransparent, 0.0f, 13.0f, 32.0f});
    reg.set("Tab", "Small", "hover",
            WidgetStyleEntry{t.surfaceHover, t.textSecondary, cTransparent, 8.0f, 13.0f, 32.0f});
    reg.set("Tab", "Small", "selected",
            WidgetStyleEntry{t.primarySoftBg, t.primary, cTransparent, 8.0f, 13.0f, 32.0f});

    // ---- ListItem -------------------------------------------------------------
    reg.set("ListItem", "default", "default",
            WidgetStyleEntry{cTransparent, t.textSecondary, cTransparent, 0.0f, 14.0f, 40.0f});
    reg.set("ListItem", "default", "hover",
            WidgetStyleEntry{t.surfaceHover, t.textSecondary, cTransparent, 0.0f, 14.0f, 40.0f});
    reg.set("ListItem", "default", "selected",
            WidgetStyleEntry{t.primarySoftBg, t.primary, cTransparent, 0.0f, 14.0f, 40.0f});
}

}  // namespace core::theme