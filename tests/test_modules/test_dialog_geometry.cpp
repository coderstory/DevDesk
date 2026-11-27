// tests/test_modules/test_dialog_geometry.cpp
// B-0 护栏（批次 B 弹窗几何四步的前置）：把弹窗壳三段几何钉成可测契约，
// 否则后续改壳（常量字号化 / compact 转默认 / 最小高下调）无自证。
//
// 三道轴（每条用例至少覆盖一道）：
//   1. 面积计算：内容区 = {24, 69, w−48, h−158}；有 contentHeightProvider 时
//      请求高 = 69 + provider + 89。
//   2. 字号档：100% 与 130% 两档下，头/底区高度随字号缩放，内容区相应收缩。
//      ★ 这是本轮修复的根因面：此前 shell 用未缩放常量、dialog_form 的
//      lineHeight 也未缩放 → 130% 档字号涨而盒不涨 → 文字被裁。
//   3. 按钮布局：右锚（主钮最右、副钮在其左）、最小宽 96、间距 8、左槽第三动作。
//
// 断言口径：只断言**相对关系与缩放比例**，不钉 69/73 的字面量——那些值会随
// 设计迭代变（步骤 2 就要把最小高 240 改 200），钉死会造成假红。唯一钉死的
// 是「120% 档下几何必须严格大于 100% 档」这条缩放不变量。
#include "doctest.h"

#include "core/component_theme.h"
#include "core/theme.h"
#include "ui_kit/dialog_form.h"
#include "ui_kit/dialog_shell.h"
#include "ui_kit/metrics.h"

#include <eui/dsl.h>
#include <eui/types.h>

#include <cmath>
#include <functional>
#include <string>

namespace {

eui::Screen makeScreen() {
    eui::Screen s;
    s.width = 1200.0f;
    s.height = 900.0f;
    return s;
}

// 跑一帧壳绘制，返回 content 回调拿到的 area（不调 → 视为未触发）
struct CapturedArea {
    bool called = false;
    ui_kit::DialogContentArea area{};
};

CapturedArea drawShellOnce(ui_kit::DialogShellSpec spec, float fontScale,
                           eui::Ui& ui) {
    core::theme::setFontScale(fontScale);
    const auto screen = makeScreen();
    const auto& t = core::theme::current();
    const auto themeTokens = core::theme::componentTokensWeChat(t);
    CapturedArea cap;
    ui.begin("dialog_geom");
    ui_kit::drawDialogShell(ui, screen, spec, t, themeTokens,
                            [&cap](const ui_kit::DialogContentArea& a) {
                                cap.called = true;
                                cap.area = a;
                            });
    ui.end();
    ui.layout(screen.width, screen.height);
    return cap;
}

}  // namespace

// 防：内容区几何写错（底区呼吸隙被吃掉/头区算进内容区）——届时时区顶到细线。
// 同时锁面积计算不变量：content.h = h − headerH − footerH − bottomGap。
REGISTER_TEST(dialog_geometry_content_area_inset,
              "弹窗几何：内容区自 (24, headerH) 起、宽 = w−48、底留呼吸隙", {
    ui_kit::DialogShellSpec spec;
    spec.id = "geom.area";
    spec.width = ui_kit::DialogWidth::Standard;
    spec.height = 520.0f;
    spec.open = true;
    spec.title = "标题";
    spec.cancel = {"取消", {}};
    spec.confirm = {"确定", {}};

    eui::Ui ui;
    const auto cap = drawShellOnce(spec, 1.0f, ui);
    DOCTEST_REQUIRE(cap.called);

    // 水平：x=24，宽 = 弹窗宽 − 48（DialogWidth::Standard = 460）
    DOCTEST_CHECK_EQ(cap.area.x, ui_kit::kDialogInsetX);
    DOCTEST_CHECK_EQ(cap.area.w, 460.0f - ui_kit::kDialogBodyInsetX);
    // 垂直：内容区顶 = 头区高
    DOCTEST_CHECK_EQ(cap.area.y, ui_kit::dialogHeaderH());
    // 三段几何自洽：area.y + area.h + footerH + gap == h
    DOCTEST_CHECK_EQ(cap.area.y + cap.area.h + ui_kit::dialogFooterH() +
                         ui_kit::dialogContentBottomGap(),
                     520.0f);
    return true;
});

