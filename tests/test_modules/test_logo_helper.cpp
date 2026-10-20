// tests/test_modules/test_logo_helper.cpp
// ui_kit::drawDevDeskLogo 契约测试（图标批 2026-10-20，C 方案终端窗 >_）。
// 每条场景注明防什么事故——场景缺口是真机三连爆的根源（交付口径纪律）。
//
// 三个消费档位共用同一 helper（侧栏 brand 28 / 标题栏 20 / splash 56），
// helper 契约漂移 = 三处 logo 一起变形，且无头单测只在这里能钉（渲染层走 probe）：
//   - 结构契约：子元素 id（.tile/.win/.gt1/.gt2/.us）与类型（Rect/Polygon）——
//     调用点与单测按 id 定位，id/类型一变三处断言齐红；
//   - 几何契约：归一化坐标 = assets/icon.ico 位图版的设计坐标（scripts/
//     build_icon_assets.py 同源）——矢量/位图两处漂移 = 任务栏图标与界面 logo
//     长得不一样（品牌事故）；
//   - 缩放线性：size 进所有子元素换算（防「size 只进了瓦、笔画仍按 240 画」
//     之类的档位失衡）；opacity 透传全部 5 子元素（防 splash 淡出时 logo 残留）。
#include "doctest.h"

#include "ui_kit/logo.h"

#include <eui/dsl.h>
#include <eui/types.h>

#include <cmath>
#include <string>

namespace {

bool floatEq(float a, float b) { return std::fabs(a - b) < 0.01f; }

// 设计契约的归一化比值（瓦边长 = 1）：与 scripts/build_icon_assets.py 的
// 256 画布设计坐标换算一致（画布坐标减瓦原点 8、除瓦边长 240）。
constexpr float kTileRadius = 52.0f / 240.0f;
constexpr float kWinX = 38.0f / 240.0f;
constexpr float kWinY = 54.0f / 240.0f;
constexpr float kWinW = 164.0f / 240.0f;
constexpr float kWinH = 132.0f / 240.0f;
constexpr float kWinRadius = 22.0f / 240.0f;
constexpr float kStroke = 13.0f / 240.0f;

} // namespace

