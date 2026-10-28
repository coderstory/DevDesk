// 自绘勾选框（module_release 内唯一勾选控件）。
//
// 为什么不用 EUI-NEO 现成的 checkbox 组件：
//   - 其 build() 里 hover / pressed 两个局部变量只赋值、从不读取，在本仓库
//     -Werror=unused-but-set-variable 下直接编译失败；
//   - EUI 侧 checkbox 没有禁用态与「跳过」视觉，单据号未填时的灰态 +
//     旁挂提示文案无法表达。EUI-NEO 是独立仓库，本模块不跨仓改上游，故自绘。
//
// 视觉契约：
//   - 未选：空心（surface 底 + 1.5px borderDefault 边）；
//   - 已选：实心主色 + 勾（步骤完成态用 success 绿色）；
//   - 跳过：灰底 + 横线，标签灰色并划线（优先于已选，二者不叠加）；
//   - 禁用：灰、不可点；hint 非空时在标签位置显示提示文案（单据号未填场景）。
//
// 布局契约：
//   - drawReleaseCheckbox 不写 position（跟随父容器流式排布，滚动列表用）；
//   - drawReleaseCheckboxAt 写 position（执行页前置卡等绝对定位场景用）。
//   两种形态共用同一外层节点 id，命中区节点固定为 <id>.hit（无头测试按此点击）。
#pragma once

#include "core/render/text.h"
#include "core/theme.h"

#include <eui/dsl.h>

#include <algorithm>
#include <functional>
#include <string>

namespace module::release {

struct ReleaseCheckboxOptions {
    bool checked = false;
    bool skipped = false;
    bool enabled = true;
    std::string label;
    std::string hint;       // 禁用时替代 label 显示（如「请先填写单据号」）
    float boxSize = 16.0f;
    float fontSize = 13.0f;
    float width = 0.0f;     // <= 0 = 按文本自适应
    float height = 32.0f;
    float inset = 8.0f;     // 行内左侧留白（对齐 EUI checkbox 的 horizontalInset）
    float gap = 8.0f;       // 盒子与标签间距
};

// 只画形状（矩形 + 勾/横线），不含标签与命中区；供自管排布的调用方（步骤状态点）。
// successAccent = true 时已选态用 success 绿（步骤完成），否则用主色。
inline void drawCheckboxGlyph(eui::Ui& ui, const std::string& id, float x, float y,
                              float boxSize, bool checked, bool skipped, bool enabled,
                              const core::theme::Tokens& t, bool successAccent = false) {
    const float box = std::max(10.0f, boxSize);
    const bool done = checked && !skipped;
    const eui::Color accent = successAccent ? t.colors.success : t.colors.primary;
    const bool filled = done && (enabled || successAccent);
    const eui::Color fill = skipped ? t.colors.hover
                            : done ? (filled ? accent : t.colors.borderDefault)
                                   : (enabled ? t.colors.surface : t.colors.hover);
    const eui::Color border = skipped ? t.colors.hover
                              : done ? (filled ? accent : t.colors.borderDefault)
                                     : t.colors.borderDefault;
    const eui::Color mark = skipped ? t.colors.textTertiary
                            : (filled ? t.colors.onPrimary : t.colors.textDisabled);

    ui.rect(id)
        .position(x, y)
        .size(box, box)
        .color(fill)
        .border(1.5f, border)
        .radius(4.0f)
        .build();
    if (!done && !skipped) {
        return;
    }
    ui.text(id + ".mark")
        .position(x, y)
        .size(box, box)
        .icon(skipped ? 0xF068u : 0xF00Cu)
        .fontSize(std::max(1.0f, box * (skipped ? 0.55f : 0.61f)))
        .lineHeight(box)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(mark)
        .build();
}

namespace checkbox_detail {

inline float totalWidth(const ReleaseCheckboxOptions& options, float box,
                        float inset, float gap, const std::string& display) {
    if (options.width > 0.0f) {
        return options.width;
    }
    const float measured =
        display.empty()
            ? 0.0f
            : core::TextPrimitive::measureTextWidth(display, {}, options.fontSize, 400);
    return inset * 2.0f + box + (display.empty() ? 0.0f : gap + measured);
}

// 外层 stack 内绘制：命中区 + 盒子 + 标签（+ 跳过划线 / 禁用提示）。
inline void drawContent(eui::Ui& ui, const std::string& id,
                        const ReleaseCheckboxOptions& options,
                        const std::function<void()>& onClick,
                        const core::theme::Tokens& t) {
    const float box = std::max(10.0f, std::min(options.boxSize, options.height));
    const float inset = std::max(0.0f, options.inset);
    const float gap = std::max(0.0f, options.gap);
    const std::string display =
        (!options.enabled && !options.hint.empty()) ? options.hint : options.label;
    const float measured = display.empty()
        ? 0.0f
        : core::TextPrimitive::measureTextWidth(display, {}, options.fontSize, 400);
    const float labelW =
        options.width > 0.0f
            ? std::max(0.0f, options.width - inset * 2.0f - box -
                                 (display.empty() ? 0.0f : gap))
            : measured;
    const float totalW = totalWidth(options, box, inset, gap, display);
    const float boxY = (options.height - box) * 0.5f;

    auto hit = ui.rect(id + ".hit")
                   .size(totalW, options.height)
                   .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f});
    if (options.enabled && onClick) {
        eui::Color rowHover = t.colors.textPrimary;
        rowHover.a = 0.05f;
        eui::Color rowPressed = t.colors.textPrimary;
        rowPressed.a = 0.09f;
        hit.states(eui::Color{0.0f, 0.0f, 0.0f, 0.0f}, rowHover, rowPressed);
        hit.onClick(onClick);
    }
    hit.build();

