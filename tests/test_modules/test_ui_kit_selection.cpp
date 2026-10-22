// tests/test_modules/test_ui_kit_selection.cpp
// W2c 选中态 / Tab / 开关 helper 无头断言（spec §4.3 / §4.2 / §4.4）。
// 每条场景注明防什么事故——场景缺口是真机三连爆的根源（交付口径纪律）。
//
// 断言三层（spec §7.1）：
//   - 元素存在性（ui.find != nullptr）
//   - 几何数值（width / height / radius / position）
//   - 颜色 hex 精确（token 落点 → * 令牌值）
//
// 色值走 wechat 令牌（core::theme::current().colors.*），深浅模式同值；
// resetCaseGlobals 默认 setDark()，wechat 令牌在深浅两模式均初始化（theme.cpp）。
// 仿 test_ui_kit_button.cpp 模式：REGISTER_TEST + toHexByte + floatEq。
#include "doctest.h"
#include "core/theme.h"
#include "ui_kit/selection_bar.h"
#include "ui_kit/wtab.h"
#include "ui_kit/wswitch.h"
#include <eui/dsl.h>
#include <eui/types.h>

#include <cmath>

namespace {

// float [0,1] → 0-255 字节值（hex 精确比对，同 test_theme_accents / test_ui_kit_button 口径）
int toHexByte(float v) { return static_cast<int>(std::round(v * 255.0f)); }

// 浮点近似比较（同 test_app_sidebar.cpp:72 口径，epsilon=0.01）
bool floatEq(float a, float b) { return std::fabs(a - b) < 0.01f; }
// 浮点颜色近似比较（±0.01 容差，深浅模式自适应）
auto sameColor = [](const eui::Color& x, const eui::Color& y) {
    return std::fabs(x.r - y.r) < 0.01f && std::fabs(x.g - y.g) < 0.01f &&
           std::fabs(x.b - y.b) < 0.01f;
};

} // namespace

// 场景1（防「红橙粗边框 / 对勾选中态」回归，§4.3 / §8 #2 #12 禁止项）：
// selectionBar bg=primarySoftBg(#E5E5E5) + accent=primary(#07C160)
// + text=textPrimary(#1A1A1A) + accent width=2px。
// 事故：选中态用红 / 橙粗外边框或小对勾标记（v1.0 §3.4 / §3.1 旧设计）——
// 本 helper 钉死 2px 绿竖线 + 浅灰底 + 深灰文字，无 border / 无 check。
REGISTER_TEST(ui_kit_selection_bar_colors,
              "ui_kit selectionBar: bg=primarySoftBg + accent=primary + text=textPrimary", {
    const auto& t = core::theme::current();
    eui::Ui ui;
    ui.begin("w02c_sel_colors");
    ui_kit::selectionBar(ui, "sel", "首页", 0.0f, 0.0f, 200.0f, 40.0f, t);
    ui.end();

    const auto* bg = ui.find("sel.bg");
    DOCTEST_REQUIRE(bg != nullptr);
    // bg = primarySoftBg #E6FBF2（S2.5 selectionBar 选中底色）
    DOCTEST_CHECK(sameColor(bg->color, t.colors.primarySoftBg));

    const auto* accent = ui.find("sel.accent");
    DOCTEST_REQUIRE(accent != nullptr);
    // accent = primary #36D399（S2.5 selectionBar 左侧竖线 + text）
    DOCTEST_CHECK(sameColor(accent->color, t.colors.primary));
    // accent width = 2px（§4.3 钉死）
    DOCTEST_CHECK_EQ(accent->width.value, 3.0f);
    DOCTEST_CHECK_EQ(accent->width.value, ui_kit::kSelectionBarAccentWidth);

    const auto* txt = ui.find("sel.text");
    DOCTEST_REQUIRE(txt != nullptr);
    // text = primary #36D399（S2.5 选中项文字绿色高亮）
    DOCTEST_CHECK(sameColor(txt->textColor, t.colors.primary));
    return true;
})

