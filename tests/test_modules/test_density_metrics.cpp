// tests/test_modules/test_density_metrics.cpp
// need534（批次 D）契约护栏：页面布局密度档的平台拆分。
//
// 两件事分开守：
//   ① **通用契约**（跨平台）：所有密度值 > 0 且是 4 的倍数（125% 缩放零半像素）、
//      headerH 落在 [32, 48] 的合理区间。这些不断言具体值 —— 否则 Windows 调档
//      会假红。
//   ② **mac 零变化**（仅非 Windows）：本文件在 mac/Linux 上跑时，把 posix 档的
//      5 个值逐个钉成「批次 D 改动前的字面量」。老板 2026-11-27 硬约束是 mac 布局
//      像素级不变，这条用例就是它的静态守卫：谁把 density_posix.cpp 的值改了，
//      这条立刻红。
#include "doctest.h"

#include "ui_kit/density.h"

#include <cmath>

namespace {

// 4 的倍数且 > 0（125% 缩放下 4×1.25=5 整数；2 的倍数不够，1.25×2=2.5 有半像素）
bool isQuarterMultiple(float v) {
    return v > 0.0f && std::fabs(std::fmod(v, 4.0f)) < 0.001f;
}

}  // namespace

// 通用契约（两平台都跑）：值域合法
REGISTER_TEST(density_values_are_valid_quarters,
              "布局密度：所有密度值为正的 4 的倍数，headerH 落在 [32,48]", {
    const float values[] = {
        ui_kit::density::pageInsetV(), ui_kit::density::cardGap(),
        ui_kit::density::cardPadV(),   ui_kit::density::listRowH(),
        ui_kit::density::headerH(),
    };
    for (const float v : values) {
        DOCTEST_CHECK_MESSAGE(isQuarterMultiple(v),
                              std::string("非法密度值 ") + std::to_string(v));
    }
    const float header = ui_kit::density::headerH();
    DOCTEST_CHECK(header >= 32.0f && header <= 48.0f);
    return true;
});

// mac/Linux 零变化守卫：posix 档必须与批次 D 改动前逐字相同。
// 用 DEVDESK_DENSITY_EXPECT_POSIX 区分平台——CMake 只在非 WIN32 时定义它
//（见 tests/CMakeLists.txt），Windows 上本用例自动跳过而不是假红。
#ifdef DEVDESK_DENSITY_EXPECT_POSIX
REGISTER_TEST(density_posix_values_unchanged,
              "布局密度：mac/Linux 档与批次 D 改动前逐字相同（像素级零变化守卫）", {
    // 这五个值 = 改动前散落在各页的裸字面量。改任何一个都意味着 mac 观感变了。
    DOCTEST_CHECK_EQ(ui_kit::density::pageInsetV(), 32.0f);
    DOCTEST_CHECK_EQ(ui_kit::density::cardGap(), 16.0f);
    DOCTEST_CHECK_EQ(ui_kit::density::cardPadV(), 20.0f);
    DOCTEST_CHECK_EQ(ui_kit::density::listRowH(), 44.0f);
    DOCTEST_CHECK_EQ(ui_kit::density::headerH(), 40.0f);
    return true;
});
#endif

// Windows 紧凑档必须**小于等于** mac 档（紧凑方向不得反过来变松）。
// 两个平台都跑：mac 上比的是同一份值（恒等成立），Windows 上才是真比较。
REGISTER_TEST(density_compact_never_looser_than_baseline,
              "布局密度：各档值不超过设计基线（紧凑方向单调）", {
    // 基线 = 改动前的 mac 值，作为上限常量写死
    DOCTEST_CHECK(ui_kit::density::pageInsetV() <= 32.0f);
    DOCTEST_CHECK(ui_kit::density::cardGap() <= 16.0f);
    DOCTEST_CHECK(ui_kit::density::cardPadV() <= 20.0f);
    DOCTEST_CHECK(ui_kit::density::listRowH() <= 44.0f);
    DOCTEST_CHECK(ui_kit::density::headerH() <= 40.0f);
    return true;
});