// 场景1（结构 + 几何 + 颜色契约）：28px 档（侧栏 brand 同档）。防「三处调用点
// 换几何 logo 后子元素 id 漂移 / >_ 退化成普通 rect / 颜色不透传」——任一命中
// 调用点视觉崩坏且无头套件静默全绿。
REGISTER_TEST(logo_helper_structure_and_geometry,
              "ui_kit drawDevDeskLogo: 结构/几何/颜色契约（28px 档）", {
    const eui::Color kTile{0.07f, 0.76f, 0.25f, 1.0f};
    const eui::Color kWin{1.0f, 1.0f, 1.0f, 1.0f};
    eui::Ui ui;
    ui.begin("logo_contract_28");
    ui_kit::drawDevDeskLogo(ui, "logo", 10.0f, 20.0f, 28.0f, kTile, kWin);
    ui.end();

    const auto* root = ui.find("logo");
    DOCTEST_REQUIRE(root != nullptr);
    DOCTEST_CHECK(root->kind == core::dsl::ElementKind::Stack);
    DOCTEST_CHECK(floatEq(root->x, 10.0f));
    DOCTEST_CHECK(floatEq(root->y, 20.0f));
    DOCTEST_CHECK(floatEq(root->width.value, 28.0f));
    DOCTEST_CHECK(floatEq(root->height.value, 28.0f));

    const auto* tile = ui.find("logo.tile");
    DOCTEST_REQUIRE(tile != nullptr);
    DOCTEST_CHECK(tile->kind == core::dsl::ElementKind::Rect);
    DOCTEST_CHECK(floatEq(tile->width.value, 28.0f));
    DOCTEST_CHECK(floatEq(tile->radius, 28.0f * kTileRadius));
    DOCTEST_CHECK(floatEq(tile->color.r, kTile.r));
    DOCTEST_CHECK(floatEq(tile->color.g, kTile.g));
    DOCTEST_CHECK(floatEq(tile->color.b, kTile.b));

    const auto* win = ui.find("logo.win");
    DOCTEST_REQUIRE(win != nullptr);
    DOCTEST_CHECK(win->kind == core::dsl::ElementKind::Rect);
    DOCTEST_CHECK(floatEq(win->x, 28.0f * kWinX));
    DOCTEST_CHECK(floatEq(win->y, 28.0f * kWinY));
    DOCTEST_CHECK(floatEq(win->width.value, 28.0f * kWinW));
    DOCTEST_CHECK(floatEq(win->height.value, 28.0f * kWinH));
    DOCTEST_CHECK(floatEq(win->radius, 28.0f * kWinRadius));
    DOCTEST_CHECK(floatEq(win->color.g, kWin.g));

    // >_ 三笔画：Polygon 胶囊（18 点 = 两端各 8 段圆弧），笔画颜色 = 瓦色（设计同色）
    for (const char* part : {"gt1", "gt2", "us"}) {
        const auto* stroke = ui.find(std::string("logo.") + part);
        DOCTEST_REQUIRE_MESSAGE(stroke != nullptr, part);
        DOCTEST_CHECK(stroke->kind == core::dsl::ElementKind::Polygon);
        DOCTEST_CHECK_EQ(stroke->polygonPoints.size(), 18u);
        DOCTEST_CHECK(floatEq(stroke->color.r, kTile.r));
    }
    // > 折点（设计 (116,126) 画布坐标 → 瓦内 (108,94)-(80,118) 段的端点 (80,118)/240）
    // 与 _ 短横（(132..168,150) → (124..160,142)）各归一化钉住：几何漂移 = 矢量版
    // 与 assets/icon.ico 位图版长得不一样
    const auto* gt2 = ui.find("logo.gt2");
    DOCTEST_CHECK(floatEq(gt2->x + 0.5f * kStroke * 28.0f, 28.0f * 80.0f / 240.0f));
    DOCTEST_CHECK(floatEq(gt2->y + 0.5f * kStroke * 28.0f, 28.0f * 118.0f / 240.0f));
    const auto* us = ui.find("logo.us");
    DOCTEST_CHECK(floatEq(us->x + 0.5f * kStroke * 28.0f, 28.0f * 124.0f / 240.0f));
    DOCTEST_CHECK(floatEq(us->height.value, 28.0f * kStroke));
    return true;
})

// 场景2（缩放线性 + opacity 透传）：20px（标题栏档）与 56px（splash 档）下，所有
// 归一化比值与 28px 档一致——防「size 只进了部分子元素、另一档笔画/圆角失衡」；
// opacity=0.5 必须 5 个子元素全接住——防「splash 淡出时几何 logo 残留到最后一帧」。
REGISTER_TEST(logo_helper_scale_linearity_and_opacity,
              "ui_kit drawDevDeskLogo: 档位缩放线性 + opacity 全透传", {
    const eui::Color kTile{0.02f, 0.6f, 0.3f, 1.0f};
    const eui::Color kWin{1.0f, 1.0f, 1.0f, 1.0f};
    for (const float size : {20.0f, 56.0f}) {
        eui::Ui ui;
        ui.begin("logo_scale_" + std::to_string(static_cast<int>(size)));
        ui_kit::drawDevDeskLogo(ui, "logo", 0.0f, 0.0f, size, kTile, kWin, 0.5f);
        ui.end();

        const auto* tile = ui.find("logo.tile");
        const auto* win = ui.find("logo.win");
        const auto* us = ui.find("logo.us");
        DOCTEST_REQUIRE(tile != nullptr);
        DOCTEST_REQUIRE(win != nullptr);
        DOCTEST_REQUIRE(us != nullptr);
        DOCTEST_CHECK(floatEq(tile->radius / size, kTileRadius));
        DOCTEST_CHECK(floatEq(win->x / size, kWinX));
        DOCTEST_CHECK(floatEq(win->width.value / size, kWinW));
        DOCTEST_CHECK(floatEq(us->height.value / size, kStroke));
        // opacity 透传：瓦/窗/三笔画五元素全接（漏一个 = splash 淡出残影）
        for (const char* id : {"logo.tile", "logo.win", "logo.gt1",
                               "logo.gt2", "logo.us"}) {
            const auto* el = ui.find(id);
            DOCTEST_REQUIRE_MESSAGE(el != nullptr, id);
            DOCTEST_CHECK(floatEq(el->opacity, 0.5f));
        }
    }
    return true;
})