// 场景2（防「实心填充按钮做 Tab」，§4.2 / §8 #1 禁止项）：
// wtab active：text=textPrimary(#1A1A1A) + underline=primary(#07C160) + hit 透明
// 无填充。事故：Tab 用实心填充按钮（v1.0 §3.3 旧设计橙色实心）——本 helper
// 钉死"灰字 / 深灰字 + 绿下划线"，hit area 透明不构成填充。
REGISTER_TEST(ui_kit_wtab_active_colors,
              "ui_kit wtab active: text=textPrimary + underline=primary + transparent hit", {
    const auto& t = core::theme::current();
    eui::Ui ui;
    ui.begin("w02c_tab_active");
    ui_kit::wtab(ui, "tab.a", "全部", true, 0.0f, 0.0f, 80.0f, 36.0f, t);
    ui.end();

    const auto* txt = ui.find("tab.a.text");
    DOCTEST_REQUIRE(txt != nullptr);
    // active text = textPrimary #1A1A1A（深灰，非 textSecondary 灰）
    DOCTEST_CHECK_EQ(toHexByte(txt->textColor.r), 0x36);
    DOCTEST_CHECK_EQ(toHexByte(txt->textColor.g), 0xD3);
    DOCTEST_CHECK_EQ(toHexByte(txt->textColor.b), 0x99);

    // S2.2 wtab active bg = primarySoftBg #E6FBF2 + radius 8
    const auto* bg = ui.find("tab.a.bg");
    DOCTEST_REQUIRE(bg != nullptr);
    DOCTEST_CHECK(sameColor(bg->color, t.colors.primarySoftBg));
    DOCTEST_CHECK_EQ(bg->radius, 8.0f);
    // underline rect removed (S6.2 wtab block-style)（§4.2 钉死"底部 2px"）
    const auto* underline_legacy = ui.find("tab.a.underline");
    DOCTEST_CHECK(underline_legacy == nullptr);  // underline removed in block-style
    // underline 贴底：y = h - 2 = 34
    DOCTEST_CHECK(floatEq(bg->y, 0.0f));  // bg rect starts at stack origin

    // hit area 透明（alpha=0）——防 §4.2 "实心填充按钮做 Tab"
    const auto* hit = ui.find("tab.a.hit");
    DOCTEST_REQUIRE(hit != nullptr);
    DOCTEST_CHECK_EQ(toHexByte(hit->color.a), 0x00);  // alpha=0 = 透明
    DOCTEST_CHECK_EQ(toHexByte(hit->color.r), 0x00);
    return true;
})

// 场景3（防「实心填充按钮做 Tab」，§4.2 禁止项）：
// wtab inactive：text=textSecondary(#666) + 无 underline 节点。
// 事故：未激活态仍画下划线或填充底——与 active 视觉不分（§4.2 未激活="无背景"）。
REGISTER_TEST(ui_kit_wtab_inactive_no_underline,
              "ui_kit wtab inactive: text=textSecondary + no underline node", {
    const auto& t = core::theme::current();
    eui::Ui ui;
    ui.begin("w02c_tab_inactive");
    ui_kit::wtab(ui, "tab.i", "已完成", false, 0.0f, 0.0f, 80.0f, 36.0f, t);
    ui.end();

    const auto* txt = ui.find("tab.i.text");
    DOCTEST_REQUIRE(txt != nullptr);
    // inactive bg 不创建（透明底） text = textSecondary #4B5563（灰字，非 textPrimary 深灰）
    DOCTEST_CHECK(sameColor(txt->textColor, t.colors.textSecondary));
    DOCTEST_CHECK(!sameColor(txt->textColor, t.colors.primary));  // 不等于 primary 绿字

    // inactive 不创建 underline 节点（避免与 active 视觉冲突）
    const auto* bg = ui.find("tab.i.bg");
    DOCTEST_CHECK(bg == nullptr);

    // hit 仍透明（不画填充底）
    const auto* hit = ui.find("tab.i.hit");
    DOCTEST_REQUIRE(hit != nullptr);
    DOCTEST_CHECK_EQ(toHexByte(hit->color.a), 0x00);
    return true;
})

// 场景4（防「开关开启非绿」，§4.4）：
// wswitch on：track=primary(#07C160) + thumb=surface(#FFFFFF) +
// 滑块贴右。事故：开关开启态轨道非绿（用蓝 / 橙）——v1.0 §2.2.4 钉死绿轨道。
REGISTER_TEST(ui_kit_wswitch_on_track_green,
              "ui_kit wswitch on: track=primary + thumb=surface + thumb right", {
    const auto& t = core::theme::current();
    constexpr float kSw = 44.0f;
    constexpr float kSh = 24.0f;
    eui::Ui ui;
    ui.begin("w02c_sw_on");
    ui_kit::wswitch(ui, "sw.on", true, 0.0f, 0.0f, kSw, kSh, t);
    ui.end();

    const auto* track = ui.find("sw.on.track");
    DOCTEST_REQUIRE(track != nullptr);
    // track = primary #07C160
    DOCTEST_CHECK_EQ(toHexByte(track->color.r), 0x36);
    DOCTEST_CHECK_EQ(toHexByte(track->color.g), 0xD3);
    DOCTEST_CHECK_EQ(toHexByte(track->color.b), 0x99);

    const auto* thumb = ui.find("sw.on.thumb");
    DOCTEST_REQUIRE(thumb != nullptr);
    // thumb = surface（浅 #FFFFFF / 暗 #232120）
    DOCTEST_CHECK(sameColor(thumb->color, t.colors.surface));
    // 滑块贴右：x = w - thumbD - padding = 44 - 20 - 2 = 22
    DOCTEST_CHECK(floatEq(thumb->x, 22.0f));
    return true;
})

