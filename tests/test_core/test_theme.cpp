#include "doctest.h"
#include "core/theme.h"

#include <eui/types.h>

DOCTEST_TEST_CASE("theme: default is dark") {
    DOCTEST_CHECK(core::theme::current().isDark);
    DOCTEST_CHECK_EQ(core::theme::currentThemeName(), std::string("Dark"));
}

DOCTEST_TEST_CASE("theme: setLight") {
    core::theme::setLight();
    DOCTEST_CHECK_FALSE(core::theme::current().isDark);
    DOCTEST_CHECK_EQ(core::theme::currentThemeName(), std::string("Light"));
    core::theme::setDark();
    DOCTEST_CHECK(core::theme::current().isDark);
}

DOCTEST_TEST_CASE("theme: tokens have values") {
    const auto& t = core::theme::current();
    // 大气档尺寸：按钮 44px、正文 15px、标题 32px、display 48px
    DOCTEST_CHECK_EQ(t.sizes.fontSizeBody, 15.0f);
    DOCTEST_CHECK_EQ(t.sizes.fontSizeDisplay, 48.0f);
    DOCTEST_CHECK(t.sizes.radius >= 10.0f);
    DOCTEST_CHECK_EQ(t.sizes.spacing, 8.0f);
}

DOCTEST_TEST_CASE("theme: dark and light differ") {
    core::theme::setDark();
    auto darkBg = core::theme::current().colors.background;
    core::theme::setLight();
    auto lightBg = core::theme::current().colors.background;
    // dark bg 与 light bg 应该不同
    DOCTEST_CHECK(darkBg.r != lightBg.r || darkBg.g != lightBg.g || darkBg.b != lightBg.b);
    core::theme::setDark();
}