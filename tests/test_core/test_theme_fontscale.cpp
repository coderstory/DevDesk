// tests/test_core/test_theme_fontscale.cpp
// 全局字号缩放（core::theme::setFontScale）纯函数测试：
// token 字号按比例缩放 / 间距圆角不动 / 切主题不丢不叠加 / 越界钳制
#include "doctest.h"
#include "core/theme.h"

REGISTER_TEST(theme_fontscale_scales_size_tokens, "theme: setFontScale scales fontSize tokens only", {
    core::theme::setFontScale(1.0f);
    DOCTEST_CHECK_EQ(core::theme::fontScale(), 1.0f);
    DOCTEST_CHECK_EQ(core::theme::current().sizes.fontSizeBody, 15.0f);

    core::theme::setFontScale(1.2f);
    DOCTEST_CHECK_EQ(core::theme::fontScale(), 1.2f);
    // 全部字号档位同步缩放（token 读取层，模块零改动受益）
    DOCTEST_CHECK_EQ(core::theme::current().sizes.fontSizeBody, 15.0f * 1.2f);
    DOCTEST_CHECK_EQ(core::theme::current().sizes.fontSizeSmall, 13.0f * 1.2f);
    DOCTEST_CHECK_EQ(core::theme::current().sizes.fontSizePage, 32.0f * 1.2f);
    DOCTEST_CHECK_EQ(core::theme::current().sizes.fontSizeDisplay, 48.0f * 1.2f);
    // 间距 / 圆角不缩放（布局度量保持稳定）
    DOCTEST_CHECK_EQ(core::theme::current().sizes.spacing, 8.0f);
    DOCTEST_CHECK_EQ(core::theme::current().sizes.radius, 10.0f);
    DOCTEST_CHECK_EQ(core::theme::current().sizes.radiusLarge, 14.0f);

    core::theme::setFontScale(1.0f);  // 恢复，不影响其他用例
    return true;
})

REGISTER_TEST(theme_fontscale_survives_theme_switch, "theme: font scale survives setDark/setLight (no compounding)", {
    core::theme::setFontScale(1.15f);
    core::theme::setLight();
    DOCTEST_CHECK_EQ(core::theme::fontScale(), 1.15f);
    DOCTEST_CHECK_EQ(core::theme::current().sizes.fontSizeBody, 15.0f * 1.15f);
    // 再切回深色：仍是 15 × 1.15（重建 token 后统一施加，不叠加不丢失）
    core::theme::setDark();
    DOCTEST_CHECK_EQ(core::theme::current().sizes.fontSizeBody, 15.0f * 1.15f);
    core::theme::setFontScale(1.0f);
    DOCTEST_CHECK_EQ(core::theme::current().sizes.fontSizeBody, 15.0f);
    return true;
})

REGISTER_TEST(theme_fontscale_clamped, "theme: setFontScale clamps to [0.5, 2.0]", {
    core::theme::setFontScale(0.1f);
    DOCTEST_CHECK_EQ(core::theme::fontScale(), 0.5f);
    core::theme::setFontScale(99.0f);
    DOCTEST_CHECK_EQ(core::theme::fontScale(), 2.0f);
    core::theme::setFontScale(1.0f);
    return true;
})
