// tests/test_modules/test_ui_kit_button.cpp
// W2b 四级按钮 + FormRow helper 无头断言（spec §4.1 / §4.15）。
// 每条场景注明防什么事故——场景缺口是真机三连爆的根源（交付口径纪律）。
//
// 断言三层（spec §7.1）：
//   - 元素存在性（ui.find != nullptr）
//   - 几何数值（height / radius / fontSize / label width / control geometry）
//   - 颜色 hex 精确（token 落点 → wechat.* 令牌值）
//
// 色值走 wechat 令牌（core::theme::current().colors.wechat.*），深浅模式同值；
// resetCaseGlobals 默认 setDark()，wechat 令牌在深浅两模式均初始化（theme.cpp）。
#include "doctest.h"
#include "core/theme.h"
#include "ui_kit/button.h"
#include "ui_kit/form_row.h"
#include <eui/dsl.h>
#include <eui/types.h>

#include <cmath>

namespace {

// float [0,1] → 0-255 字节值（hex 精确比对，同 test_theme_accents 口径）
int toHexByte(float v) { return static_cast<int>(std::round(v * 255.0f)); }

} // namespace

// 场景1（防「主按钮非绿/字非白」）：Primary 渲染 bg=wechat.primary(#07C160)
// + text=wechat.onPrimary(#FFFFFF)。事故：主按钮用橙/灰等非绿底、或字色非白
// 致可读性塌陷（D-01 微信基线 vs v1.1 橙系冲突——绿白必须钉死）。
REGISTER_TEST(ui_kit_button_primary_colors,
              "ui_kit button: Primary bg=wechat.primary(#07C160) + text=wechat.onPrimary(#FFFFFF)", {
    const auto& t = core::theme::current();
    eui::Ui ui;
    ui.begin("w02b_btn_primary");
    ui_kit::button(ui, "btn.p", "保存", ui_kit::ButtonLevel::Primary,
                   0.0f, 0.0f, 80.0f, ui_kit::kButtonHeight, t);
    ui.end();

    const auto* bg = ui.find("btn.p.bg");
    DOCTEST_REQUIRE(bg != nullptr);
    // bg 走 .states() 的 normal 态 = wechat.primary
    DOCTEST_CHECK_EQ(toHexByte(bg->color.r), 0x07);
    DOCTEST_CHECK_EQ(toHexByte(bg->color.g), 0xC1);
    DOCTEST_CHECK_EQ(toHexByte(bg->color.b), 0x60);

    const auto* txt = ui.find("btn.p.text");
    DOCTEST_REQUIRE(txt != nullptr);
    // text 走 textColor = wechat.onPrimary（#FFFFFF）
    DOCTEST_CHECK_EQ(toHexByte(txt->textColor.r), 0xFF);
    DOCTEST_CHECK_EQ(toHexByte(txt->textColor.g), 0xFF);
    DOCTEST_CHECK_EQ(toHexByte(txt->textColor.b), 0xFF);
    return true;
})

// 场景2（防「次按钮与主按钮等权」）：Secondary bg=secondaryButtonBg(#F2F2F2)
// ≠ primary(#07C160) + text=textPrimary(#1A1A1A)。事故：次按钮用主色底/等宽
// 致操作权重不分（spec §4.1「与主按钮视觉等权」禁止项）。
REGISTER_TEST(ui_kit_button_secondary_not_primary,
              "ui_kit button: Secondary bg≠primary + text=textPrimary", {
    const auto& t = core::theme::current();
    eui::Ui ui;
    ui.begin("w02b_btn_secondary");
    ui_kit::button(ui, "btn.s", "取消", ui_kit::ButtonLevel::Secondary,
                   0.0f, 0.0f, 80.0f, ui_kit::kButtonHeight, t);
    ui.end();

    const auto* bg = ui.find("btn.s.bg");
    DOCTEST_REQUIRE(bg != nullptr);
    // bg = secondaryButtonBg #F2F2F2，不等于 primary #07C160
    DOCTEST_CHECK_EQ(toHexByte(bg->color.r), 0xF2);
    DOCTEST_CHECK_EQ(toHexByte(bg->color.g), 0xF2);
    DOCTEST_CHECK_EQ(toHexByte(bg->color.b), 0xF2);
    DOCTEST_CHECK(toHexByte(bg->color.r) != 0x07);  // 不等于 primary
    DOCTEST_CHECK(toHexByte(bg->color.g) != 0xC1);

    const auto* txt = ui.find("btn.s.text");
    DOCTEST_REQUIRE(txt != nullptr);
    // text = textPrimary #1A1A1A（深灰，不是白字——区分主次）
    DOCTEST_CHECK_EQ(toHexByte(txt->textColor.r), 0x1A);
    DOCTEST_CHECK_EQ(toHexByte(txt->textColor.g), 0x1A);
    DOCTEST_CHECK_EQ(toHexByte(txt->textColor.b), 0x1A);
    return true;
})

