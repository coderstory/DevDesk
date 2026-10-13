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
// blue 浅色档锁在 [2.3, 2.7]（品牌色档位，非正文色），深色档 ≥ 3.0
REGISTER_TEST(theme_accent_contrast, "theme accent: contrast ratio guards", {
    core::theme::setLight();
    const auto& l = core::theme::current().colors;
    const eui::Color lightBg("#F5F2EC");
    DOCTEST_CHECK(wcagContrast(l.purple, lightBg) >= 3.0);
    DOCTEST_CHECK(wcagContrast(l.pink, lightBg) >= 3.0);
    double blueLightCR = wcagContrast(l.blue, lightBg);
    DOCTEST_CHECK(blueLightCR >= 2.3);
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