// ★ 本轮根因守卫：130% 字号档下，三段几何必须同比放大。
// 修复前 shell 用未缩放常量 → 头/底区恒 69/73，字号涨了盒不涨，内容被裁。
REGISTER_TEST(dialog_geometry_scales_with_font_scale,
              "弹窗几何：130% 档头/底区同比放大（此前常量不缩放 → 文字被裁）", {
    ui_kit::DialogShellSpec spec;
    spec.id = "geom.scale";
    spec.width = ui_kit::DialogWidth::Standard;
    spec.height = 800.0f;  // 足够高，避免视口 clamp 干扰
    spec.open = true;
    spec.title = "标题";
    spec.cancel = {"取消", {}};
    spec.confirm = {"确定", {}};

    eui::Ui ui1;
    const auto base = drawShellOnce(spec, 1.0f, ui1);
    DOCTEST_REQUIRE(base.called);

    eui::Ui ui2;
    const auto big = drawShellOnce(spec, 1.3f, ui2);
    DOCTEST_REQUIRE(big.called);

    // 头区与底区（= 总高 − 头区 − 内容区 − 呼吸隙）都随档位放大
    DOCTEST_CHECK(big.area.y > base.area.y + 1.0f);
    const float baseFooter =
        spec.height - base.area.y - base.area.h;
    const float bigFooter = spec.height - big.area.y - big.area.h;
    DOCTEST_CHECK(bigFooter > baseFooter + 1.0f);
    // 内容区因上下两块变大而收缩（同请求高下）
    DOCTEST_CHECK(big.area.h < base.area.h - 1.0f);
    return true;
});

// 防：contentHeightProvider 语义被算错（69 + provider + 89 少算呼吸隙 → 内容被压）。
REGISTER_TEST(dialog_geometry_content_height_provider,
              "弹窗几何：有 contentHeightProvider 时请求高 = 头 + 内容 + 底 + 隙", {
    ui_kit::DialogShellSpec spec;
    spec.id = "geom.provider";
    spec.width = ui_kit::DialogWidth::Standard;
    spec.open = true;
    spec.title = "标题";
    spec.cancel = {"取消", {}};
    spec.confirm = {"确定", {}};
    const float naturalContent = 300.0f;
    spec.contentHeightProvider = [naturalContent](float) { return naturalContent; };

    eui::Ui ui;
    const auto cap = drawShellOnce(spec, 1.0f, ui);
    DOCTEST_REQUIRE(cap.called);

    // 内容区高应恰为自然内容高（未被压缩：请求高正好容得下）
    DOCTEST_CHECK_EQ(cap.area.h, naturalContent);
    return true;
});

// 防：最小高下限失效（确认类弹窗高度塌到内容之下）或 compact 档的另一条路径失效。
// need531 后：下限统一 200，compact 字段已无作用——本用例锁两件事：
// ①极矮请求被抬到下限（内容区高 = 下限 − 头 − 底 − 隙 > 0）；
// ②compact=true 与 false 结果必须一致（证明它真的不再分流）。
// 不钉具体像素值（下限会随设计迭代变），只钉自洽关系与「两档同值」。
REGISTER_TEST(dialog_geometry_min_height_and_compact,
              "弹窗几何：极矮请求被抬到下限，compact 开关已无分流作用", {
    // 极矮请求 → 被抬到下限
    ui_kit::DialogShellSpec tiny;
    tiny.id = "geom.min";
    tiny.width = ui_kit::DialogWidth::Standard;
    tiny.height = 10.0f;
    tiny.open = true;
    tiny.title = "标题";
    tiny.cancel = {"取消", {}};
    tiny.confirm = {"确定", {}};

    eui::Ui ui1;
    const auto minCap = drawShellOnce(tiny, 1.0f, ui1);
    DOCTEST_REQUIRE(minCap.called);
    // 内容区高 = 下限 − 头 − 底 − 隙；三块都由同一套函数给出，故可直接对比
    const float expectH = ui_kit::dialogMinH() - ui_kit::dialogHeaderH() -
                          ui_kit::dialogFooterH() - ui_kit::dialogContentBottomGap();
    DOCTEST_CHECK_MESSAGE(std::fabs(minCap.area.h - expectH) < 0.5f,
                          std::string("实测 ") + std::to_string(minCap.area.h) +
                              " 期望 " + std::to_string(expectH));
    DOCTEST_CHECK(minCap.area.h > 0.0f);

    // compact 已无作用：同请求下两档结果必须逐值相同
    ui_kit::DialogShellSpec compactSpec = tiny;
    compactSpec.id = "geom.compact";
    compactSpec.compact = true;
    eui::Ui ui2;
    const auto compactCap = drawShellOnce(compactSpec, 1.0f, ui2);
    DOCTEST_REQUIRE(compactCap.called);
    DOCTEST_CHECK(std::fabs(compactCap.area.h - minCap.area.h) < 0.5f);
    return true;
});

