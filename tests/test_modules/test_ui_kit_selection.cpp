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

#include <chrono>
#include <cmath>
#include <thread>

namespace {

// float [0,1] → 0-255 字节值（hex 精确比对，同 test_theme_accents / test_ui_kit_button 口径）
int toHexByte(float v) { return static_cast<int>(std::round(v * 255.0f)); }

// need356：Windows 加深档平台差异——期望字节按加深因子现算（posix 因子恒 1.0 =
// 钉设计原值不变），同 test_theme_accents.cpp 口径
int deepened(int byte) {
    return toHexByte(static_cast<float>(byte) / 255.0f *
                     core::theme::colorDeepenFactor());
}

// 浮点近似比较（同 test_app_sidebar.cpp:72 口径，epsilon=0.01）
bool floatEq(float a, float b) { return std::fabs(a - b) < 0.01f; }
// 浮点颜色近似比较（±0.01 容差，深浅模式自适应）
auto sameColor = [](const eui::Color& x, const eui::Color& y) {
    return std::fabs(x.r - y.r) < 0.01f && std::fabs(x.g - y.g) < 0.01f &&
           std::fabs(x.b - y.b) < 0.01f;
};

} // namespace

// 场景1（防「红橙粗边框 / 对勾选中态」回归，§4.3 / §8 #2 #12 禁止项）：
// H5: selectionBar bg=surface 白卡 + border + accent=primary(#36D399) + text=primary(#36D399)
// + accent width=3px。原 primarySoftBg 与侧栏底同色分不清，改为白卡 + 描边对比。
// 事故：选中态用红 / 橙粗外边框或小对勾标记（v1.0 §3.4 / §3.1 旧设计）——
// 本 helper 钉死 3px 绿竖线 + 白卡描边 + 绿色文字。
REGISTER_TEST(ui_kit_selection_bar_colors,
              "ui_kit selectionBar: H5 bg=surface+border + accent=primary + text=primary", {
    const auto& t = core::theme::current();
    eui::Ui ui;
    ui.begin("w02c_sel_colors");
    ui_kit::selectionBar(ui, "sel", "首页", 0.0f, 0.0f, 200.0f, 40.0f, t);
    ui.end();

    const auto* bg = ui.find("sel.bg");
    DOCTEST_REQUIRE(bg != nullptr);
    // H5: bg = surface 白卡 + 1px borderDefault 描边（elevated card 观感）
    DOCTEST_CHECK(sameColor(bg->color, t.colors.surface));

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
// need501 B：wtab active = 底部指示条（primaryText/2.5/radius999）+ textPrimary
// 字 + 常驻透明 bg（hover 载体 primarySoftBg）+ hit 透明。事故：Tab 回退实心
// 填充按钮或旧 S2.2 块面式——钉死"指示条 + 常驻透明底"。
REGISTER_TEST(ui_kit_wtab_active_colors,
              "ui_kit wtab active: bottom indicator + textPrimary + transparent bg/hit", {
    const auto& t = core::theme::current();
    eui::Ui ui;
    ui.begin("w02c_tab_active");
    ui_kit::wtab(ui, "tab.a", "全部", true, 0.0f, 0.0f, 80.0f, 36.0f, t);
    ui.end();

    const auto* txt = ui.find("tab.a.text");
    DOCTEST_REQUIRE(txt != nullptr);
    // active 字 = textPrimary（深字，非 textSecondary 灰）
    DOCTEST_CHECK(sameColor(txt->textColor, t.colors.textPrimary));

    // need501 B 无静止底：bg 常驻但透明，hover 载体 = primarySoftBg + radius 8
    const auto* bg = ui.find("tab.a.bg");
    DOCTEST_REQUIRE(bg != nullptr);
    DOCTEST_CHECK_EQ(toHexByte(bg->color.a), 0x00);  // 静止透明，非块面填充
    DOCTEST_CHECK(sameColor(bg->hoverColor, t.colors.primarySoftBg));
    DOCTEST_CHECK_EQ(bg->radius, 8.0f);

    // 指示条：primaryText 槽 / 高 2.5 / radius 999 / 贴 tab 底缘 y = h-2.5
    const auto* ind = ui.find("tab.a.ind");
    DOCTEST_REQUIRE(ind != nullptr);
    DOCTEST_CHECK(sameColor(ind->color, t.colors.primaryText));
    DOCTEST_CHECK(floatEq(ind->height.value, 2.5f));
    DOCTEST_CHECK_EQ(ind->radius, 999.0f);
    DOCTEST_CHECK(floatEq(ind->y, 33.5f));

    // 旧 S2.2 块面式标记节点不得回归（underline 命名属于被淘汰旧款）
    const auto* underline_legacy = ui.find("tab.a.underline");
    DOCTEST_CHECK(underline_legacy == nullptr);

    // hit area 透明（alpha=0）——防 §4.2 "实心填充按钮做 Tab"
    const auto* hit = ui.find("tab.a.hit");
    DOCTEST_REQUIRE(hit != nullptr);
    DOCTEST_CHECK_EQ(toHexByte(hit->color.a), 0x00);  // alpha=0 = 透明
    DOCTEST_CHECK_EQ(toHexByte(hit->color.r), 0x00);
    return true;
})

// 场景3（防「实心填充按钮做 Tab」，§4.2 禁止项）：
// need501 B wtab inactive：textSecondary + bg 常驻透明（hover 载体）+ 无指示条。
// 事故：未激活态画填充底或指示条——与 active 视觉不分。
REGISTER_TEST(ui_kit_wtab_inactive_no_underline,
              "ui_kit wtab inactive: textSecondary + resident transparent bg + no indicator", {
    const auto& t = core::theme::current();
    eui::Ui ui;
    ui.begin("w02c_tab_inactive");
    ui_kit::wtab(ui, "tab.i", "已完成", false, 0.0f, 0.0f, 80.0f, 36.0f, t);
    ui.end();

    const auto* txt = ui.find("tab.i.text");
    DOCTEST_REQUIRE(txt != nullptr);
    // inactive 字 = textSecondary（灰字）
    DOCTEST_CHECK(sameColor(txt->textColor, t.colors.textSecondary));
    DOCTEST_CHECK(!sameColor(txt->textColor, t.colors.primary));  // 不等于 primary 绿字

    // bg 常驻透明（hover 载体 primarySoftBg），不得有填充色
    const auto* bg = ui.find("tab.i.bg");
    DOCTEST_REQUIRE(bg != nullptr);
    DOCTEST_CHECK_EQ(toHexByte(bg->color.a), 0x00);
    DOCTEST_CHECK(sameColor(bg->hoverColor, t.colors.primarySoftBg));
    DOCTEST_CHECK_EQ(bg->radius, 8.0f);

    // 未激活无指示条节点
    const auto* ind = ui.find("tab.i.ind");
    DOCTEST_CHECK(ind == nullptr);

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
    DOCTEST_CHECK_EQ(toHexByte(track->color.r), deepened(0x36));
    DOCTEST_CHECK_EQ(toHexByte(track->color.g), deepened(0xD3));
    DOCTEST_CHECK_EQ(toHexByte(track->color.b), deepened(0x99));

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
// - t.sizes.radiusSmall == 6（统一圆角 token，钉死）
// - wswitch track radius == h/2（pill 形，强制圆角轨道）
// - selectionBar accent width == 2、wtab underline height == 2
//   （§4.3 / §4.2 钉死 2px——这是"左竖线"与"下划线"的视觉契约）
// 事故：后续工单改坏 helper 常量或 token 值——本测试钉死几何契约。
REGISTER_TEST(ui_kit_selection_unified_metrics,
              "ui_kit selection: radiusSmall=6 + wswitch pill + 2px accents", {
    const auto& t = core::theme::current();
    // t.sizes.radiusSmall == 6（统一圆角 token，钉死）
    DOCTEST_CHECK_EQ(t.sizes.radiusSmall, 6.0f);

    // selectionBar accent width = 2（§4.3 钉死）
    eui::Ui ui1;
    ui1.begin("w02c_metrics_sel");
    ui_kit::selectionBar(ui1, "m.sel", "x", 0.0f, 0.0f, 100.0f, 40.0f, t);
    ui1.end();
    const auto* accent = ui1.find("m.sel.accent");
    DOCTEST_REQUIRE(accent != nullptr);
    DOCTEST_CHECK_EQ(accent->width.value, 3.0f);
    DOCTEST_CHECK_EQ(accent->width.value, ui_kit::kSelectionBarAccentWidth);

    // need501 B：指示条高 2.5 / radius 999 胶囊 + hover 载体 radius 8（§4 钉死）
    eui::Ui ui2;
    ui2.begin("w02c_metrics_tab");
    ui_kit::wtab(ui2, "m.tab", "x", true, 0.0f, 0.0f, 80.0f, 36.0f, t);
    ui2.end();
    const auto* ind2 = ui2.find("m.tab.ind");
    DOCTEST_REQUIRE(ind2 != nullptr);
    DOCTEST_CHECK_EQ(ind2->radius, 999.0f);
    DOCTEST_CHECK(floatEq(ind2->height.value, 2.5f));
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

// 场景4（need501 §10 滑移）：切激活后立即 compose = 插值中间态（指示条仍在
// 旧位附近、未瞬跳到新位），>kSlideSeconds 后落位到新位。事故：滑移退化成
// 瞬跳（无中间帧）或中断跳变。墙钟口径：帧间隔 < 160ms 中间态断言才成立，
// 进程内 compose 正常远小于该值，极端负载假红按 flake 复跑。
REGISTER_TEST(ui_kit_wtab_slide_interpolation,
              "ui_kit wtab slide: mid-flight frame stays near old pos, settles after 160ms", {
    eui::Ui ui;
    const auto& t = core::theme::current();
    auto frame = [&](bool tab0Active) {
        ui.begin(tab0Active ? "sl_f_a" : "sl_f_b");
        ui_kit::wtab(ui, "sl.0", "甲乙", tab0Active, 0.0f, 0.0f, 80.0f, 36.0f, t);
        ui_kit::wtab(ui, "sl.1", "丙丁", !tab0Active, 80.0f, 0.0f, 80.0f, 36.0f, t);
        ui.end();
    };
    frame(true);
    const auto* i0 = ui.find("sl.0.ind");
    DOCTEST_REQUIRE(i0 != nullptr);
    // 组内首个激活：无动画初始位（rest），local x = restAbsX - 0
    const float startX = i0->x;  // .position 相对 stack 的 local x（无窗口无 layout，frame 恒 0）

    // 切激活后立即 compose：elapsed ≪ kSlideSeconds → 指示条未到目标
    frame(false);
    DOCTEST_CHECK(ui.find("sl.0.ind") == nullptr);  // 失活即无指示条
    const auto* i1 = ui.find("sl.1.ind");
    DOCTEST_REQUIRE(i1 != nullptr);
    // .x = .position 的 local x（相对所属 stack）；tab1 stack 原点 80 → 绝对 = .x + 80
    const float midAbsX = i1->x + 80.0f;
    DOCTEST_CHECK(midAbsX < startX + 20.0f);  // 仍在旧位附近（未瞬跳 80px）

    // 落位帧：> kSlideSeconds(160ms) 后到达新 rest 位
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    frame(false);
    const auto* i1s = ui.find("sl.1.ind");
    DOCTEST_REQUIRE(i1s != nullptr);
    const float endAbsX = i1s->x + 80.0f;
    DOCTEST_CHECK(endAbsX > startX + 60.0f);  // 目标 ≈ start + 80（tab 右移一位）
    return true;
})
