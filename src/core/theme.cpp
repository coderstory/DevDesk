// 颜色值与 devdesk_theme.h light()/dark() 完全同步（S1 令牌总表为权威）。
// 变更时两处同改，以 hex 值为对照。
#include "theme.h"

#include <utility>

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
        c.primary = eui::Color("#36D399");
        c.primaryHover = eui::Color("#2EC48F");
        c.primaryPressed = eui::Color("#23B07F");
        c.primarySoftBg = eui::Color("#0D3326");
        c.primaryText = eui::Color("#36D399");
        c.onPrimary = eui::Color("#FFFFFF");
        c.textPrimary = eui::Color("#FAF9F5");
        c.textSecondary = eui::Color("#C9C5BD");
        c.textTertiary = eui::Color("#9A948A");
        c.textDisabled = eui::Color("#6B6660");
        c.borderDefault = eui::Color("#3A3735");
        c.borderMuted = eui::Color("#2A2826");
        c.success = eui::Color("#34D399");
        c.warning = eui::Color("#FBBF24");
        c.error = eui::Color("#F87171");
        c.info = eui::Color("#60A5FA");
        c.sidebarBg = eui::Color("#0F1F18");
        c.purple = eui::Color("#A18FC8");
        c.pink = eui::Color("#DE8FB4");
        c.blue = eui::Color("#12B7F5");
        // 微信风格配色（D-12 整改 → mint green 迁移）
        c.wechat.primary = eui::Color("#36D399");
        c.wechat.primaryHover = eui::Color("#2EC48F");
        c.wechat.primaryPressed = eui::Color("#23B07F");
        c.wechat.primaryText = eui::Color("#36D399");  // 暗主题用 primary 本身即够（~7.85:1 on #1A1816）
        c.wechat.onPrimary = eui::Color("#FFFFFF");
        c.wechat.warningRed = eui::Color("#EF4444");
        c.wechat.linkBlue = eui::Color("#3B82F6");
        c.wechat.pageBg = eui::Color("#FAFAF9");
        c.wechat.cardBg = eui::Color("#FFFFFF");
        c.wechat.dividerPrimary = eui::Color("#E5E7EB");
        c.wechat.dividerSecondary = eui::Color("#F3F4F6");
        c.wechat.textPrimary = eui::Color("#1F2937");
        c.wechat.textSecondary = eui::Color("#4B5563");
        c.wechat.textTertiary = eui::Color("#6B7280");
        c.wechat.selectedBg = eui::Color("#E6FBF2");
        c.wechat.secondaryButtonBg = eui::Color("#F3F4F6");
        c.wechat.sidebarBgTinted = eui::Color("#F0FDF7");

        auto& s = t.sizes;
        s.spacing = 8.0f;
        s.spacingSmall = 4.0f;
        s.spacingLarge = 16.0f;
        s.spacingXLarge = 24.0f;
        s.spacingXXLarge = 40.0f;
        s.radius = 8.0f;
        s.radiusSmall = 6.0f;
        s.radiusLarge = 12.0f;
        s.radiusPill = 999.0f;
        s.fontSizeSmall = 12.0f;
        s.fontSizeBody = 14.0f;
        s.fontSizeLarge = 18.0f;
        s.fontSizeTitle = 24.0f;
        s.fontSizePage = 24.0f;
        s.fontSizeDisplay = 48.0f;
        s.fontSizeStat = 28.0f;
        s.spacingPage = 12.0f;
        s.radiusWeChat = 6.0f;
        s.fontSizeWeChat = 16.0f;
        return t;
    }

    Tokens lightTokens() {
        Tokens t;
        t.isDark = false;
        auto& c = t.colors;
        c.background = eui::Color("#FAFAF9");
        c.surface = eui::Color("#FFFFFF");
        c.surfaceElevated = eui::Color("#FFFFFF");
        c.hover = eui::Color("#F3F4F6");
        c.active = eui::Color("#E5E7EB");
        c.primary = eui::Color("#36D399");
        c.primaryHover = eui::Color("#2EC48F");
        c.primaryPressed = eui::Color("#23B07F");
        c.primarySoftBg = eui::Color("#E6FBF2");
        c.primaryText = eui::Color("#157E4E");  // ~4.87:1 on #FAFAF9 ≥ WCAG AA 4.5:1（审计 F3.4 裁决）
        c.onPrimary = eui::Color("#FFFFFF");
        c.textPrimary = eui::Color("#1F2937");
        c.textSecondary = eui::Color("#4B5563");
        c.textTertiary = eui::Color("#6B7280");
        c.textDisabled = eui::Color("#9CA3AF");
        c.borderDefault = eui::Color("#E5E7EB");
        c.borderMuted = eui::Color("#F3F4F6");
        c.success = eui::Color("#10B981");
        c.warning = eui::Color("#F59E0B");
        c.error = eui::Color("#EF4444");
        c.info = eui::Color("#3B82F6");
        c.sidebarBg = eui::Color("#F0FDF7");
        c.purple = eui::Color("#71639B");
        c.pink = eui::Color("#C05E86");
        c.blue = eui::Color("#12B7F5");
        // 微信风格配色（D-12 整改 → mint green 迁移）
        c.wechat.primary = eui::Color("#36D399");
        c.wechat.primaryHover = eui::Color("#2EC48F");
        c.wechat.primaryPressed = eui::Color("#23B07F");
        c.wechat.primaryText = eui::Color("#157E4E");  // ~4.87:1 on #FAFAF9 ≥ WCAG AA 4.5:1
        c.wechat.onPrimary = eui::Color("#FFFFFF");
        c.wechat.warningRed = eui::Color("#EF4444");
        c.wechat.linkBlue = eui::Color("#3B82F6");
        c.wechat.pageBg = eui::Color("#FAFAF9");
        c.wechat.cardBg = eui::Color("#FFFFFF");
        c.wechat.dividerPrimary = eui::Color("#E5E7EB");
        c.wechat.dividerSecondary = eui::Color("#F3F4F6");
        c.wechat.textPrimary = eui::Color("#1F2937");
        c.wechat.textSecondary = eui::Color("#4B5563");
        c.wechat.textTertiary = eui::Color("#6B7280");
        c.wechat.selectedBg = eui::Color("#E6FBF2");
        c.wechat.secondaryButtonBg = eui::Color("#F3F4F6");
        c.wechat.sidebarBgTinted = eui::Color("#F0FDF7");

        auto& s = t.sizes;
        s.spacing = 8.0f;
        s.spacingSmall = 4.0f;
        s.spacingLarge = 16.0f;
        s.spacingXLarge = 24.0f;
        s.spacingXXLarge = 40.0f;
        s.radius = 8.0f;
        s.radiusSmall = 6.0f;
        s.radiusLarge = 12.0f;
        s.radiusPill = 999.0f;
        s.fontSizeSmall = 12.0f;
        s.fontSizeBody = 14.0f;
        s.fontSizeLarge = 18.0f;
        s.fontSizeTitle = 24.0f;
        s.fontSizePage = 24.0f;
        s.fontSizeDisplay = 48.0f;
        s.fontSizeStat = 28.0f;
        s.spacingPage = 12.0f;
        s.radiusWeChat = 6.0f;
        s.fontSizeWeChat = 16.0f;
        return t;
    }

    Tokens g_current = darkTokens();

    Accent g_accent = Accent::Clay;

    void applyAccentPrimary(Tokens& t, Accent a) {
        if (a == Accent::Clay) return;
        auto& c = t.colors;
        if (t.isDark) {
            switch (a) {
            case Accent::Purple:
                c.primary = eui::Color("#A18FC8");
                c.primaryHover = eui::Color("#A998CC");
                c.primaryPressed = eui::Color("#8E7EB0");
                c.primarySoftBg = eui::Color("#322E34");
                c.primaryText = eui::Color("#A18FC8");
                c.onPrimary = eui::Color("#FFFFFF");
                break;
            case Accent::Pink:
                c.primary = eui::Color("#DE8FB4");
                c.primaryHover = eui::Color("#E198BA");
                c.primaryPressed = eui::Color("#C37E9E");
                c.primarySoftBg = eui::Color("#392E32");
                c.primaryText = eui::Color("#DE8FB4");
                c.onPrimary = eui::Color("#FFFFFF");
                break;
            case Accent::Blue:
                c.primary = eui::Color("#12B7F5");
                c.primaryHover = eui::Color("#25BDF6");
                c.primaryPressed = eui::Color("#10A1D8");
                c.primarySoftBg = eui::Color("#213035");
                c.primaryText = eui::Color("#12B7F5");
                c.onPrimary = eui::Color("#1A1816");
                break;
            case Accent::Clay:
                break;
            }
        } else {
            switch (a) {
            case Accent::Purple:
                c.primary = eui::Color("#71639B");
                c.primaryHover = eui::Color("#7C6FA3");
                c.primaryPressed = eui::Color("#635788");
                c.primarySoftBg = eui::Color("#E5E1E2");
                c.primaryText = eui::Color("#5A4F7C");
                c.onPrimary = eui::Color("#FFFFFF");
                break;
            case Accent::Pink:
                c.primary = eui::Color("#B4567C");
                c.primaryHover = eui::Color("#BA6486");
                c.primaryPressed = eui::Color("#9E4C6D");
                c.primarySoftBg = eui::Color("#EDDFDF");
                c.primaryText = eui::Color("#904563");
                c.onPrimary = eui::Color("#FFFFFF");
                break;
            case Accent::Blue:
                c.primary = eui::Color("#12B7F5");
                c.primaryHover = eui::Color("#25BDF6");
                c.primaryPressed = eui::Color("#10A1D8");
                c.primarySoftBg = eui::Color("#DEECED");
                c.primaryText = eui::Color("#0B6E93");
                c.onPrimary = eui::Color("#1A1816");
                break;
            case Accent::Clay:
                break;
            }
        }
    }

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
        s.fontSizeStat *= g_fontScale;
    }

    // 按当前 isDark + g_fontScale 重建全局 token（单一来源，避免缩放叠加）
    void rebuild() {
        g_current = g_current.isDark ? darkTokens() : lightTokens();
        applyAccentPrimary(g_current, g_accent);
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
    applyAccentPrimary(g_current, g_accent);
    applyFontScale(g_current);
    notifyThemeChanged(true);
}

