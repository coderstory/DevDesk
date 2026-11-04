#include "terminal_page.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

#include "core/theme.h"
#include "terminal_keyencoder.h"
#include "ui_kit/card.h"

namespace terminal {

using core::Color;

namespace {
constexpr float kToolbarH = 44.0f;
} // namespace

GridLayout TerminalPage::compose(
    eui::Ui& ui, const eui::Screen& screen, SessionController& controller,
    const std::function<void(const uint8_t*, int32_t)>& onKeyBytes,
    const std::function<void(const std::string&, bool paste)>& onTextInput,
    const std::function<void(const std::string&)>& onCopyText) {
    const auto& t = core::theme::current();
    GridLayout layout;
    layout.rows = 0;
    layout.cols = 0;

    if (!fullscreenInit_) {
        fullscreenInit_ = true;
        fullscreen_ = std::getenv("DEVDESK_TERMINAL_FULLSCREEN") != nullptr;
    }
    const float gridTop = fullscreen_ ? 0.0f : kToolbarH;
    const float bodyH = screen.height - gridTop;

    controller.poll();
    const platform::TerminalFrame& frame = controller.frame();
    const int sb = controller.scrollbackCount();
    const int totalLines = sb + frame.rows;
    // 单根 stack：绝对坐标层叠（顶层=页面坐标系，非自动 flow——bug:124 根因）
    ui.stack("term.root")
        .size(screen.width, screen.height)
        .content([&] {
            // 层 0：终端底（设计 §3.2：暗 #1A1816=background token；亮=surface 白卡）
            ui.rect("term.bg")
                .size(screen.width, screen.height)
                .color(t.isDark ? t.colors.background : t.colors.surface);

            // 层 1：网格体
            ui.column("term.body")
                .width(screen.width)
                .height(bodyH)
                .margin(0.0f, gridTop, 0.0f, 0.0f)
                .content([&] {
                    grid_.compose(
                        ui, frame, sb, scrollOffset_, screen.width, bodyH, t.isDark,
                        [&controller](int idx, std::string& out) {
                            return controller.scrollbackLine(idx, out);
                        },
                        layout);
                });
            pendingRows_ = layout.rows;
            pendingCols_ = layout.cols;

            // 层 2：交互层（拖选/滚轮/键盘/IME）——近透明可点击，浮于网格行上。
            // 坐标：Press/Release=物理（除 scale），Move=逻辑（input_types.h 契约）
            auto toLogical = [](const eui::PointerEvent& e) {
                const double s = e.scale > 0.0 ? static_cast<double>(e.scale) : 1.0;
                const bool physical = e.space != eui::PointerSpace::Logical;
                return std::pair<double, double>(physical ? e.x / s : e.x,
                                                 physical ? e.y / s : e.y);
            };
            const auto rowOf = [&](double logicalY) {
                return static_cast<int>((logicalY - gridTop) / (fontSize_ * 1.25f));
            };
            const auto colOf = [&](double logicalX) {
                return std::max(0, static_cast<int>(logicalX / (fontSize_ * 0.6f)));
            };
            ui.rect("term.input")
                .size(screen.width, bodyH)
                .margin(0.0f, gridTop, 0.0f, 0.0f)
                .color(Color{0, 0, 0, 0.01f})
                .onPress([&](const eui::PointerEvent& e, const eui::Rect&) {
                    auto [lx, ly] = toLogical(e);
                    selecting_ = true;
                    hasSelection_ = false;
                    selARow_ = rowOf(ly);
                    selACol_ = colOf(lx);
                    selHRow_ = selARow_;
                    selHCol_ = selACol_;
                })
                .onMove([&](const eui::PointerEvent& e, const eui::Rect&) {
                    if (!selecting_) return false;
                    selHRow_ = rowOf(e.y);
                    selHCol_ = colOf(e.x);
                    hasSelection_ = true;
                    return true;
                })
                .onRelease([&](const eui::PointerEvent& e, const eui::Rect&) {
                    if (!selecting_) return;
                    selecting_ = false;
                    auto [lx, ly] = toLogical(e);
                    selHRow_ = rowOf(ly);
                    selHCol_ = colOf(lx);
                    hasSelection_ = !(selARow_ == selHRow_ && selACol_ == selHCol_);
                })
                .onScroll([this, &controller](const core::ScrollEvent& e) {
                    scrollRemainder_ += static_cast<float>(e.y);
                    const float cellH = fontSize_ * 1.25f;
                    const int lines = static_cast<int>(scrollRemainder_ / cellH);
                    if (lines == 0) return;
                    scrollRemainder_ -= lines * cellH;
                    const int maxOffset = std::max(0, controller.scrollbackCount());
                    scrollOffset_ = std::clamp(scrollOffset_ - lines, 0, maxOffset);
                    // 滚动即清选区：选区行列锚在拖选时可视网格上，滚动后 startLine
                    // 漂移会使旧行列号指向错行（重锚复杂度不值；终端惯例=滚动放弃选择）
                    hasSelection_ = false;
                    selecting_ = false;
                })
                .onKeyEvent([&controller, &onKeyBytes, &onCopyText, &totalLines, &layout, this](
                          const eui::KeyEvent& e) {
                    if (!e.isDown()) return false;
                    // Ctrl+C 二象性（技术卡 v2 决策 4/§5）：win 惯例且选区非空 → 复制并吞
                    // 事件；无选区/mac 惯例 → keyencoder 语义（\x03 中断，encodeKey 同值）
                    if (e.modifiers.control && e.key == eui::InputKey::C) {
                        std::string bytes;
                        if (terminal::encodeCtrlC(platform::terminalKeyConventions(),
                                                  hasSelection_, e.modifiers.shift, bytes)) {
                            // P1-3②：拖选行列是可视行——滚动即清选区（onScroll），非滚动
                            // 态可视行=网格行；滚动态按 frameRow 映射，越界段裁掉（不复制，
                            // 落回中断语义）——selectionText 只服务当前可视网格
                            const int startLine =
                                std::max(0, totalLines - layout.rows - scrollOffset_);
                            const int sbNow = controller.scrollbackCount();
                            const int fR0 = selARow_ + startLine - sbNow;
                            const int fR1 = selHRow_ + startLine - sbNow;
                            std::string sel;
                            if (fR0 >= 0 && fR1 >= 0 && fR1 < layout.rows &&
                                controller.selectionText(fR0, selACol_, fR1, selHCol_, sel)) {
                                onCopyText(sel);
                                return true;
                            }
                            bytes.assign(1, '\x03'); // 裁掉/提取失败 → 落回中断
                        }
                        if (!bytes.empty()) {
                            onKeyBytes(reinterpret_cast<const uint8_t*>(bytes.data()),
                                       static_cast<int32_t>(bytes.size()));
                            return true;
                        }
                        return false;
                    }
                    std::string bytes;
                    if (terminal::encodeKey(e.key, e.modifiers, bytes)) {
                        onKeyBytes(reinterpret_cast<const uint8_t*>(bytes.data()),
                                   static_cast<int32_t>(bytes.size()));
                        return true;
                    }
                    return false;
                })
                .onTextInput([&onTextInput](const eui::TextInputEvent& e) {
                    if (!e.text.empty()) onTextInput(e.text, false);
                    if (!e.pasteText.empty()) onTextInput(e.pasteText, true);
                });

            // 层 3：选区高亮 rects（可视行段；primary α0.30 暗 / α0.18 亮，§3.3）。
            // 列→像素按 cell.width 折算：半角 1 格、宽字 2 格（CJK 精确列布局，不做近似）
            const auto cellX = [&frame](int row, int col, float cw) {
                float x = 0;
                if (row >= 0 && row < frame.rows &&
                    static_cast<size_t>(row) * frame.cols < frame.cells.size()) {
                    const platform::TerminalCell* r =
                        &frame.cells[static_cast<size_t>(row) * frame.cols];
                    for (int c = 0; c < col && c < frame.cols; c++) {
                        x += std::max(1, static_cast<int>(r[c].width)) * cw;
                    }
                } else {
                    x = static_cast<float>(col) * cw;
                }
                return x;
            };
            const auto cellSpan = [&frame](int row, int c0, int c1, float cw) {
                float w = 0;
                if (row >= 0 && row < frame.rows &&
                    static_cast<size_t>(row) * frame.cols < frame.cells.size()) {
                    const platform::TerminalCell* r =
                        &frame.cells[static_cast<size_t>(row) * frame.cols];
                    for (int c = c0; c < c1 && c < frame.cols; c++) {
                        w += std::max(1, static_cast<int>(r[c].width)) * cw;
                    }
                } else {
                    w = static_cast<float>(c1 - c0) * cw;
                }
                return w;
            };
            if (hasSelection_) {
                const float cellW = fontSize_ * 0.6f;
                const float cellH = fontSize_ * 1.25f;
                const int top = std::min(selARow_, selHRow_);
                const int bottom = std::max(selARow_, selHRow_);
                const float selAlpha = t.isDark ? 0.30f : 0.18f;
                for (int r = std::max(top, 0); r <= bottom && r < layout.rows; r++) {
                    const int c0 = (r == top) ? std::min(selACol_, selHCol_) : 0;
                    const int c1 =
                        (r == bottom) ? std::max(selACol_, selHCol_) : std::max(1, layout.cols);
                    if (c1 <= c0) continue;
                    const int frameRow = r + std::max(0, totalLines - layout.rows - scrollOffset_) - sb;
                    ui.rect("term.sel." + std::to_string(r))
                        .size(cellSpan(frameRow, c0, c1, cellW), cellH)
                        .margin(cellX(frameRow, c0, cellW),
                                gridTop + static_cast<float>(r) * cellH, 0.0f, 0.0f)
                        .color(Color{t.colors.primary.r, t.colors.primary.g, t.colors.primary.b,
                                     selAlpha});
                }
            }

            // 层 4：IME 锚定（POC-5 收尾）：光标 cell 可视坐标上报 imeRect
            if (!frame.cells.empty() && layout.rows > 0) {
                const float cellW = fontSize_ * 0.6f;
                const float cellH = fontSize_ * 1.25f;
                const int startLine = std::max(0, totalLines - layout.rows - scrollOffset_);
                const int visRow = (sb + frame.cursorRow) - startLine;
                if (visRow >= 0 && visRow < layout.rows) {
                    const int frameRow = visRow + startLine - sb;
                    const float ix = cellX(frameRow, frame.cursorCol, cellW);
                    const float iy = gridTop + static_cast<float>(visRow) * cellH;
                    ui.rect("term.ime.anchor")
                        .size(cellW, cellH)
                        .margin(ix, iy, 0.0f, 0.0f)
                        .color(Color{0, 0, 0, 0})
                        .imeRect(ix, iy, cellW, cellH);
                }
            }

            // 层 5：工具栏 44px（全屏态隐藏，F-05）
            if (!fullscreen_) {
                std::string badge = "待启动";
                Color badgeColor = t.colors.textSecondary;
                switch (controller.state()) {
                    case SessionState::Running: badge = "运行中"; badgeColor = t.colors.primary; break;
                    case SessionState::Exited:
                        badge = "已结束 (" + std::to_string(controller.exitCode()) + ")";
                        break;
                    case SessionState::Failed: badge = "启动失败"; badgeColor = t.colors.error; break;
                    case SessionState::Idle: break;
                }
                ui.text("term.title")
                    .size(56.0f, core::theme::scaled(20.0f))
                    .margin(t.sizes.spacingLarge, 12.0f, 0.0f, 0.0f)
                    .text("终端")
                    .fontSize(14.0f)
                    .color(t.colors.textPrimary);
                ui.text("term.badge")
                    .size(130.0f, core::theme::scaled(20.0f))
                    .margin(72.0f, 12.0f, 0.0f, 0.0f)
                    .text(badge)
                    .fontSize(12.0f)
                    .color(badgeColor);
                ui.text("term.new")
                    .size(70.0f, core::theme::scaled(20.0f))
                    .margin(screen.width - 220.0f, 12.0f, 0.0f, 0.0f)
                    .text("+ 新建")
                    .fontSize(12.0f)
                    .color(t.colors.textPrimary)
                    .onClick([&controller]() { controller.startNew(); });
                ui.text("term.fullscreen")
                    .size(28.0f, core::theme::scaled(20.0f))
                    .margin(screen.width - 44.0f, 12.0f, 0.0f, 0.0f)
                    .text("\uF065")  // fa-expand（字形核验项 §13）
                    .fontSize(12.0f)
                    .color(t.colors.textSecondary)
                    .onClick([this]() { toggleFullscreen(); });
            } else {
                // 悬浮退出钮：右上 28/28 半透（设计 §4.2 16/16 参数，近似实现）
                ui.rect("term.exit.fab")
                    .size(28.0f, 28.0f)
                    .margin(screen.width - 44.0f, 16.0f, 0.0f, 0.0f)
                    .color(Color{1.0f, 1.0f, 1.0f, 0.35f})
                    .onClick([this]() { toggleFullscreen(); });
                ui.text("term.exit.icon")
                    .size(28.0f, core::theme::scaled(20.0f))
                    .margin(screen.width - 44.0f, 20.0f, 0.0f, 0.0f)
                    .text("\uF06E")  // fa-compress
                    .fontSize(12.0f)
                    .color(t.colors.textPrimary);
            }

            // 层 6：覆盖卡（Exited/Failed）
            if (controller.state() == SessionState::Exited ||
                controller.state() == SessionState::Failed) {
                const std::string title =
                    controller.state() == SessionState::Exited
                        ? "会话已结束，退出码 " + std::to_string(controller.exitCode())
                        : "终端启动失败";
                const std::string detail = controller.state() == SessionState::Failed
                                               ? controller.errorMessage()
                                               : "输出已保留，可重新开始或新建会话";
                const float cx = (screen.width - 320.0f) / 2.0f;
                const float cy = (bodyH - 140.0f) / 2.0f + gridTop;
                ui.rect("term.overlay.scrim")
                    .size(screen.width, screen.height)
                    .color(Color{0.0f, 0.0f, 0.0f, 0.35f});
                ui_kit::cardPanel(ui, "term.overlay.card", cx, cy, 320.0f, 140.0f,
                                  t.sizes.radiusLarge, t);
                ui.column("term.overlay.card.body")
                    .width(320.0f)
                    .height(140.0f)
                    .padding(cx + t.sizes.spacingLarge, cy + t.sizes.spacingLarge, 0.0f, 0.0f)
                    .content([&] {
                        ui.text("term.overlay.title")
                            .size(300.0f, core::theme::scaled(20.0f))
                            .text(title)
                            .fontSize(15.0f)
                            .color(t.colors.textPrimary);
                        ui.text("term.overlay.detail")
                            .size(300.0f, core::theme::scaled(18.0f))
                            .text(detail)
                            .fontSize(12.0f)
                            .color(t.colors.textSecondary);
                        ui.text("term.overlay.retry")
                            .size(90.0f, core::theme::scaled(18.0f))
                            .text(controller.state() == SessionState::Failed ? "重试"
                                                                             : "重新开始")
                            .fontSize(13.0f)
                            .color(t.colors.primary)
                            .onClick([&controller]() { controller.startNew(); });
                    });
            }
        });

    // Cmd+C 兜底路径：module 级 onKeyEvent 捕 super+C → onCopyText（见 module_terminal.cpp）
    (void)onKeyBytes;
    (void)onTextInput;
    (void)onCopyText;
    return layout;
}

} // namespace terminal
