// src/ui_kit/home_anim.cpp — need506 定稿：点阵波纹层 + 打字机组绘制。
#include "home_anim.h"

#include "core/component_theme.h"

#include <components/theme.h>  // withOpacity

#include <algorithm>
#include <cmath>

namespace ui_kit::home_anim {

namespace {

constexpr float kTau = 6.2831853f;

}  // namespace

bool drawDotGrid(eui::Ui& ui, float x, float y, float w, float h,
                 float phase, float layerOpacity, const core::theme::Tokens& t) {
    if (layerOpacity <= 0.0f || h < 60.0f || w < 60.0f) return false;
    const float spacing = gridSpacingFor(w, h);
    const float x0 = x + 20.0f;            // 左右内缩 20
    const float y0 = y + 14.0f;            // 上 14
    const float usableW = w - 40.0f;
    const float usableH = h - 54.0f;       // 下 40
    const int cols = static_cast<int>(usableW / spacing) + 1;
    const int rows = static_cast<int>(usableH / spacing) + 1;
    if (cols * rows > kMaxDots) return false;  // 降档后仍超限 → 不画（防御）
    const auto& primary = t.colors.primary;
    const auto& deep = t.colors.primaryText;   // 深档绿 #157E4E 槽
    int count = 0;
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const float scale = dotScale(phase, r, c);
            float local = phase / kPeriodSec +
                          static_cast<float>(r + c) * kDiagStepSec / kPeriodSec;
            local -= std::floor(local);
            const float bell = waveBell(local);
            const float alpha = (kRestAlpha + (kCrestAlpha - kRestAlpha) * bell) *
                                layerOpacity;
            const float radius = kDotDiameter * 0.5f * scale;
            const float cx = x0 + c * spacing + (usableW - (cols - 1) * spacing) * 0.5f;
            const float cy = y0 + r * spacing + (usableH - (rows - 1) * spacing) * 0.5f;
            const bool deepDot = (r * 7 + c * 13) % 17 == 0;
            ui.rect("home.anim.dot." + std::to_string(r) + "." + std::to_string(c))
                .position(cx - radius, cy - radius)
                .size(radius * 2.0f, radius * 2.0f)
                .color(deepDot ? deep : primary)
                .radius(radius)
                .opacity(alpha)
                .build();
            ++count;
        }
    }
    return count > 0;
}

// ── 打字机状态机 ──
constexpr double kTypeMs = 82.0;
constexpr double kHoldMs = 2800.0;  // 用户令 2026-11-24：≈3s 一换
constexpr double kEraseMs = 26.0;
constexpr double kGapMs = 320.0;

void Typer::tick(double nowSec) {
    if (items.empty() || !rotating()) return;  // 空/单条：静态直显
    const double dtMs = (nowSec - lastTick) * 1000.0;  // tick 入参秒、节奏参数毫秒
    if (dtMs <= 0.0) return;
    const std::string cur = items[static_cast<size_t>(idx)];
    const int len = static_cast<int>(cur.size());  // UTF-8 字节步进 = 码点近似（中英均 3/1 字节，节奏一致）
    accMs += dtMs;  // 帧间残量累积：20fps 帧距 50ms < 82ms 步长 → 跨帧攒够才走一步
    while (accMs > 0.0) {
        const double stepMs = phase == Phase::Typing ? kTypeMs
                            : phase == Phase::Hold ? kHoldMs
                            : phase == Phase::Erasing ? kEraseMs : kGapMs;
        if (accMs < stepMs) break;  // 残量不足一步：留着下帧接着攒
        accMs -= stepMs;
        if (phase == Phase::Typing) {
            ++shown;
            if (shown >= len) {
                shown = len;
                phase = Phase::Hold;
            }
        } else if (phase == Phase::Hold) {
            phase = Phase::Erasing;
        } else if (phase == Phase::Erasing) {
            --shown;
            if (shown <= 0) {
                shown = 0;
                phase = Phase::Gap;
            }
        } else {  // Gap
            idx = (idx + 1) % static_cast<int>(items.size());
            phase = Phase::Typing;
        }
    }
    lastTick = nowSec;
}

std::string Typer::visibleText() const {
    if (items.empty()) return "最近打开的笔记会出现在这里";
    if (!rotating()) return items[0];
    const std::string& cur = items[static_cast<size_t>(idx)];
    // UTF-8 边界回退（不切半个码点）
    int cut = shown;
    size_t pos = 0;
    while (pos < cur.size() && cut > 0) {
        const unsigned char ch = static_cast<unsigned char>(cur[pos]);
        pos += ch < 0x80 ? 1 : ch < 0xE0 ? 2 : ch < 0xF0 ? 3 : 4;
        --cut;
    }
    return cur.substr(0, pos);
}