void setLight() {
    g_current = lightTokens();
    applyAccentPrimary(g_current, g_accent);
    applyFontScale(g_current);
    notifyThemeChanged(false);
}

void setFontScale(float scale) {
    // 钳制到 [0.5, 2.0]：低于 0.5 文字不可读，高于 2.0 布局必然破裂
    g_fontScale = scale < 0.5f ? 0.5f : (scale > 2.0f ? 2.0f : scale);
    rebuild();
    // 与 setDark/setLight/setAccent 同权广播：字号档也是主题状态变更。此前是唯一
    // 不通知的 mutator（UAT6「字号设置无效，起码没实时生效」排查结论之一）——
    // 下游联动（标题栏/窗口效果回查等）不该对档位变化无感，重算/重渲染也不再
    // 依赖输入路径顺带触发的 composeRequested。
    notifyThemeChanged(g_current.isDark);
}

float fontScale() { return g_fontScale; }

namespace {
    // 编辑器字号全局通道；默认 15 = fontSizeBody token 基准
    float g_editorFontSize = 15.0f;
}

void setEditorFontSize(float px) {
    g_editorFontSize = px < 10.0f ? 10.0f : (px > 32.0f ? 32.0f : px);
}

float editorFontSize() { return g_editorFontSize; }

std::string currentThemeName() {
    return g_current.isDark ? "Dark" : "Light";
}

void setAccent(Accent a) {
    if (g_accent == a) return;
    g_accent = a;
    rebuild();
    notifyThemeChanged(g_current.isDark);
}

Accent accent() { return g_accent; }

} // namespace core::theme