    drawCheckboxGlyph(ui, id + ".box", inset, boxY, box, options.checked,
                      options.skipped, options.enabled, t);

    if (display.empty()) {
        return;
    }
    eui::Color labelColor = options.skipped
        ? t.colors.textDisabled
        : (options.checked ? t.colors.textPrimary : t.colors.textSecondary);
    if (!options.enabled && !options.hint.empty()) {
        labelColor = t.colors.textTertiary;
    }
    const float labelX = inset + box + gap;
    ui.text(id + ".label")
        .position(labelX, 0.0f)
        .size(labelW, options.height)
        .text(display)
        .fontSize(options.fontSize)
        .lineHeight(options.height)
        .maxWidth(labelW)
        .wrap(false)
        .overflow(core::TextOverflow::Ellipsis)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(labelColor)
        .build();
    if (options.skipped) {
        ui.rect(id + ".strike")
            .position(labelX, options.height * 0.5f)
            .size(std::min(labelW, measured), 1.0f)
            .color(t.colors.textDisabled)
            .build();
    }
}

inline float stackWidth(const ReleaseCheckboxOptions& options) {
    const float box = std::max(10.0f, std::min(options.boxSize, options.height));
    const float inset = std::max(0.0f, options.inset);
    const float gap = std::max(0.0f, options.gap);
    const std::string display =
        (!options.enabled && !options.hint.empty()) ? options.hint : options.label;
    return totalWidth(options, box, inset, gap, display);
}

} // namespace checkbox_detail

// 流式排布：不写 position，由父容器（Column 等）依次落位（滚动列表 / 行内用）。
inline void drawReleaseCheckbox(eui::Ui& ui, const std::string& id,
                                const ReleaseCheckboxOptions& options,
                                const std::function<void()>& onClick,
                                const core::theme::Tokens& t) {
    ui.stack(id)
        .size(checkbox_detail::stackWidth(options), options.height)
        .content([&] { checkbox_detail::drawContent(ui, id, options, onClick, t); })
        .build();
}

// 绝对定位：执行页前置卡等父容器按坐标排布的场景用。
inline void drawReleaseCheckboxAt(eui::Ui& ui, const std::string& id, float x, float y,
                                  const ReleaseCheckboxOptions& options,
                                  const std::function<void()>& onClick,
                                  const core::theme::Tokens& t) {
    ui.stack(id)
        .position(x, y)
        .size(checkbox_detail::stackWidth(options), options.height)
        .content([&] { checkbox_detail::drawContent(ui, id, options, onClick, t); })
        .build();
}

} // namespace module::release