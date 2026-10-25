// tests/test_modules/test_ui_kit_slider.cpp
// UAT6.2 字号无级调节：ui_kit::slider 无头断言 + 设置页取值映射。
// 断言三层（spec §7.1）：节点存在性（ui.find）/ 几何数值 / 颜色 token 落点。
// 场景缺口纪律：每条注明防什么事故。
#include "doctest.h"

#include "core/theme.h"
#include "module_settings/settings_config.h"
#include "module_settings/settings_ui_logic.h"
#include "ui_kit/slider.h"

#include <eui/dsl.h>

#include <cmath>

namespace {

// 浮点近似比较（几何断言容差）
auto nearF = [](float x, float y) { return std::fabs(x - y) < 0.01f; };

} // namespace

// 场景1（防「结构 id 漂移 / 取值通道没接上」）：四节点齐全 + stack 挂 sliderState
//（拖拽取值 = EUI 运行时托管，sliderInputFrom 缺失 = 点击/拖拽全失灵）。
REGISTER_TEST(ui_kit_slider_nodes_and_input_binding,
              "ui_kit slider: track/fill/handle/hit 四节点 + sliderState/inputFrom 绑定", {
    const auto& t = core::theme::current();
    eui::Ui ui;
    ui.begin("s01");
    ui_kit::slider(ui, "s1", 0.0f, 0.0f, 200.0f, 36.0f, 0.5f, t,
                   [](float) {});
    ui.end();

    const auto* stack = ui.find("s1");
    DOCTEST_REQUIRE(stack != nullptr);
    // 四个 id 字段（state/input/fill/knob）都经 eui resolveId 归一（可能带作用域
    // 前缀），非空即绑定成立——语义锁「取值通道接上了」，不锁 id 字符串形态
    DOCTEST_CHECK(nearF(stack->sliderValue, 0.5f));
    DOCTEST_CHECK(nearF(stack->sliderWidth, 200.0f));
    DOCTEST_CHECK(nearF(stack->sliderKnobSize, ui_kit::kSliderHandleSize));
    DOCTEST_CHECK(stack->onSliderValueChanged != nullptr);

    const auto* hit = ui.find("s1.hit");
    DOCTEST_REQUIRE(hit != nullptr);
    DOCTEST_CHECK(hit->interactive);
    DOCTEST_CHECK(!hit->sliderInputSourceId.empty());

    const auto* fill = ui.find("s1.fill");
    DOCTEST_REQUIRE(fill != nullptr);
    DOCTEST_CHECK(!fill->sliderFillSourceId.empty());

    const auto* handle = ui.find("s1.handle");
    DOCTEST_REQUIRE(handle != nullptr);
    DOCTEST_CHECK(!handle->sliderKnobSourceId.empty());

    const auto* track = ui.find("s1.track");
    DOCTEST_REQUIRE(track != nullptr);
    return true;
})

// 场景2（防「颜色落错 token」）：轨道 hover / 填充 primary / 手柄 surface 白底
// + 1px borderDefault 描边——生产码不新增裸 hex 的 token 落点锁。
REGISTER_TEST(ui_kit_slider_token_colors,
              "ui_kit slider: track=hover + fill=primary + handle=surface/borderDefault", {
    const auto& t = core::theme::current();
    eui::Ui ui;
    ui.begin("s02");
    ui_kit::slider(ui, "s2", 0.0f, 0.0f, 200.0f, 36.0f, 0.5f, t);
    ui.end();
    eui::Screen scr2{400.0f, 300.0f};
    ui.layout(scr2.width, scr2.height);   // frame 几何须经 layout 才有值

    const auto* track = ui.find("s2.track");
    DOCTEST_REQUIRE(track != nullptr);
    DOCTEST_CHECK(nearF(track->color.r, t.colors.hover.r));
    DOCTEST_CHECK(nearF(track->color.g, t.colors.hover.g));
    DOCTEST_CHECK(nearF(track->color.b, t.colors.hover.b));
    DOCTEST_CHECK(nearF(track->frame.height, ui_kit::kSliderTrackHeight));
    DOCTEST_CHECK(nearF(track->radius, t.sizes.radiusSmall));

    const auto* fill = ui.find("s2.fill");
    DOCTEST_REQUIRE(fill != nullptr);
    DOCTEST_CHECK(nearF(fill->color.r, t.colors.primary.r));
    DOCTEST_CHECK(nearF(fill->color.g, t.colors.primary.g));
    DOCTEST_CHECK(nearF(fill->color.b, t.colors.primary.b));

    const auto* handle = ui.find("s2.handle");
    DOCTEST_REQUIRE(handle != nullptr);
    DOCTEST_CHECK(nearF(handle->color.r, t.colors.surface.r));
    DOCTEST_CHECK(nearF(handle->color.g, t.colors.surface.g));
    DOCTEST_CHECK(nearF(handle->color.b, t.colors.surface.b));
    DOCTEST_CHECK(nearF(handle->border.width, 1.0f));
    DOCTEST_CHECK(nearF(handle->border.color.r, t.colors.borderDefault.r));
    DOCTEST_CHECK(nearF(handle->border.color.g, t.colors.borderDefault.g));
    DOCTEST_CHECK(nearF(handle->border.color.b, t.colors.borderDefault.b));
    return true;
})