// 防：按钮右锚算错（主钮压到边上/副钮与主钮错位）——规范 §3.3 右缘内缩 24、间距 8。
// ⚠ 布局后的 frame 是**屏幕绝对坐标**（弹窗居中偏移已计入），断言只用同帧实测的
// 相对关系，不掺面板 x 的推导值（否则断言的是弹窗居中算法而非按钮布局）。
REGISTER_TEST(dialog_geometry_buttons_right_anchored,
              "弹窗几何：主钮在副钮右侧、间距 8、最小宽 96", {
    ui_kit::DialogShellSpec spec;
    spec.id = "geom.btn";
    spec.width = ui_kit::DialogWidth::Standard;
    spec.height = 400.0f;
    spec.open = true;
    spec.title = "标题";
    spec.cancel = {"取消", {}};
    spec.confirm = {"确定", {}};

    eui::Ui ui;
    const auto cap = drawShellOnce(spec, 1.0f, ui);
    DOCTEST_REQUIRE(cap.called);

    auto* confirm = ui.find("geom.btn.confirm");
    auto* cancel = ui.find("geom.btn.cancel");
    DOCTEST_REQUIRE(confirm != nullptr);
    DOCTEST_REQUIRE(cancel != nullptr);

    const auto& cb = confirm->frame;
    const auto& xb = cancel->frame;
    // 主钮在副钮右侧；两者间距恰为 8
    DOCTEST_CHECK(xb.x < cb.x);
    DOCTEST_CHECK_MESSAGE(
        std::fabs((cb.x - (xb.x + xb.width)) - ui_kit::dialogButtonGap()) < 0.5f,
        std::string("按钮间距实测 = ") +
            std::to_string(cb.x - (xb.x + xb.width)));
    // 最小宽 96（"取消"/"确定"两字文本宽 + 48 < 96，取下限）
    DOCTEST_CHECK(cb.width >= ui_kit::dialogButtonMinW() - 0.01f);
    DOCTEST_CHECK(xb.width >= ui_kit::dialogButtonMinW() - 0.01f);
    return true;
});

// 防：左槽第三动作（删除类）丢失或错位——规范要求左缘锚定、与右锚组同基线。
REGISTER_TEST(dialog_geometry_third_action_left_anchored,
              "弹窗几何：第三动作在按钮行最左、与右锚按钮同 y 基线", {
    ui_kit::DialogShellSpec spec;
    spec.id = "geom.third";
    spec.width = ui_kit::DialogWidth::Standard;
    spec.height = 400.0f;
    spec.open = true;
    spec.title = "标题";
    spec.cancel = {"取消", {}};
    spec.confirm = {"确定", {}};
    spec.thirdAction = {"删除", {}};
    spec.dangerThird = true;

    eui::Ui ui;
    const auto cap = drawShellOnce(spec, 1.0f, ui);
    DOCTEST_REQUIRE(cap.called);

    auto* third = ui.find("geom.third.third");
    auto* confirm = ui.find("geom.third.confirm");
    auto* cancel = ui.find("geom.third.cancel");
    DOCTEST_REQUIRE(third != nullptr);
    DOCTEST_REQUIRE(confirm != nullptr);
    DOCTEST_REQUIRE(cancel != nullptr);
    // 第三动作在按钮行最左（先于副钮）
    DOCTEST_CHECK(third->frame.x < cancel->frame.x);
    // 与右锚组同 y 基线
    DOCTEST_CHECK(std::fabs(third->frame.y - confirm->frame.y) < 0.5f);
    return true;
});

// ★ need531 验收：内容驱动的确认类弹窗不再留底盘空白。
// 语义：请求高 = 头 + 内容 + 底 + 隙；内容区高应恰等于声明的自然内容高，
// 而不是被抬到 240 下限后留下 30~40px 空白。
REGISTER_TEST(dialog_geometry_auto_height_no_blank,
              "弹窗几何：内容驱动弹窗高度贴合内容，无底盘空白", {
    ui_kit::DialogShellSpec spec;
    spec.id = "geom.auto";
    spec.width = ui_kit::DialogWidth::Standard;
    spec.open = true;
    spec.title = "确认删除？";
    spec.cancel = {"取消", {}};
    spec.confirm = {"删除", {}};
    spec.dangerConfirm = true;
    const float natural = 60.0f;
    spec.contentHeightProvider = ui_kit::dialogAutoHeight(natural);

    eui::Ui ui;
    const auto cap = drawShellOnce(spec, 1.0f, ui);
    DOCTEST_REQUIRE(cap.called);

    // 内容区 = 自然高（未被下限抬升挤压，也无多余留白）
    DOCTEST_CHECK_MESSAGE(std::fabs(cap.area.h - natural) < 0.5f,
                          std::string("内容区高 ") + std::to_string(cap.area.h));
    // 总请求高 = 头 + 内容 + 底 + 隙。
    // 壳高由底区细节反推（节点 geom.auto 命中的是根容器，不是面板）：
    // 内容区底 = area.y + area.h，再 + 隙 + 底区 = 面板高。
    const float expectTotal = ui_kit::dialogHeaderH() + natural +
                              ui_kit::dialogFooterH() +
                              ui_kit::dialogContentBottomGap();
    const float actualTotal = cap.area.y + cap.area.h +
                              ui_kit::dialogContentBottomGap() +
                              ui_kit::dialogFooterH();
    DOCTEST_CHECK_MESSAGE(std::fabs(actualTotal - expectTotal) < 0.5f,
                          std::string("壳高 ") + std::to_string(actualTotal) +
                              " 期望 " + std::to_string(expectTotal));
    // 关键：总高应显著小于旧下限 240（证明空白确实消掉了）
    DOCTEST_CHECK(expectTotal < 240.0f);
    return true;
});