std::string Typer::staticFull() const {
    return items.empty() ? "最近打开的笔记会出现在这里" : items[0];
}


// ── 打字机组绘制（§3 全参数；几何按留白区中心 cx,cy 锚定）──
void drawTyper(eui::Ui& ui, float cx, float cy, float maxW, const Typer& typer,
               float phase, const std::string& subtitle, bool reduced,
               const core::theme::Tokens& t) {
    using components::theme::withOpacity;
    const float plateW = std::min(maxW * 0.7f + 68.0f, 560.0f);
    const float plateH = 76.0f;
    const float px = cx - plateW * 0.5f;
    const float py = cy - plateH * 0.5f - 8.0f;
    // 垫板：白 rgba(.74)（无 blur → 提高不透明度 .9 降级）圆角 999 阴影描边
    ui.rect("home.anim.typer.plate")
        .position(px, py)
        .size(plateW, plateH)
        .color(withOpacity(eui::Color("#FFFFFF"), reduced ? 0.9f : 0.74f))
        .radius(plateH * 0.5f)
        .border(1.0f, withOpacity(eui::Color("#FFFFFF"), 0.95f))
        .shadow(components::theme::shadow(core::theme::componentTokensWeChat(t), 3.0f, 1.0f, 0.12f, 0.02f))
        .build();
    // 上标签（垫板内水平居中，风格与主行统一）
    const float labelW =
        core::TextPrimitive::measureTextWidth("RECENT · 最近打开", {}, 15.0f, 600.0f);
    ui.text("home.anim.typer.label")
        .position(px + (plateW - labelW) * 0.5f, py - 26.0f)
        .size(labelW + 8.0f, 18.0f)
        .text("RECENT · 最近打开")
        .fontSize(15.0f)
        .fontWeight(600)
        .color(eui::Color("#6B7280"))
        .build();
    // 主行（28px/600/#157E4E）+ 光标（1100ms 闪烁 steps(2)：相位前半亮）
    const std::string text = reduced ? typer.staticFull() : typer.visibleText();
    const bool cursorOn = reduced ||
        std::fmod(phase, 1.1f) < 0.55f;
    const float rowY = py + 16.0f;
    const float rowH = 40.0f;
    // 主行居中（UAT 第 ⑥ 件）：锚 x = 垫板中心 − 已打前缀实测宽/2，随打字微移
    const float prefixW =
        core::TextPrimitive::measureTextWidth(text, {}, 28.0f, 600.0f);
    const float anchorX =
        px + (plateW - std::min(prefixW, plateW - 68.0f)) * 0.5f;
    ui.text("home.anim.typer.row")
        .position(anchorX, rowY)
        .size(std::min(prefixW, plateW - 68.0f) + 8.0f, rowH)
        .text(text)
        .fontSize(28.0f)
        .fontWeight(600)
        .lineHeight(rowH)
        .color(t.colors.primaryText)
        .build();
    if (cursorOn) {
        // 光标贴已打前缀末尾（居中锚 + 前缀宽；空前缀=锚点）
        ui.rect("home.anim.typer.cursor")
            .position(anchorX + std::min(prefixW, plateW - 68.0f), rowY + 6.0f)
            .size(3.0f, 28.0f * 1.15f)
            .color(t.colors.primary)
            .radius(2.0f)
            .build();
    }
    // 副语 + 轮换指示条（4 条 24×4，静 #DDEBE3 / 活 #36D399）
    if (!subtitle.empty()) {
        const float subW =
            core::TextPrimitive::measureTextWidth(subtitle, {}, 16.0f, 400.0f);
        ui.text("home.anim.typer.sub")
            .position(px + (plateW - subW) * 0.5f, py + plateH + 10.0f)
            .size(subW + 8.0f, 18.0f)
            .text(subtitle)
            .fontSize(16.0f)
            .color(eui::Color("#6B7280"))
            .build();
    }
    if (typer.rotating()) {
        const float barY = py + plateH + (subtitle.empty() ? 10.0f : 34.0f);
        const int n = static_cast<int>(typer.items.size());
        const float totalW = n * 24.0f + (n - 1) * 10.0f;
        const float bx = cx - totalW * 0.5f;
        for (int i = 0; i < n; ++i) {
            const bool active = i == typer.idx;
            ui.rect("home.anim.typer.bar." + std::to_string(i))
                .position(bx + i * 34.0f, barY)
                .size(24.0f, 4.0f)
                .color(active ? t.colors.primary : eui::Color("#DDEBE3"))
                .radius(2.0f)
                .build();
        }
    }
    (void)kTau;
}

}  // namespace ui_kit::home_anim
