// src/ui_kit/logo.cpp
// DevDesk 品牌几何 logo 实现：几何契约与 scripts/build_icon_assets.py / assets/icon.ico
// 同源（240 单位瓦面设计坐标）。画法分解：rect+radius 铺瓦与窗；>_ 笔画用 EUI-NEO
// lineChart 同款胶囊 polygon（18 点：两端各 8 段圆弧）——dsl 无 stroke 原语，此为
// 仓库已验证的圆帽斜线惯例画法（components/linechart.h drawCapsuleSegment）。
#include <ui_kit/logo.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace ui_kit {
namespace {

// 设计坐标：瓦边长 240 单位（icon 画布 256 减 2×8 边距）。瓦内要素（单位→瓦内偏移）：
//   白窗 [38, 54]-[202, 186]（164×132）rx=22；瓦 rx=52；笔画宽 13
//   > ：(108,94)→(80,118)→(108,142)  _：(124,142)→(160,142)
constexpr float kTileUnits = 240.0f;
constexpr float kTileRadiusUnits = 52.0f;
constexpr float kWindowXUnits = 38.0f;
constexpr float kWindowYUnits = 54.0f;
constexpr float kWindowWUnits = 164.0f;
constexpr float kWindowHUnits = 132.0f;
constexpr float kWindowRadiusUnits = 22.0f;
constexpr float kStrokeUnits = 13.0f;

// 圆弧采样（同 linechart.h appendArc：8 段，9 点）。origin 为 polygon 框原点，
// 采样点减 origin 落到 polygon 局部坐标。
void appendArcPoints(std::vector<eui::Vec2>& points, const eui::Vec2& center,
                     float radius, float startAngle, float endAngle,
                     const eui::Vec2& origin) {
    constexpr int kSegments = 8;
    for (int step = 0; step <= kSegments; ++step) {
        const float t = static_cast<float>(step) / static_cast<float>(kSegments);
        const float angle = startAngle + (endAngle - startAngle) * t;
        points.push_back({center.x + std::cos(angle) * radius - origin.x,
                          center.y + std::sin(angle) * radius - origin.y});
    }
}

// 胶囊（圆帽线段）采样：to 端 + from 端各半圆。退化（长度≈0）= 整圆。
std::vector<eui::Vec2> capsulePoints(const eui::Vec2& from, const eui::Vec2& to,
                                     float radius, const eui::Vec2& origin) {
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length <= 0.001f) {
        std::vector<eui::Vec2> points;
        points.reserve(18u);
        appendArcPoints(points, from, radius, 0.0f, 6.28318530718f, origin);
        return points;
    }
    const float angle = std::atan2(dy, dx);
    const float normalAngle = angle + 1.57079632679f;
    std::vector<eui::Vec2> points;
    points.reserve(18u);
    appendArcPoints(points, to, radius, normalAngle, normalAngle - 3.14159265359f, origin);
    appendArcPoints(points, from, radius, normalAngle - 3.14159265359f,
                    normalAngle - 6.28318530718f, origin);
    return points;
}

// 单笔画 = 胶囊 polygon。框 = 线段包围盒外扩半径（linechart 同款）。
void drawCapsuleStroke(eui::Ui& ui, const std::string& id, const eui::Vec2& from,
                       const eui::Vec2& to, float stroke, const eui::Color& color,
                       float opacity) {
    const float radius = stroke * 0.5f;
    const float originX = std::min(from.x, to.x) - radius;
    const float originY = std::min(from.y, to.y) - radius;
    ui.polygon(id)
        .x(originX)
        .y(originY)
        .size(std::fabs(to.x - from.x) + stroke, std::fabs(to.y - from.y) + stroke)
        .points(capsulePoints(from, to, radius, {originX, originY}))
        .color(color)
        .opacity(opacity)
        .build();
}

} // namespace

void drawDevDeskLogo(eui::Ui& ui, const std::string& id, float x, float y,
                     float size, const eui::Color& tileColor,
                     const eui::Color& windowColor, float opacity) {
    if (size <= 0.0f) {
        return;
    }
    const float u = size / kTileUnits;
    ui.stack(id)
        .position(x, y)
        .size(size, size)
        .content([&] {
            ui.rect(id + ".tile")
                .position(0.0f, 0.0f)
                .size(size, size)
                .radius(kTileRadiusUnits * u)
                .color(tileColor)
                .opacity(opacity)
                .build();
            ui.rect(id + ".win")
                .position(kWindowXUnits * u, kWindowYUnits * u)
                .size(kWindowWUnits * u, kWindowHUnits * u)
                .radius(kWindowRadiusUnits * u)
                .color(windowColor)
                .opacity(opacity)
                .build();
            const float stroke = kStrokeUnits * u;
            drawCapsuleStroke(ui, id + ".gt1",
                             {108.0f * u, 94.0f * u}, {80.0f * u, 118.0f * u},
                             stroke, tileColor, opacity);
            drawCapsuleStroke(ui, id + ".gt2",
                             {80.0f * u, 118.0f * u}, {108.0f * u, 142.0f * u},
                             stroke, tileColor, opacity);
            drawCapsuleStroke(ui, id + ".us",
                             {124.0f * u, 142.0f * u}, {160.0f * u, 142.0f * u},
                             stroke, tileColor, opacity);
        })
        .build();
}

} // namespace ui_kit
