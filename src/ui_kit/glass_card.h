// src/ui_kit/glass_card.h
// UAT6 E+F 批：共享参数化玻璃质感帮手——从 home_module drawGlassStatCard 内联代码
// 抽取为可复用绘制函数（勿复制粘贴）。参数取当前运行淡雅版：面 cardBg@0.62、
// 白边 @0.55、双渐变高光、1px 内嵌线、污渍晕。首页 5 卡传 D 加浓彩参（tint 有值）；
// 其余卡片传无染色淡雅参（tint=nullopt）。
//
// 调用约定：在卡片容器的 content lambda 内调用 drawGlassCardLayers，位于你自己的
// 内容元素之前（z 序：玻璃层在文字之下）。
#pragma once

#include <core/component_theme.h>
#include <core/theme.h>

#include <components/card.h>
#include <components/theme.h>

#include <eui/dsl.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <string>

namespace ui_kit {

struct GlassCardStyle {
    // 预置：首页 D 批加浓彩玻璃（tint 有值 + 高浓 alpha）
    static GlassCardStyle homeTinted(const eui::Color& tintColor) {
        GlassCardStyle g;
        g.tint = tintColor;
        g.tintAlpha = 0.40f;
        g.hoverVeilAlpha = 0.10f;
        g.pressedVeilAlpha = 0.12f;
        return g;
    }
    // 预置：E+F 淡雅玻璃（无染色，面 0.62 + 白边 0.55 + 双高光 + 污渍晕）
    static GlassCardStyle subtle() {
        GlassCardStyle g;
        g.tint = std::nullopt;  // 无染色
        g.tintAlpha = 0.0f;
        g.hoverVeilAlpha = 0.06f;
        g.pressedVeilAlpha = 0.12f;
        return g;
    }

    // 染色层（nullopt = 无染色纯玻璃）
    std::optional<eui::Color> tint;
    float tintAlpha = 0.40f;