// 场景3（防「手柄行程映射与 runtime 取值口径脱钩」）：compose 期手柄初值 x =
// value*(w-16)（与 runtime sliderValueFromPointer 的手柄中心映射同式）；越界
// value 钳进 [0,1]（旧库存越界值回放不画出界）。
REGISTER_TEST(ui_kit_slider_value_mapping_and_clamp,
              "ui_kit slider: handle x=value*(w-knob) 同 runtime 口径 + 越界钳制 [0,1]", {
    const auto& t = core::theme::current();
    eui::Ui ui;
    ui.begin("s03");
    ui_kit::slider(ui, "s3", 0.0f, 0.0f, 200.0f, 36.0f, 0.25f, t);
    ui.end();
    ui.layout(400.0f, 100.0f);

    const auto* handle = ui.find("s3.handle");
    DOCTEST_REQUIRE(handle != nullptr);
    DOCTEST_CHECK(nearF(handle->frame.x, 0.25f * (200.0f - ui_kit::kSliderHandleSize)));

    eui::Ui ui2;
    ui2.begin("s03hi");
    ui_kit::slider(ui2, "s3hi", 0.0f, 0.0f, 200.0f, 36.0f, 2.0f, t);
    ui2.end();
    const auto* hi = ui2.find("s3hi");
    DOCTEST_REQUIRE(hi != nullptr);
    DOCTEST_CHECK(nearF(hi->sliderValue, 1.0f));

    eui::Ui ui3;
    ui3.begin("s03lo");
    ui_kit::slider(ui3, "s3lo", 0.0f, 0.0f, 200.0f, 36.0f, -1.0f, t);
    ui3.end();
    const auto* lo = ui3.find("s3lo");
    DOCTEST_REQUIRE(lo != nullptr);
    DOCTEST_CHECK(nearF(lo->sliderValue, 0.0f));
    return true;
})

// 场景4（防「映射函数偏移/分辨率回归」）：归一化值 → 百分比 1% 分辨率取整，
// min/max/中点/越界钳制四类钉值（UAT6.2 无级调节 80-130%）。
REGISTER_TEST(settings_font_scale_percent_from_ratio,
              "settings: fontScalePercentFromRatio min/max/中点/越界钳制", {
    namespace ms = module::settings;
    DOCTEST_CHECK_EQ(ms::fontScalePercentFromRatio(0.0f), ms::kMinFontScalePercent);
    DOCTEST_CHECK_EQ(ms::fontScalePercentFromRatio(1.0f), ms::kMaxFontScalePercent);
    DOCTEST_CHECK_EQ(ms::fontScalePercentFromRatio(0.5f), 105);
    DOCTEST_CHECK_EQ(ms::fontScalePercentFromRatio(0.7f), 115);
    DOCTEST_CHECK_EQ(ms::fontScalePercentFromRatio(1.7f), ms::kMaxFontScalePercent);
    DOCTEST_CHECK_EQ(ms::fontScalePercentFromRatio(-0.4f), ms::kMinFontScalePercent);
    DOCTEST_CHECK_EQ(ms::kMinFontScalePercent, 80);  // UAT6.2 下限 85→80
    return true;
})