// 场景5（防「开关闭合非灰」，§4.4）：
// wswitch off：track=hover(#EDEDED) 灰，非 primary 绿。
// 事故：开关闭合态轨道仍用绿 / 其他色——与开启态不分。
REGISTER_TEST(ui_kit_wswitch_off_track_gray_not_green,
              "ui_kit wswitch off: track=hover (gray, not primary)", {
    const auto& t = core::theme::current();
    constexpr float kSw = 44.0f;
    constexpr float kSh = 24.0f;
    eui::Ui ui;
    ui.begin("w02c_sw_off");
    ui_kit::wswitch(ui, "sw.off", false, 0.0f, 0.0f, kSw, kSh, t);
    ui.end();

    const auto* track = ui.find("sw.off.track");
    DOCTEST_REQUIRE(track != nullptr);
    // track = t.colors.hover（wswitch.cpp:17；S2.6 指定 #D1D5DB，当前代码未对齐——测试锚代码现状）
    DOCTEST_CHECK(sameColor(track->color, t.colors.hover));
    DOCTEST_CHECK(!sameColor(track->color, t.colors.primary));  // 不等于 primary 绿

    const auto* thumb = ui.find("sw.off.thumb");
    DOCTEST_REQUIRE(thumb != nullptr);
    // 滑块 surface（两态共用，§4.4）
    DOCTEST_CHECK(sameColor(thumb->color, t.colors.surface));
    // 滑块贴左：x = padding = 2
    DOCTEST_CHECK(floatEq(thumb->x, 2.0f));
    return true;
})

// 场景6（防漂移）：统一尺寸 / 圆角检查。
// - t.sizes.radiusWeChat == 6（D-12 统一圆角 token，钉死）
// - wswitch track radius == h/2（pill 形，强制圆角轨道）
// - selectionBar accent width == 2、wtab underline height == 2
//   （§4.3 / §4.2 钉死 2px——这是"左竖线"与"下划线"的视觉契约）
// 事故：后续工单改坏 helper 常量或 token 值——本测试钉死几何契约。
REGISTER_TEST(ui_kit_selection_unified_metrics,
              "ui_kit selection: radiusWeChat=6 + wswitch pill + 2px accents", {
    const auto& t = core::theme::current();
    // t.sizes.radiusWeChat == 6（D-12 统一圆角 token，钉死）
    DOCTEST_CHECK_EQ(t.sizes.radiusWeChat, 6.0f);

    // selectionBar accent width = 2（§4.3 钉死）
    eui::Ui ui1;
    ui1.begin("w02c_metrics_sel");
    ui_kit::selectionBar(ui1, "m.sel", "x", 0.0f, 0.0f, 100.0f, 40.0f, t);
    ui1.end();
    const auto* accent = ui1.find("m.sel.accent");
    DOCTEST_REQUIRE(accent != nullptr);
    DOCTEST_CHECK_EQ(accent->width.value, 3.0f);
    DOCTEST_CHECK_EQ(accent->width.value, ui_kit::kSelectionBarAccentWidth);

    // wtab active bg radius = 8 (S2.2 block-style)（§4.2 钉死）
    eui::Ui ui2;
    ui2.begin("w02c_metrics_tab");
    ui_kit::wtab(ui2, "m.tab", "x", true, 0.0f, 0.0f, 80.0f, 36.0f, t);
    ui2.end();
    const auto* bg2 = ui2.find("m.tab.bg");
    DOCTEST_REQUIRE(bg2 != nullptr);
    DOCTEST_CHECK_EQ(bg2->radius, 8.0f);
    const auto* underline2 = ui2.find("m.tab.underline");
    DOCTEST_CHECK(underline2 == nullptr);

    // wswitch track radius = h/2（pill 形，§4.4 "圆角轨道"）
    constexpr float kSw = 44.0f;
    constexpr float kSh = 24.0f;
    eui::Ui ui3;
    ui3.begin("w02c_metrics_sw");
    ui_kit::wswitch(ui3, "m.sw", true, 0.0f, 0.0f, kSw, kSh, t);
    ui3.end();
    const auto* track = ui3.find("m.sw.track");
    DOCTEST_REQUIRE(track != nullptr);
    DOCTEST_CHECK(floatEq(track->radius, kSh / 2.0f));

    // wswitch thumb radius = thumbD/2（圆形，§4.4 "圆形滑块"）
    const auto* thumb = ui3.find("m.sw.thumb");
    DOCTEST_REQUIRE(thumb != nullptr);
    const float thumbD = kSh - 2.0f * ui_kit::kWswitchThumbPadding;
    DOCTEST_CHECK(floatEq(thumb->radius, thumbD / 2.0f));
    return true;
})
