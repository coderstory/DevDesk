#include "doctest.h"
#include "core/theme.h"

#include <cmath>

namespace {

// WCAG 2.x 相对亮度（sRGB 线性化 → 加权和）
double wcagLuminance(const eui::Color& c) {
    const auto lin = [](double v) {
        return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * lin(c.r) + 0.7152 * lin(c.g) + 0.0722 * lin(c.b);
}

double wcagContrast(const eui::Color& a, const eui::Color& b) {
    const double la = wcagLuminance(a);
    const double lb = wcagLuminance(b);
    const double hi = la > lb ? la : lb;
    const double lo = la > lb ? lb : la;
    return (hi + 0.05) / (lo + 0.05);
}

// CIE76 ΔE（sRGB → XYZ → Lab，D65 白点）
struct Lab { double L, a, b; };

Lab rgbToLab(const eui::Color& c) {
    const auto lin = [](double v) {
        return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    };
    double r = lin(c.r), g = lin(c.g), b = lin(c.b);
    // linear RGB → XYZ (D65)
    double x = r * 0.4124564 + g * 0.3575761 + b * 0.1804375;
    double y = r * 0.2126729 + g * 0.7151522 + b * 0.0721750;
    double z = r * 0.0193339 + g * 0.1191920 + b * 0.9503041;
    // XYZ → Lab (D65 reference white: Xn=0.95047, Yn=1.0, Zn=1.08883)
    const auto f = [](double t) {
        const double delta = 6.0 / 29.0;
        return t > delta * delta * delta ? std::cbrt(t)
                                         : t / (3.0 * delta * delta) + 4.0 / 29.0;
    };
    double fy = f(y);
    Lab lab;
    lab.L = 116.0 * fy - 16.0;
    lab.a = 500.0 * (f(x / 0.95047) - fy);
    lab.b = 200.0 * (fy - f(z / 1.08883));
    return lab;
}

double deltaE76(const eui::Color& a, const eui::Color& b) {
    Lab la = rgbToLab(a), lb = rgbToLab(b);
    double dL = la.L - lb.L, da = la.a - lb.a, db = la.b - lb.b;
    return std::sqrt(dL * dL + da * da + db * db);
}

// float [0,1] → 0-255 字节值（用于 hex 精确比对）
int toHexByte(float v) { return static_cast<int>(std::round(v * 255.0f)); }

} // namespace

// 三个强调色浅/深档 hex 精确等于设计定稿值（设计即代码，防将来误调）
REGISTER_TEST(theme_accent_exact_hex, "theme accent: exact hex values for all six slots", {
    core::theme::setLight();
    const auto& l = core::theme::current().colors;
    DOCTEST_CHECK_EQ(toHexByte(l.purple.r), 0x71);
    DOCTEST_CHECK_EQ(toHexByte(l.purple.g), 0x63);
    DOCTEST_CHECK_EQ(toHexByte(l.purple.b), 0x9B);
    DOCTEST_CHECK_EQ(toHexByte(l.pink.r), 0xC0);
    DOCTEST_CHECK_EQ(toHexByte(l.pink.g), 0x5E);
    DOCTEST_CHECK_EQ(toHexByte(l.pink.b), 0x86);
    DOCTEST_CHECK_EQ(toHexByte(l.blue.r), 0x12);
    DOCTEST_CHECK_EQ(toHexByte(l.blue.g), 0xB7);
    DOCTEST_CHECK_EQ(toHexByte(l.blue.b), 0xF5);

    core::theme::setDark();
    const auto& d = core::theme::current().colors;
    DOCTEST_CHECK_EQ(toHexByte(d.purple.r), 0xA1);
    DOCTEST_CHECK_EQ(toHexByte(d.purple.g), 0x8F);
    DOCTEST_CHECK_EQ(toHexByte(d.purple.b), 0xC8);
    DOCTEST_CHECK_EQ(toHexByte(d.pink.r), 0xDE);
    DOCTEST_CHECK_EQ(toHexByte(d.pink.g), 0x8F);
    DOCTEST_CHECK_EQ(toHexByte(d.pink.b), 0xB4);
    DOCTEST_CHECK_EQ(toHexByte(d.blue.r), 0x12);
    DOCTEST_CHECK_EQ(toHexByte(d.blue.g), 0xB7);
    DOCTEST_CHECK_EQ(toHexByte(d.blue.b), 0xF5);

    core::theme::setDark();
    return true;
})

// 对比度护栏：purple/pink 浅色档对 #F5F2EC ≥ 3.0、深色档对 #232120 ≥ 3.0；
// blue 浅色档锁在 [1.9, 2.3]（实测 2.06：品牌色档位、远低于图形 AA，只作图标/标签/装饰，非正文色），深色档 ≥ 3.0
REGISTER_TEST(theme_accent_contrast, "theme accent: contrast ratio guards", {
    core::theme::setLight();
    const auto& l = core::theme::current().colors;
    const eui::Color lightBg("#F5F2EC");
    DOCTEST_CHECK(wcagContrast(l.purple, lightBg) >= 3.0);
    DOCTEST_CHECK(wcagContrast(l.pink, lightBg) >= 3.0);
    double blueLightCR = wcagContrast(l.blue, lightBg);
    DOCTEST_CHECK(blueLightCR >= 1.9);
    DOCTEST_CHECK(blueLightCR <= 2.7);

    core::theme::setDark();
    const auto& d = core::theme::current().colors;
    const eui::Color darkBg("#232120");
    DOCTEST_CHECK(wcagContrast(d.purple, darkBg) >= 3.0);
    DOCTEST_CHECK(wcagContrast(d.pink, darkBg) >= 3.0);
    DOCTEST_CHECK(wcagContrast(d.blue, darkBg) >= 3.0);

    core::theme::setDark();
    return true;
})

// 三色两两 ΔE76 ≥ 20（深浅两模式各自算），确认为可分辨的不同强调色
REGISTER_TEST(theme_accent_deltaE, "theme accent: pairwise CIE76 ΔE >= 20", {
    core::theme::setDark();
    const auto& d = core::theme::current().colors;
    DOCTEST_CHECK(deltaE76(d.purple, d.pink) >= 20.0);
    DOCTEST_CHECK(deltaE76(d.purple, d.blue) >= 20.0);
    DOCTEST_CHECK(deltaE76(d.pink, d.blue) >= 20.0);

    core::theme::setLight();
    const auto& l = core::theme::current().colors;
    DOCTEST_CHECK(deltaE76(l.purple, l.pink) >= 20.0);
    DOCTEST_CHECK(deltaE76(l.purple, l.blue) >= 20.0);
    DOCTEST_CHECK(deltaE76(l.pink, l.blue) >= 20.0);

    core::theme::setDark();
    return true;
})

// 强调色切换：setAccent → accent getter 返回对应枚举值
REGISTER_TEST(theme_accent_select_getter, "theme accent: setAccent/getter roundtrip", {
    core::theme::setAccent(core::theme::Accent::Purple);
    DOCTEST_CHECK(core::theme::accent() == core::theme::Accent::Purple);
    core::theme::setAccent(core::theme::Accent::Pink);
    DOCTEST_CHECK(core::theme::accent() == core::theme::Accent::Pink);
    core::theme::setAccent(core::theme::Accent::Blue);
    DOCTEST_CHECK(core::theme::accent() == core::theme::Accent::Blue);
    core::theme::setAccent(core::theme::Accent::Clay);
    DOCTEST_CHECK(core::theme::accent() == core::theme::Accent::Clay);
    return true;
})

// setAccent 触发 ChangeHandler（含同值不重复通知）
REGISTER_TEST(theme_accent_select_notify, "theme accent: setAccent triggers ChangeHandler", {
    int calls = 0;
    core::theme::setChangeHandler([&](bool) { ++calls; });
    core::theme::setAccent(core::theme::Accent::Purple);
    DOCTEST_CHECK_EQ(calls, 1);
    core::theme::setAccent(core::theme::Accent::Pink);
    DOCTEST_CHECK_EQ(calls, 2);
    core::theme::setAccent(core::theme::Accent::Pink);  // same accent, no notify
    DOCTEST_CHECK_EQ(calls, 2);
    core::theme::setChangeHandler(nullptr);
    core::theme::setAccent(core::theme::Accent::Clay);
    return true;
})

// setDark/setLight 保留 accent（覆盖后重建须重新施加 applyAccentPrimary）
REGISTER_TEST(theme_accent_select_preserve, "theme accent: setDark/setLight preserves accent", {
    core::theme::setDark();
    core::theme::setAccent(core::theme::Accent::Purple);
    // 深色 Purple primary = #A18FC8 → R=0xA1
    DOCTEST_CHECK_EQ(toHexByte(core::theme::current().colors.primary.r), 0xA1);
    core::theme::setLight();
    // 浅色 Purple primary = #71639B → R=0x71
    DOCTEST_CHECK_EQ(toHexByte(core::theme::current().colors.primary.r), 0x71);
    core::theme::setDark();
    // 回到深色 purple → R=0xA1
    DOCTEST_CHECK_EQ(toHexByte(core::theme::current().colors.primary.r), 0xA1);
    core::theme::setAccent(core::theme::Accent::Clay);
    return true;
})

// 非 Clay 的 primary 在深浅下不同（切换即见色值变化）
REGISTER_TEST(theme_accent_select_values, "theme accent: non-Clay primary differs per mode", {
    core::theme::setLight();
    core::theme::setAccent(core::theme::Accent::Blue);
    // 浅色 Blue primary = #12B7F5（亮色档也是蓝）
    DOCTEST_CHECK_EQ(toHexByte(core::theme::current().colors.primary.r), 0x12);
    DOCTEST_CHECK_EQ(toHexByte(core::theme::current().colors.primary.g), 0xB7);
    DOCTEST_CHECK_EQ(toHexByte(core::theme::current().colors.primary.b), 0xF5);

    core::theme::setDark();
    core::theme::setAccent(core::theme::Accent::Blue);
    // 深色 Blue primary 同值 #12B7F5，但 primarySoftBg/primaryText 不同
    DOCTEST_CHECK_EQ(toHexByte(core::theme::current().colors.primary.r), 0x12);
    DOCTEST_CHECK_EQ(toHexByte(core::theme::current().colors.primary.g), 0xB7);
    DOCTEST_CHECK_EQ(toHexByte(core::theme::current().colors.primary.b), 0xF5);

    core::theme::setAccent(core::theme::Accent::Clay);
    return true;
})

// Pink accent light-mode hex values: primary=#B4567C, primaryText=#904563
REGISTER_TEST(theme_accent_select_pink_light, "theme accent: Pink light exact hex", {
    core::theme::setLight();
    core::theme::setAccent(core::theme::Accent::Pink);
    const auto& c = core::theme::current().colors;
    DOCTEST_CHECK_EQ(toHexByte(c.primary.r), 0xB4);
    DOCTEST_CHECK_EQ(toHexByte(c.primary.g), 0x56);
    DOCTEST_CHECK_EQ(toHexByte(c.primary.b), 0x7C);
    DOCTEST_CHECK_EQ(toHexByte(c.primaryText.r), 0x90);
    DOCTEST_CHECK_EQ(toHexByte(c.primaryText.g), 0x45);
    DOCTEST_CHECK_EQ(toHexByte(c.primaryText.b), 0x63);
    core::theme::setAccent(core::theme::Accent::Clay);
    return true;
})

// 微信色板对比度护栏（D-10）：如实记录 WeChat 品牌绿低对比权衡，不调既有 accent 护栏
// WeChat #07C160 绿作按钮底白字实测 2.385、作链接文字 on 白 2.385，均低于 WCAG AA 4.5（正文）/ 图形 AA 3.0；
// 这是 WeChat 品牌色权衡（v1.0 用户给定基线），D-10 要求如实上报用户定夺、不许改护栏放水。
// 本测试钉实际区间（值漂移即红），既有 purple/pink ≥3.0、blue [1.9,2.7] 护栏完全不动。
REGISTER_TEST(theme_wechat_contrast, "theme wechat: contrast actual-range (D-10 report-not-fudge)", {
    core::theme::setLight();
    const auto& w = core::theme::current().colors.wechat;
    // 正文文字 on 卡片白底：高对比，应达 WCAG AA 4.5（textPrimary #1A1A1A 实测 17.40，textSecondary #666666 实测 5.74）
    DOCTEST_CHECK(wcagContrast(w.textPrimary, w.cardBg) >= 4.5);
    DOCTEST_CHECK(wcagContrast(w.textSecondary, w.cardBg) >= 4.5);
    // 主色绿 #07C160 上白字（按钮场景）：WeChat 品牌绿，实测 2.385 < AA，钉区间不强行过
    double onPrimaryCR = wcagContrast(w.onPrimary, w.primary);
    DOCTEST_CHECK(onPrimaryCR >= 1.8);
    DOCTEST_CHECK(onPrimaryCR <= 2.5);
    // 主色绿作链接 on 白：实测同 2.385（与 onPrimary 同色对，对比度对称），钉区间（D-10 待用户定夺是否接受/加深）
    double linkOnWhite = wcagContrast(w.primary, w.cardBg);
    DOCTEST_CHECK(linkOnWhite >= 1.8);
    DOCTEST_CHECK(linkOnWhite <= 2.5);
    core::theme::setDark();
    return true;
})