// 场景3（防「统一尺寸漂移」）：四级按钮 height 全=34、font 全=14、radius 全=6
//（同一档，不高低错落）。事故：某级按钮高度/font/圆角与其他级不一致致同视口
// 按钮高低错落（spec §4.1 D-12 统一尺寸「避免高低错落」）。
REGISTER_TEST(ui_kit_button_uniform_metrics,
              "ui_kit button: 四级 height=34 / font=14 / radius=6 统一", {
    const auto& t = core::theme::current();
    eui::Ui ui;
    ui.begin("w02b_btn_uniform");
    const float w = 80.0f;
    ui_kit::button(ui, "u.p", "P", ui_kit::ButtonLevel::Primary,   0,   0, w, ui_kit::kButtonHeight, t);
    ui_kit::button(ui, "u.s", "S", ui_kit::ButtonLevel::Secondary, 0,  40, w, ui_kit::kButtonHeight, t);
    ui_kit::button(ui, "u.t", "T", ui_kit::ButtonLevel::Text,      0,  80, w, ui_kit::kButtonHeight, t);
    ui_kit::button(ui, "u.d", "D", ui_kit::ButtonLevel::Danger,    0, 120, w, ui_kit::kButtonHeight, t);
    ui.end();

    const char* ids[] = {"u.p", "u.s", "u.t", "u.d"};
    for (const char* id : ids) {
        const std::string s = std::string(id);
        const auto* bg = ui.find(s + ".bg");
        DOCTEST_REQUIRE_MESSAGE(bg != nullptr, std::string("缺 ") + s + ".bg");
        // 高度固定 34（kButtonHeight）
        DOCTEST_CHECK_EQ(bg->height.value, ui_kit::kButtonHeight);
        DOCTEST_CHECK_EQ(bg->height.value, 34.0f);
        // 圆角 6（t.sizes.radiusWeChat，D-12 统一）
        DOCTEST_CHECK_EQ(bg->radius, t.sizes.radiusWeChat);
        DOCTEST_CHECK_EQ(bg->radius, 6.0f);

        const auto* txt = ui.find(s + ".text");
        DOCTEST_REQUIRE_MESSAGE(txt != nullptr, std::string("缺 ") + s + ".text");
        // 字号 14（kButtonFontSize）
        DOCTEST_CHECK_EQ(txt->fontSize, ui_kit::kButtonFontSize);
        DOCTEST_CHECK_EQ(txt->fontSize, 14.0f);
    }
    return true;
})

// 场景4（防「danger 误作普通色」）：Danger bg=warningRed(#FA5151)。事故：删除/
// 高危按钮用普通绿/灰底致用户误点（spec §2.2 红仅告警语义；§4.1 Danger 红填充）。
REGISTER_TEST(ui_kit_button_danger_red,
              "ui_kit button: Danger bg=wechat.warningRed(#FA5151)", {
    const auto& t = core::theme::current();
    eui::Ui ui;
    ui.begin("w02b_btn_danger");
    ui_kit::button(ui, "btn.d", "删除", ui_kit::ButtonLevel::Danger,
                   0.0f, 0.0f, 80.0f, ui_kit::kButtonHeight, t);
    ui.end();

    const auto* bg = ui.find("btn.d.bg");
    DOCTEST_REQUIRE(bg != nullptr);
    // bg = warningRed #FA5151，不是 primary 绿
    DOCTEST_CHECK_EQ(toHexByte(bg->color.r), 0xFA);
    DOCTEST_CHECK_EQ(toHexByte(bg->color.g), 0x51);
    DOCTEST_CHECK_EQ(toHexByte(bg->color.b), 0x51);
    DOCTEST_CHECK(toHexByte(bg->color.g) != 0xC1);  // 不等于 primary 绿的 G 通道

    const auto* txt = ui.find("btn.d.text");
    DOCTEST_REQUIRE(txt != nullptr);
    // Danger 文字 = onPrimary 白（红底白字）
    DOCTEST_CHECK_EQ(toHexByte(txt->textColor.r), 0xFF);
    DOCTEST_CHECK_EQ(toHexByte(txt->textColor.g), 0xFF);
    DOCTEST_CHECK_EQ(toHexByte(txt->textColor.b), 0xFF);
    return true;
})

// 场景5（FormRow 防「label 宽度不定」）：label width=110 + control 填满剩余。
// 事故：label 宽度随文字长短漂移致 control 起始位置不齐（spec §4.15 强化进规范）。
REGISTER_TEST(ui_kit_formrow_label_fixed_control_fill,
              "ui_kit formRow: label width=110 + control 填满剩余", {
    const auto& t = core::theme::current();
    constexpr float kTotalW = 300.0f;
    constexpr float kTotalH = 34.0f;

    float capControlX = -1.0f, capControlW = -1.0f, capControlH = -1.0f;
    eui::Ui ui;
    ui.begin("w02b_formrow");
    ui_kit::formRow(ui, "fr", "名称", 0.0f, 0.0f, kTotalW, kTotalH,
        [&](eui::Ui&, float cx, float /*cy*/, float cw, float ch) {
            capControlX = cx;
            capControlW = cw;
            capControlH = ch;
        }, t);
    ui.end();

    // label 节点存在 + 宽度固定 110（kFormRowLabelWidth）
    const auto* lbl = ui.find("fr.label");
    DOCTEST_REQUIRE(lbl != nullptr);
    DOCTEST_CHECK_EQ(lbl->width.value, ui_kit::kFormRowLabelWidth);
    DOCTEST_CHECK_EQ(lbl->width.value, 110.0f);
    // label 文字色 = wechat.textPrimary（#1A1A1A）
    DOCTEST_CHECK_EQ(toHexByte(lbl->textColor.r), 0x1A);

    // control 区：填满剩余宽 = total - label(110) - gap(8) = 182
    const float gap = t.sizes.spacing;  // 8px
    DOCTEST_CHECK_EQ(capControlX, ui_kit::kFormRowLabelWidth + gap);
    DOCTEST_CHECK_EQ(capControlX, 118.0f);
    DOCTEST_CHECK_EQ(capControlW, kTotalW - ui_kit::kFormRowLabelWidth - gap);
    DOCTEST_CHECK_EQ(capControlW, 182.0f);
    DOCTEST_CHECK_EQ(capControlH, kTotalH);
    return true;
})
