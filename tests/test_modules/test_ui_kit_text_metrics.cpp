// tests/test_modules/test_ui_kit_text_metrics.cpp
// need:296 D2：ui_kit::cachedTextWidth 统一估宽入口。缓存对调用方是行为透明的
// 纯函数（命中与否结果一致），断言承载两点：① weight 维度真实入键——若实现丢
// weight（一律按 400 测），600 档与直测对不上即红；② 同键重入一致 + 不同文本
// 不互串（键拼接错误防回归）。
#include "doctest.h"

#include "core/render/text.h"
#include "ui_kit/text_metrics.h"

REGISTER_TEST(ui_kit_text_metrics_weight_keyed,
              "cachedTextWidth: weight 维度入键，与直测对表不串档", {
    const float w400 = ui_kit::cachedTextWidth("估宽字重维度", 14.0f, 400);
    const float w600 = ui_kit::cachedTextWidth("估宽字重维度", 14.0f, 600);
    DOCTEST_CHECK(w400 > 0.0f);
    DOCTEST_CHECK(w600 > 0.0f);
    DOCTEST_CHECK_EQ(w400, core::TextPrimitive::measureTextWidth(
                               "估宽字重维度", "", 14.0f, 400));
    DOCTEST_CHECK_EQ(w600, core::TextPrimitive::measureTextWidth(
                               "估宽字重维度", "", 14.0f, 600));
    return true;
});

REGISTER_TEST(ui_kit_text_metrics_repeat_stable,
              "cachedTextWidth: 同键重入一致（缓存路径与直测路径同值）", {
    const float a = ui_kit::cachedTextWidth("重入探针ABC", 13.0f, 500);
    const float b = ui_kit::cachedTextWidth("重入探针ABC", 13.0f, 500);
    DOCTEST_CHECK_EQ(a, b);
    DOCTEST_CHECK_EQ(a, core::TextPrimitive::measureTextWidth(
                            "重入探针ABC", "", 13.0f, 500));
    return true;
});

REGISTER_TEST(ui_kit_text_metrics_text_keyed,
              "cachedTextWidth: 不同文本不互串（长文本宽于短文本）", {
    const float x = ui_kit::cachedTextWidth("短", 12.0f, 400);
    const float y = ui_kit::cachedTextWidth("明显更长的一段文本", 12.0f, 400);
    DOCTEST_CHECK(y > x);
    return true;
});