    // 玻璃质感常量（与 home_module drawGlassStatCard 同源）
    float faceAlpha = 0.62f;         // 半透白卡面
    float borderAlpha = 0.55f;       // 白细边
    float highlightAlpha = 0.32f;    // 顶部高光峰值
    float highlightFrac = 0.58f;     // 高光纵向覆盖
    float highlightSideAlpha = 0.15f;  // 左上偏置横向高光
    float highlightSideFrac = 0.46f;   // 横向高光覆盖
    float topLineAlpha = 0.50f;      // 顶缘 inset 白线
    float stainWhiteCore = 0.06f;    // 白污渍芯
    float stainWhiteHalo = 0.03f;    // 白污渍壳
    float stainDarkCore = 0.019f;    // 暗污渍芯
    float stainDarkHalo = 0.010f;    // 暗污渍壳
    float hoverVeilAlpha = 0.10f;    // 整卡 hover 深色纱
    float pressedVeilAlpha = 0.12f;  // 整卡按压纱
};

// 在卡片容器的 content lambda 中绘制玻璃质感装饰层。
// originX/Y = 玻璃层起始坐标（通常 -inset 以覆盖组件 padding 区）
// cardW/H = 玻璃层覆盖尺寸（通常 = 卡宽/高）
// corner = 圆角半径（通常 = radiusWeChat = 6px）
// onClick 非空时追加整卡交互层（hover/press/click）。
// 调用前卡片容器应已建好背景色+边框（components::card 或自绘 rect）。
inline void drawGlassCardLayers(eui::Ui& ui, const std::string& baseId,
                                float originX, float originY,
                                float cardW, float cardH, float corner,
                                const core::theme::Tokens& t,
                                const GlassCardStyle& style,
                                std::function<void()> onClick = nullptr) {
    const auto& wc = t.colors.wechat;

    // ① 染色面：tint 低 alpha 直叠（soft-light 浅底近似，见 home_module 公式）
    if (style.tint.has_value() && style.tintAlpha > 0.0f) {
        ui.rect(baseId + ".glass.tint")
            .position(originX, originY)
            .size(cardW, cardH)
            .color(components::theme::withOpacity(
                *style.tint, style.tintAlpha))
            .radius(corner)
            .build();
    }

    // ② 白色顶部高光（纵向主带）
    ui.rect(baseId + ".glass.hl.top")
        .position(originX, originY)
        .size(cardW, cardH * style.highlightFrac)
        .gradient(components::theme::withOpacity(wc.cardBg,
                                                  style.highlightAlpha),
                  components::theme::withAlpha(wc.cardBg, 0.0f),
                  eui::GradientDirection::Vertical)
        .radius(corner)
        .build();

    // ③ 左上偏置横向高光
    ui.rect(baseId + ".glass.hl.side")
        .position(originX, originY)
        .size(cardW * style.highlightSideFrac, cardH)
        .gradient(components::theme::withOpacity(wc.cardBg,
                                                  style.highlightSideAlpha),
                  components::theme::withAlpha(wc.cardBg, 0.0f),
                  eui::GradientDirection::Horizontal)
        .radius(corner)
        .build();

    // ④ 顶缘 inset 高光线（1px 平直顶边）
    ui.rect(baseId + ".glass.topline")
        .position(originX + corner, originY)
        .size(cardW - corner * 2.0f, 1.0f)
        .color(components::theme::withOpacity(wc.cardBg,
                                              style.topLineAlpha))
        .build();

    // ⑤ 轻污渍（两白 + 一暗三层圆斑，模拟径向衰减）
    const auto stain = [&](int idx, float fx, float fy, float coreR,
                            float haloR, const eui::Color& col,
                            float coreA, float haloA) {
        const float cx = fx * cardW;
        const float cy = fy * cardH;
        const float rc = std::min({coreR, cx, cy, cardW - cx, cardH - cy});
        const float rh = std::min({haloR, cx, cy, cardW - cx, cardH - cy});
        const std::string sid = baseId + ".glass.stain." + std::to_string(idx);
        if (rh > 0.5f) {
            ui.rect(sid + ".halo")
                .position(originX + cx - rh, originY + cy - rh)
                .size(rh * 2.0f, rh * 2.0f)
                .color(components::theme::withOpacity(col, haloA))
                .radius(rh)
                .build();
        }
        if (rc > 0.5f) {
            ui.rect(sid + ".core")
                .position(originX + cx - rc, originY + cy - rc)
                .size(rc * 2.0f, rc * 2.0f)
                .color(components::theme::withOpacity(col, coreA))
                .radius(rc)
                .build();
        }
    };
    stain(0, 0.18f, 0.22f, cardH * 0.14f, cardH * 0.24f,
          wc.cardBg, style.stainWhiteCore, style.stainWhiteHalo);
    stain(1, 0.82f, 0.78f, cardH * 0.12f, cardH * 0.20f,
          wc.textPrimary, style.stainDarkCore, style.stainDarkHalo);
    stain(2, 0.64f, 0.12f, cardH * 0.10f, cardH * 0.16f,
          wc.cardBg, style.stainWhiteCore, style.stainWhiteHalo);

    // ⑥ 整卡交互层（点击 + hover 过渡）
    if (onClick) {
        ui.rect(baseId + ".glass.press")
            .position(originX, originY)
            .size(cardW, cardH)
            .states(components::theme::color(0.0f, 0.0f, 0.0f, 0.0f),
                    components::theme::withOpacity(wc.textPrimary,
                                                   style.hoverVeilAlpha),
                    components::theme::withOpacity(wc.textPrimary,
                                                   style.pressedVeilAlpha))
            .radius(corner)
            .transition(core::Transition::make(0.1f, core::Ease::OutQuad))
            .animate(core::AnimProperty::Color)
            .onClick(std::move(onClick))
            .build();
    }
}

} // namespace ui_kit