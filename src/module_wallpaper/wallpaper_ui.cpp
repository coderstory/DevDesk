// src/module_wallpaper/wallpaper_ui.cpp — 壁纸页「播放 / 片库」两页 UI。
//
// 节点 id 契约（probe / 单测按 id 定位，不得改）：
//   wallpaper.page                    页面根（column，左右 padding 32）
//   wallpaper.title                   页头标题「壁纸」
//   wallpaper.source                  来源分段（页头右侧，段 .video / .image）
//   wallpaper.tabs                    播放/片库 Tab 容器（.0 / .1 为块面）
//   wallpaper.scroll                  内容滚动视口（可滚动时才出现 .scroll 滚动条）
//   播放页：
//     wallpaper.card.status           状态卡（空态时整卡不建）
//       .dot / .title / .subtitle / .reasons（含 .text）/ .countdown（含 .label）
//       / .progress（含 .fill）/ .pause / .next
//     wallpaper.card.empty            空态卡（.title / .body / .support / .primary / .secondary）
//     wallpaper.card.mode / .interval 模式 + 间隔（一行两卡）
//       wallpaper.mode.0/1/2；wallpaper.interval.dropdown（禁用时多 .disabled 遮罩）
//     wallpaper.card.speed / .audio   速度 + 音频（共用折叠组）
//       wallpaper.speed.title / .slider / .value；wallpaper.audio.title / .slider /
//       .value / .mute（.mute.track 为开关轨道）
//     wallpaper.card.holds            让路规则（独立折叠组）
//       wallpaper.holds.title / .reason.0..3 / .battery（.battery.track）
//   片库页：
//     wallpaper.card.folder           文件夹卡（.path / .select / .rescan）
//       wallpaper.folder.stat.videos；图片一格 .stat.total（参与/低于两格不建：
//       像素档过滤未接入，imageFiltered 恒 0，画出来即宣称已过滤）
//       wallpaper.folder.scanning（扫描中）
//     wallpaper.card.formats          视频格式卡
//     wallpaper.card.resolution / .fit 图片分辨率 + 适配（一行两卡）
//
// 几何（100% 档；字号档走 roundFs，卡片高度同步缩放）：
//   页面左右 32、底部 32、页头 40、Tab 40、区块间距 16；卡内 20；卡圆角 12；
//   页标题 24/700、卡标题 15/600、正文 14、说明 12；按钮 40 高（ui_kit 口径）。
//   折叠态：速度+音频 56；展开 150。让路 56；展开 192。首屏（1000×640 页区
//   792×600）任何来源/页签都不出现纵向滚动条——承诺只限默认字号档：130% 档
//   内容高 ≈521 > 456 必滚动，由单测钉成显式口径（wallpaper_ui_fontscale_130）。
#include "wallpaper_ui.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>
#include <vector>

#include <components/dropdown.h>
#include <components/scrollview.h>
#include <core/component_theme.h>
#include <core/render/text.h>
#include <core/theme.h>
#include <eui/dsl.h>
#include <ui_kit/button.h>
#include <ui_kit/card.h>
#include <ui_kit/metrics.h>
#include <ui_kit/slider.h>
#include <ui_kit/wswitch.h>
#include <ui_kit/wtab.h>

#include "module_wallpaper/core/presentation.h"
#include "module_wallpaper/core/rotation.h"
#include "wallpaper_service.h"

namespace module::wallpaper {

namespace {

using ::core::theme::Tokens;

constexpr float kPagePadX = 32.0f;
constexpr float kPagePadBottom = 32.0f;
constexpr float kCardPad = 20.0f;
constexpr float kCardGap = 16.0f;  // = t.sizes.spacingLarge（100% 档 16）

// 卡片高度（100% 档基准，运行期 roundFs 派生，字号档与内容同步缩放）。
constexpr float kStatusCardH = 104.0f;
constexpr float kStatusCardHoldsH = 140.0f;  // 系统让路时多一行原因 chip
constexpr float kPairCardH = 148.0f;          // 模式 / 间隔
constexpr float kEffectsCollapsedH = 56.0f;
constexpr float kEffectsExpandedH = 150.0f;
constexpr float kHoldsCollapsedH = 56.0f;
constexpr float kHoldsExpandedH = 192.0f;
constexpr float kEmptyCardH = 180.0f;
constexpr float kFolderCardH = 208.0f;
constexpr float kFormatsCardH = 152.0f;
constexpr float kLibraryPairCardH = 176.0f;   // 分辨率 / 适配
constexpr float kFooterH = 20.0f;

constexpr float kStatusActionsW = 88.0f * 2.0f + 8.0f;
constexpr float kStatusCountdownW = 104.0f;

constexpr const char* kMonoFontStack =
    "Cascadia Code, SFMono-Regular, Consolas, monospace";

float fs(float pixels) { return ::core::theme::scaled(pixels); }
float roundFs(float base) {
    return std::round(base * ::core::theme::fontScale());
}

eui::Color transparent() { return eui::Color{0.0f, 0.0f, 0.0f, 0.0f}; }

// 直接作为内容列子元素的卡（x/y 由列布局给）。
void cardStack(eui::Ui& ui, const std::string& id, float w, float h,
               const Tokens& t, const std::function<void()>& body) {
    ui.stack(id).size(w, h).content([&] {
        ui_kit::cardFrame(ui, id, w, h, t);
        if (body) body();
    }).build();
}

// 行内定位的卡（模式/间隔、速度/音频、分辨率/适配）。
void cardAt(eui::Ui& ui, const std::string& id, float x, float w, float h,
            const Tokens& t, const std::function<void()>& body) {
    ui.stack(id).position(x, 0.0f).size(w, h).content([&] {
        ui_kit::cardFrame(ui, id, w, h, t);
        if (body) body();
    }).build();
}

void cardTitle(eui::Ui& ui, const std::string& id, const std::string& text,
               const Tokens& t, float w, float y = kCardPad) {
    ui.text(id)
        .position(kCardPad, y)
        .size(std::max(0.0f, w - kCardPad * 2.0f), roundFs(24.0f))
        .text(text)
        .fontSize(fs(15.0f))
        .lineHeight(roundFs(24.0f))
        .fontWeight(600)
        .color(t.colors.textPrimary)
        .build();
}

void hintText(eui::Ui& ui, const std::string& id, const std::string& text,
              const Tokens& t, float x, float y, float w, float h) {
    ui.text(id)
        .position(x, y)
        .size(std::max(0.0f, w), h)
        .text(text)
        .fontSize(fs(12.0f))
        .lineHeight(roundFs(18.0f))
        .wrap(true)
        .maxWidth(std::max(0.0f, w))
        .color(t.colors.textTertiary)
        .build();
}

// 主题 token 分段控件：容器 8 圆角 + 内嵌选中块（6 圆角，primarySoftBg）。
// itemIds 显式给出（来源/模式/分辨率/适配各有契约 id），标签文案由调用方给。
void segmented(eui::Ui& ui, const std::string& id, float x, float y, float w,
               float h, const std::vector<std::string>& itemIds,
               const std::vector<std::string>& labels, int selected,
               const Tokens& t, const std::function<void(int)>& onSelect) {
    const int count = static_cast<int>(std::min(itemIds.size(), labels.size()));
    if (count <= 0) return;
    const int sel = std::clamp(selected, 0, count - 1);
    ui.stack(id).position(x, y).size(w, h).content([&] {
        ui.rect(id + ".bg")
            .size(w, h)
            .color(t.colors.surface)
            .radius(8.0f)
            .border(1.0f, t.colors.borderDefault)
            .build();
        const float inset = 2.0f;
        const float gap = 2.0f;
        const float itemW =
            std::max(0.0f, (w - inset * 2.0f - gap * static_cast<float>(count - 1)) /
                               static_cast<float>(count));
        const float itemH = std::max(0.0f, h - inset * 2.0f);
        for (int i = 0; i < count; ++i) {
            const float itemX = inset + static_cast<float>(i) * (itemW + gap);
            const bool active = i == sel;
            auto item = ui.rect(itemIds[i])
                            .position(itemX, inset)
                            .size(itemW, itemH)
                            .radius(6.0f);
            if (active) {
                item.states(t.colors.primarySoftBg, t.colors.primarySoftBg,
                            t.colors.primarySoftBg)
                    .border(1.0f, t.colors.borderDefault);
            } else {
                item.states(transparent(), t.colors.hover, t.colors.active)
                    .border(0.0f, transparent());
            }
            item.onClick([onSelect, i] {
                        if (onSelect) onSelect(i);
                    })
                .build();
            ui.text(itemIds[i] + ".text")
                .position(itemX, inset)
                .size(itemW, itemH)
                .text(labels[i])
                .fontSize(fs(14.0f))
                .lineHeight(itemH)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(active ? t.colors.primaryText : t.colors.textSecondary)
                .build();
        }
    }).build();
}

void chip(eui::Ui& ui, const std::string& id, float x, float y, float w,
          float h, const std::string& text, const Tokens& t, bool wrap,
          bool dim) {
    ui.stack(id).position(x, y).size(w, h).content([&] {
        ui.rect(id + ".bg")
            .size(w, h)
            .color(dim ? t.colors.surface : t.colors.background)
            .radius(6.0f)
            .border(1.0f, t.colors.borderDefault)
            .build();
        ui.text(id + ".text")
            .position(10.0f, 0.0f)
            .size(std::max(0.0f, w - 20.0f), h)
            .text(text)
            .fontSize(fs(12.0f))
            .lineHeight(roundFs(16.0f))
            .wrap(wrap)
            .maxWidth(std::max(0.0f, w - 20.0f))
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.textSecondary)
            .build();
    }).build();
}

// 折叠卡标题行：整行可点（不是只点箭头），摘要 + 上下箭头随展开态翻转。
void foldTitleRow(eui::Ui& ui, const std::string& idBase,
                  const std::string& title, const std::string& summary,
                  bool expanded, float cardW, const Tokens& t,
                  const std::function<void()>& onToggle) {
    const float summaryW = 96.0f;
    const float chevronW = 14.0f;
    ui.stack(idBase + ".title")
        .position(0.0f, 0.0f)
        .size(cardW, roundFs(kEffectsCollapsedH))
        .onClick(onToggle)
        .content([&] {
            ui.text(idBase + ".title.text")
                .position(kCardPad, 16.0f)
                .size(std::max(0.0f, cardW - kCardPad * 2.0f - summaryW - 8.0f -
                                             chevronW),
                      roundFs(24.0f))
                .text(title)
                .fontSize(fs(15.0f))
                .lineHeight(roundFs(24.0f))
                .fontWeight(600)
                .color(t.colors.textPrimary)
                .build();
            ui.text(idBase + ".summary")
                .position(cardW - kCardPad - chevronW - 8.0f - summaryW, 20.0f)
                .size(summaryW, roundFs(16.0f))
                .text(summary)
                .fontSize(fs(12.0f))
                .lineHeight(roundFs(16.0f))
                .horizontalAlign(eui::HorizontalAlign::Right)
                .color(t.colors.textTertiary)
                .build();
            ui.text(idBase + ".chevron")
                .position(cardW - kCardPad - chevronW, 21.0f)
                .size(chevronW, chevronW)
                .icon(expanded ? 0xF077 : 0xF078)  // FA chevron-up / down
                .fontSize(fs(12.0f))
                .lineHeight(chevronW)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .color(t.colors.textTertiary)
                .build();
        })
        .build();
}

// 系统让路原因 chip 文案（固定四项，顺序 = HoldReason 声明序）。
const char* kHoldChipLabels[] = {"全屏 / 最大化", "屏幕锁定", "显示器熄屏",
                                 "系统睡眠"};

std::string modeHint(WallpaperKind kind, PlayMode mode) {
    if (kind == WallpaperKind::Video) {
        switch (mode) {
            case PlayMode::LoopSingle: return "只播放一个视频，不轮换。";
            case PlayMode::LoopList: return "按顺序播完一圈再回到第一个。";
            case PlayMode::Shuffle: return "每次从清单里随机挑一个。";
        }
        return "按顺序播完一圈再回到第一个。";
    }
    switch (mode) {
        case PlayMode::LoopSingle: return "只显示一张图片，不轮换。";
        case PlayMode::LoopList: return "按顺序轮播图片。";
        case PlayMode::Shuffle: return "每次随机挑一张图片。";
    }
    return "按顺序轮播图片。";
}

int intervalIndex(int seconds) {
    const int snapped = snapRotationIntervalSec(seconds);
    for (int i = 0; i < static_cast<int>(std::size(kRotationChoiceSeconds));
         ++i) {
        if (kRotationChoiceSeconds[i] == snapped) return i;
    }
    return 0;
}

int resolutionIndex(int minPixels) {
    constexpr int kTiers[] = {2073600, 3686400, 8294400};
    int best = 1;
    int bestDistance = std::abs(kTiers[1] - minPixels);
    for (int i = 0; i < 3; ++i) {
        const int distance = std::abs(kTiers[i] - minPixels);
        if (distance < bestDistance) {
            best = i;
            bestDistance = distance;
        }
    }
    return best;
}

// 过滤未接入（image_library 对 minPixels 显式忽略、filtered 恒 0）：口径必须与
// presentation::emptyStateCopy 一致——"筛选将在后续版本生效"，不得宣称"低于档位不
// 参与轮播"（审计 F1；哨兵断言见 test_wallpaper_ui_no_filter_claims）。
std::string resolutionHint(int index) {
    switch (index) {
        case 0:
            return "分辨率筛选将在后续版本生效；当前记住 1080P 档（宽 × 高 ≥ "
                   "2,073,600），全部图片参与轮播。";
        case 2:
            return "分辨率筛选将在后续版本生效；当前记住 4K 档（宽 × 高 ≥ "
                   "8,294,400），全部图片参与轮播。";
        default:
            return "分辨率筛选将在后续版本生效；当前记住 2K 档（宽 × 高 ≥ "
                   "3,686,400），全部图片参与轮播。";
    }
}

std::string fitHint(ImageFit fit) {
    switch (fit) {
        case ImageFit::Fill:
            return "默认。等比放大到铺满屏幕，超出部分裁掉，不变形。";
        case ImageFit::Fit:
            return "等比缩放到完整显示，四周留黑边（窗口底色就是黑的）。";
        case ImageFit::Center:
            return "按原始尺寸居中显示，不放大也不缩小。";
        case ImageFit::Tile:
            return "按原始尺寸重复铺满屏幕。";
    }
    return "默认。等比放大到铺满屏幕，超出部分裁掉，不变形。";
}

std::string supportLine(WallpaperKind kind) {
    if (kind == WallpaperKind::Image) {
        return "支持 jpg / jpeg / png / webp / heic；HEIC 需要系统解码扩展。";
    }
    return "支持 mp4 / mov / m4v。";
}

// ---------------- 播放页 ----------------

void drawStatusCard(eui::Ui& ui, float w, const WallpaperSnapshot& snapshot,
                    const Tokens& t, const UiActions& actions) {
    const bool systemHeld = snapshot.systemHolds != 0;
    const float h = roundFs(systemHeld ? kStatusCardHoldsH : kStatusCardH);
    const float copyW = std::max(96.0f, w - kCardPad * 2.0f -
                                             kStatusCountdownW -
                                             kStatusActionsW - 48.0f);

    cardStack(ui, "wallpaper.card.status", w, h, t, [&] {
        // 状态点：播放中 success；手动暂停 / 系统让路 warning（空态整卡不建）。
        const eui::Color dotColor = (snapshot.paused || snapshot.manualPaused)
                                        ? t.colors.warning
                                        : t.colors.success;
        ui.rect("wallpaper.status.dot")
            .position(kCardPad, 28.0f)
            .size(8.0f, 8.0f)
            .color(dotColor)
            .radius(4.0f)
            .build();

        ui.text("wallpaper.status.title")
            .position(36.0f, 20.0f)
            .size(std::max(0.0f, copyW - 16.0f), roundFs(24.0f))
            .text(snapshot.statusTitle)
            .fontSize(fs(15.0f))
            .lineHeight(roundFs(24.0f))
            .fontWeight(600)
            .maxWidth(std::max(0.0f, copyW - 16.0f))
            .overflow(core::TextOverflow::Ellipsis)
            .clip()
            .color(t.colors.textPrimary)
            .build();

        ui.text("wallpaper.status.subtitle")
            .position(kCardPad, 48.0f)
            .size(copyW, roundFs(20.0f))
            .text(snapshot.statusSubtitle)
            .fontSize(fs(14.0f))
            .lineHeight(roundFs(20.0f))
            .maxWidth(copyW)
            .overflow(core::TextOverflow::Ellipsis)
            .clip()
            .color(t.colors.textSecondary)
            .build();

        if (systemHeld) {
            chip(ui, "wallpaper.status.reasons", kCardPad, 76.0f, copyW,
                 roundFs(44.0f), presentation::holdReasonsLabel(snapshot.systemHolds),
                 t, /*wrap=*/true, /*dim=*/true);
        }

        // 倒计时：值 + 说明都取自快照 countdownLabel（不在此拼状态文案）。
        std::string countdownValue = "—";
        std::string countdownCaption = snapshot.countdownLabel;
        const bool notRotating =
            snapshot.mode == PlayMode::LoopSingle ||
            snapshot.secondsUntilRotation < 0;
        if (!notRotating && !snapshot.paused) {
            const std::size_t split = countdownCaption.rfind(' ');
            if (split != std::string::npos) {
                countdownValue = countdownCaption.substr(split + 1);
                countdownCaption = countdownCaption.substr(0, split);
            }
        }
        const float countdownX =
            w - kCardPad - kStatusActionsW - 24.0f - kStatusCountdownW;
        const float countdownY = (h - roundFs(48.0f)) * 0.5f;
        ui.text("wallpaper.status.countdown")
            .position(countdownX, countdownY)
            .size(kStatusCountdownW, roundFs(30.0f))
            .text(countdownValue)
            .fontSize(fs(24.0f))
            .lineHeight(roundFs(30.0f))
            .fontWeight(600)
            .horizontalAlign(eui::HorizontalAlign::Right)
            .color(t.colors.textPrimary)
            .build();
        ui.text("wallpaper.status.countdown.label")
            .position(countdownX, countdownY + roundFs(30.0f))
            .size(kStatusCountdownW, roundFs(18.0f))
            .text(countdownCaption)
            .fontSize(fs(12.0f))
            .lineHeight(roundFs(18.0f))
            .horizontalAlign(eui::HorizontalAlign::Right)
            .color(t.colors.textTertiary)
            .build();

        const float actionsX = w - kCardPad - kStatusActionsW;
        const float actionsY = (h - ui_kit::kButtonHeight) * 0.5f;
        ui_kit::ButtonOptions pauseOptions;
        if (actions.togglePause) {
            const auto onPause = actions.togglePause;
            pauseOptions.onClick = [onPause] { onPause(); };
        }
        ui_kit::button(ui, "wallpaper.status.pause",
                       snapshot.manualPaused ? "继续" : "暂停",
                       ui_kit::ButtonLevel::Primary, actionsX, actionsY, 88.0f,
                       ui_kit::kButtonHeight, t, pauseOptions);
        ui_kit::ButtonOptions nextOptions;
        if (actions.next) {
            const auto onNext = actions.next;
            nextOptions.onClick = [onNext] { onNext(); };
        }
        ui_kit::button(ui, "wallpaper.status.next", "下一个",
                       ui_kit::ButtonLevel::Secondary, actionsX + 96.0f,
                       actionsY, 88.0f, ui_kit::kButtonHeight, t, nextOptions);

        // 3px 进度条：轮换中显示剩余比例；暂停 / 单循环隐藏。
        if (!notRotating && !snapshot.paused) {
            const float progress =
                std::clamp(snapshot.rotationProgress, 0.0f, 1.0f);
            ui.rect("wallpaper.status.progress")
                .position(0.0f, h - 3.0f)
                .size(w, 3.0f)
                .color(t.colors.borderMuted)
                .build();
            ui.rect("wallpaper.status.progress.fill")
                .position(0.0f, h - 3.0f)
                .size(w * (1.0f - progress), 3.0f)
                .color(systemHeld ? t.colors.warning : t.colors.primary)
                .build();
        }
    });
}

void drawEmptyCard(eui::Ui& ui, float w, const WallpaperSnapshot& snapshot,
                   WallpaperKind kind, const Tokens& t,
                   const UiActions& actions) {
    const auto copy = presentation::emptyStateCopy(snapshot.availability, kind);
    const std::string title = copy.has_value() ? copy->title : std::string();
    const std::string reason = copy.has_value() ? copy->reason : std::string();
    const std::string primary = copy.has_value() ? copy->primaryAction : "选择文件夹…";
    const std::string secondary = copy.has_value() ? copy->secondaryAction : "重新扫描";

    cardStack(ui, "wallpaper.card.empty", w, roundFs(kEmptyCardH), t, [&] {
        ui.rect("wallpaper.empty.icon")
            .position(kCardPad, 24.0f)
            .size(44.0f, 44.0f)
            .color(t.colors.primarySoftBg)
            .radius(12.0f)
            .build();
        ui.text("wallpaper.empty.icon.glyph")
            .position(kCardPad, 24.0f)
            .size(44.0f, 44.0f)
            .icon(0xF07B)  // FA folder-open
            .fontSize(fs(20.0f))
            .lineHeight(44.0f)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.primaryText)
            .build();
        const float textX = 84.0f;
        const float textW = std::max(0.0f, w - textX - kCardPad);
        ui.text("wallpaper.card.empty.title")
            .position(textX, 24.0f)
            .size(textW, roundFs(24.0f))
            .text(title)
            .fontSize(fs(18.0f))
            .lineHeight(roundFs(24.0f))
            .fontWeight(600)
            .color(t.colors.textPrimary)
            .build();
        ui.text("wallpaper.empty.body")
            .position(textX, 56.0f)
            .size(textW, roundFs(20.0f))
            .text(reason)
            .fontSize(fs(14.0f))
            .lineHeight(roundFs(20.0f))
            .maxWidth(textW)
            .overflow(core::TextOverflow::Ellipsis)
            .clip()
            .color(t.colors.textSecondary)
            .build();
        ui.text("wallpaper.empty.support")
            .position(textX, 94.0f)
            .size(textW, roundFs(16.0f))
            .text(supportLine(kind))
            .fontSize(fs(12.0f))
            .lineHeight(roundFs(16.0f))
            .maxWidth(textW)
            .overflow(core::TextOverflow::Ellipsis)
            .clip()
            .color(t.colors.textTertiary)
            .build();

        ui_kit::ButtonOptions primaryOptions;
        if (actions.selectFolder) {
            const auto onSelect = actions.selectFolder;
            primaryOptions.onClick = [onSelect] { onSelect(); };
        }
        ui_kit::button(ui, "wallpaper.empty.primary", primary,
                       ui_kit::ButtonLevel::Primary, textX, 120.0f, 128.0f,
                       ui_kit::kButtonHeight, t, primaryOptions);
        ui_kit::ButtonOptions secondaryOptions;
        if (actions.rescan) {
            const auto onRescan = actions.rescan;
            secondaryOptions.onClick = [onRescan] { onRescan(); };
        }
        ui_kit::button(ui, "wallpaper.empty.secondary", secondary,
                       ui_kit::ButtonLevel::Secondary, textX + 136.0f, 120.0f,
                       96.0f, ui_kit::kButtonHeight, t, secondaryOptions);
    });
}

void drawModeCard(eui::Ui& ui, float x, float w, float h,
                  const WallpaperSnapshot& snapshot, WallpaperKind kind,
                  const Tokens& t, const UiActions& actions) {
    cardAt(ui, "wallpaper.card.mode", x, w, h, t, [&] {
        cardTitle(ui, "wallpaper.card.mode.title",
                  kind == WallpaperKind::Video ? "播放模式" : "轮播方式", t, w);
        const std::vector<std::string> ids = {"wallpaper.mode.0",
                                              "wallpaper.mode.1",
                                              "wallpaper.mode.2"};
        const std::vector<std::string> labels = {
            presentation::playModeLabel(PlayMode::LoopSingle, kind),
            presentation::playModeLabel(PlayMode::LoopList, kind),
            presentation::playModeLabel(PlayMode::Shuffle, kind)};
        const PlayMode modes[] = {PlayMode::LoopSingle, PlayMode::LoopList,
                                  PlayMode::Shuffle};
        int selected = 0;
        for (int i = 0; i < 3; ++i) {
            if (modes[i] == snapshot.mode) selected = i;
        }
        const auto onMode = actions.setMode;
        segmented(ui, "wallpaper.mode", kCardPad, 60.0f,
                  std::max(0.0f, w - kCardPad * 2.0f), 36.0f, ids, labels,
                  selected, t, [onMode, modes](int index) {
                      if (onMode) onMode(modes[index]);
                  });
        hintText(ui, "wallpaper.card.mode.hint", modeHint(kind, snapshot.mode),
                 t, kCardPad, 104.0f, std::max(0.0f, w - kCardPad * 2.0f),
                 roundFs(18.0f));
    });
}

void drawIntervalCard(eui::Ui& ui, float x, float w, float h,
                      const WallpaperSnapshot& snapshot, WallpaperKind kind,
                      const Tokens& t, const UiActions& actions,
                      UiState& state) {
    const bool disabled = snapshot.mode == PlayMode::LoopSingle;
    const auto compTheme = ::core::theme::componentTokensWeChat(t);
    components::DropdownStyle style(compTheme);
    style.radius = 8.0f;  // 控件圆角 8（组件默认卡圆角 12）
    const auto onInterval = actions.setInterval;
    cardAt(ui, "wallpaper.card.interval", x, w, h, t, [&] {
        cardTitle(ui, "wallpaper.card.interval.title", "轮换间隔", t, w);
        std::vector<std::string> labels;
        for (int seconds : kRotationChoiceSeconds) {
            labels.push_back(presentation::rotationIntervalLabel(seconds));
        }
        const float ddW = std::max(0.0f, w - kCardPad * 2.0f);
        // 契约：DropdownBuilder 无 .position()（根 stack 走流式布局），wrapper
        // 只负责把字段盒钉在 (kCardPad, 60)、ddW×40；其 build() 根高 = 字段高
        // + gap + 弹层高，wrapper 不画背景、content 内不加偏移，弹层几何由组件自管。
        ui.stack("wallpaper.interval.dropdown.wrap")
            .position(kCardPad, 60.0f)
            .size(ddW, 40.0f)
            .content([&] {
                components::dropdown(ui, "wallpaper.interval.dropdown")
                    .size(ddW, 40.0f)
                    .items(labels)
                    .selected(intervalIndex(snapshot.rotationIntervalSec))
                    .itemHeight(30.0f)
                    .theme(compTheme)
                    .style(style)
                    .open(!disabled && state.intervalOpen)
                    .onChange([onInterval](int index) {
                        if (!onInterval) return;
                        if (index >= 0 &&
                            index < static_cast<int>(std::size(kRotationChoiceSeconds))) {
                            onInterval(kRotationChoiceSeconds[index]);
                        }
                    })
                    .onOpenChange([&state, disabled](bool open) {
                        state.intervalOpen = disabled ? false : open;
                    })
                    .build();
            })
            .build();
        if (disabled) {
            // 组件没有 disabled 旗标：盖一层半透明遮罩既断开点击也表达失效。
            // 不变量：遮罩 zIndex 必须 > dropdown 根 stack 的恒定档 20
            //（components/dropdown.h 默认）——否则字段子树按 subtreeMaxZIndex
            // 后绘 + 先命中，遮罩视觉与点击双失效（审计 F2；变异断言见
            // test_wallpaper_ui_interval_and_modes）。disabled 时 open 恒 false，
            // 弹层子树（900 档）不存在，25 档不会与弹层冲突。
            ui.rect("wallpaper.interval.dropdown.disabled")
                .position(kCardPad, 60.0f)
                .size(ddW, 40.0f)
                .color(eui::Color{t.colors.surface.r, t.colors.surface.g,
                                  t.colors.surface.b, 0.62f})
                .radius(8.0f)
                .border(1.0f, t.colors.borderDefault)
                .zIndex(25)
                .blockPointer()
                .build();
        }
        hintText(
            ui, "wallpaper.card.interval.hint",
            disabled
                ? "单循环 / 单张不变时不轮换，间隔不可用。"
                : (kind == WallpaperKind::Image
                       ? "顺序 / 随机轮播时按这个间隔换图。"
                       : "列表 / 随机时按这个间隔换片。"),
            t, kCardPad, 108.0f, std::max(0.0f, w - kCardPad * 2.0f),
            roundFs(18.0f));
    });
}

void drawSpeedCard(eui::Ui& ui, float x, float w, float rowH,
                   const WallpaperSnapshot& snapshot, const Tokens& t,
                   UiState& state, const UiActions& actions) {
    const bool expanded = state.effectsExpanded;
    cardAt(ui, "wallpaper.card.speed", x, w, rowH, t, [&] {
        const auto onToggle = [&state] { state.effectsExpanded = !state.effectsExpanded; };
        foldTitleRow(ui, "wallpaper.speed", "播放速度",
                     presentation::rateLabel(snapshot.rate), expanded, w, t,
                     onToggle);
        if (!expanded) return;
        const auto onRate = actions.setRate;
        const float valueW = 56.0f;
        const float sliderW = std::max(0.0f, w - kCardPad * 2.0f - valueW - 8.0f);
        const float normalized =
            std::clamp((snapshot.rate - 0.5f) / 1.5f, 0.0f, 1.0f);
        ui_kit::slider(ui, "wallpaper.speed.slider", kCardPad, 64.0f, sliderW,
                       24.0f, normalized, t, [onRate](float value) {
                           if (onRate) onRate(0.5f + value * 1.5f);
                       });
        ui.text("wallpaper.speed.value")
            .position(w - kCardPad - valueW, 66.0f)
            .size(valueW, roundFs(20.0f))
            .text(presentation::rateLabel(snapshot.rate))
            .fontSize(fs(14.0f))
            .lineHeight(roundFs(20.0f))
            .horizontalAlign(eui::HorizontalAlign::Right)
            .color(t.colors.textPrimary)
            .build();
        hintText(ui, "wallpaper.speed.hint", "0.5×–2.0×，变速不变调。", t,
                 kCardPad, 98.0f, std::max(0.0f, w - kCardPad * 2.0f),
                 roundFs(18.0f));
    });
}

void drawAudioCard(eui::Ui& ui, float x, float w, float rowH,
                   const WallpaperSnapshot& snapshot, const Tokens& t,
                   UiState& state, const UiActions& actions) {
    const bool expanded = state.effectsExpanded;
    cardAt(ui, "wallpaper.card.audio", x, w, rowH, t, [&] {
        const auto onToggle = [&state] { state.effectsExpanded = !state.effectsExpanded; };
        foldTitleRow(ui, "wallpaper.audio", "音频",
                     snapshot.muted ? "已静音"
                                    : presentation::volumeLabel(snapshot.volume),
                     expanded, w, t, onToggle);
        if (!expanded) return;
        const auto onVolume = actions.setVolume;
        const auto onMuted = actions.setMuted;
        const float valueW = 56.0f;
        const float muteLabelW = 28.0f;
        const float switchW = 44.0f;
        const float rightSide = valueW + 8.0f + muteLabelW + 8.0f + switchW;
        const float sliderW =
            std::max(0.0f, w - kCardPad * 2.0f - rightSide - 8.0f);
        ui_kit::slider(ui, "wallpaper.audio.slider", kCardPad, 64.0f, sliderW,
                       24.0f, std::clamp(snapshot.volume, 0.0f, 1.0f), t,
                       [onVolume](float value) {
                           if (onVolume) onVolume(value);
                       });
        const float switchX = w - kCardPad - switchW;
        const float muteLabelX = switchX - 8.0f - muteLabelW;
        ui.text("wallpaper.audio.value")
            .position(muteLabelX - 8.0f - valueW, 66.0f)
            .size(valueW, roundFs(20.0f))
            .text(presentation::volumeLabel(snapshot.volume))
            .fontSize(fs(14.0f))
            .lineHeight(roundFs(20.0f))
            .horizontalAlign(eui::HorizontalAlign::Right)
            .color(t.colors.textPrimary)
            .build();
        ui.text("wallpaper.audio.mute.label")
            .position(muteLabelX, 66.0f)
            .size(muteLabelW, roundFs(20.0f))
            .text("静音")
            .fontSize(fs(12.0f))
            .lineHeight(roundFs(20.0f))
            .color(t.colors.textSecondary)
            .build();
        ui_kit::wswitch(ui, "wallpaper.audio.mute", snapshot.muted, switchX,
                        62.0f, switchW, 24.0f, t, [onMuted](bool on) {
                            if (onMuted) onMuted(on);
                        });
        hintText(ui, "wallpaper.audio.hint", "静音时视频照常播放。", t,
                 kCardPad, 98.0f, std::max(0.0f, w - kCardPad * 2.0f),
                 roundFs(18.0f));
    });
}

void drawHoldsCard(eui::Ui& ui, float w, const WallpaperSnapshot& snapshot,
                   const Tokens& t, UiState& state,
                   const UiActions& actions) {
    const bool expanded = state.holdsExpanded;
    const float h = roundFs(expanded ? kHoldsExpandedH : kHoldsCollapsedH);
    cardStack(ui, "wallpaper.card.holds", w, h, t, [&] {
        const auto onToggle = [&state] { state.holdsExpanded = !state.holdsExpanded; };
        foldTitleRow(ui, "wallpaper.holds", "这些情况会让路",
                     snapshot.pauseOnBattery ? "电池供电时暂停"
                                             : "电池供电时继续",
                     expanded, w, t, onToggle);
        if (!expanded) return;
        float chipX = kCardPad;
        for (int i = 0; i < 4; ++i) {
            const std::string label = kHoldChipLabels[i];
            const float chipW = core::TextPrimitive::measureTextWidth(
                                    label, {}, fs(12.0f), 400) +
                                20.0f;
            chip(ui, "wallpaper.holds.reason." + std::to_string(i), chipX,
                 64.0f, chipW, roundFs(26.0f), label, t, false, false);
            chipX += chipW + 6.0f;
        }
        ui.text("wallpaper.holds.battery.label")
            .position(kCardPad, 104.0f)
            .size(std::max(0.0f, w - kCardPad * 2.0f - 60.0f),
                  roundFs(20.0f))
            .text("电池供电")
            .fontSize(fs(14.0f))
            .lineHeight(roundFs(20.0f))
            .color(t.colors.textPrimary)
            .build();
        ui.text("wallpaper.holds.battery.sub")
            .position(kCardPad, 122.0f)
            .size(std::max(0.0f, w - kCardPad * 2.0f - 60.0f),
                  roundFs(18.0f))
            .text("关闭后，用电池时也继续播放（更费电）。")
            .fontSize(fs(12.0f))
            .lineHeight(roundFs(18.0f))
            .color(t.colors.textTertiary)
            .build();
        const auto onBattery = actions.setPauseOnBattery;
        ui_kit::wswitch(ui, "wallpaper.holds.battery", snapshot.pauseOnBattery,
                        w - kCardPad - 44.0f, 104.0f, 44.0f, 24.0f, t,
                        [onBattery](bool on) {
                            if (onBattery) onBattery(on);
                        });
        hintText(ui, "wallpaper.holds.hint",
                 "前四项不可关闭。任何一项解除后自动续播，播放位置从暂停处继续——不会从头开始。",
                 t, kCardPad, 148.0f, std::max(0.0f, w - kCardPad * 2.0f),
                 roundFs(20.0f));
    });
}

void drawPlayPage(eui::Ui& ui, float contentW, const WallpaperSnapshot& snapshot,
                  WallpaperKind kind, const Tokens& t, const UiActions& actions,
                  UiState& state) {
    const bool playing = snapshot.availability == Availability::Playing;
    if (playing) {
        drawStatusCard(ui, contentW, snapshot, t, actions);

        const float colW = std::max(0.0f, (contentW - kCardGap) * 0.5f);
        const float pairH = roundFs(kPairCardH);
        ui.stack("wallpaper.play.pair")
            .size(contentW, pairH)
            .content([&] {
                drawModeCard(ui, 0.0f, colW, pairH, snapshot, kind, t, actions);
                drawIntervalCard(ui, colW + kCardGap, colW, pairH, snapshot,
                                 kind, t, actions, state);
            })
            .build();

        // 图片模式隐藏速度 + 音频整行（需求 §7.3）。
        if (kind == WallpaperKind::Video) {
            const float effectsH = roundFs(state.effectsExpanded
                                               ? kEffectsExpandedH
                                               : kEffectsCollapsedH);
            ui.stack("wallpaper.play.effects")
                .size(contentW, effectsH)
                .content([&] {
                    drawSpeedCard(ui, 0.0f, colW, effectsH, snapshot, t, state,
                                  actions);
                    drawAudioCard(ui, colW + kCardGap, colW, effectsH,
                                  snapshot, t, state, actions);
                })
                .build();
        }
    } else {
        drawEmptyCard(ui, contentW, snapshot, kind, t, actions);
    }

    // 让路卡空态仍可见（信息性），模式/间隔/速度/音频已隐藏。
    drawHoldsCard(ui, contentW, snapshot, t, state, actions);
}

// ---------------- 片库页 ----------------

void drawFolderStatCell(eui::Ui& ui, const std::string& id, float x,
                        const std::string& value, const std::string& label,
                        const Tokens& t, bool accent) {
    ui.text(id)
        .position(x, 100.0f)
        .size(160.0f, roundFs(26.0f))
        .text(value)
        .fontSize(fs(18.0f))
        .lineHeight(roundFs(26.0f))
        .fontWeight(600)
        .color(accent ? t.colors.accentText : t.colors.textPrimary)
        .build();
    ui.text(id + ".label")
        .position(x, 128.0f)
        .size(160.0f, roundFs(16.0f))
        .text(label)
        .fontSize(fs(12.0f))
        .lineHeight(roundFs(16.0f))
        .color(t.colors.textTertiary)
        .build();
}

void drawFolderCard(eui::Ui& ui, float w, const WallpaperSnapshot& snapshot,
                    WallpaperKind kind, const Tokens& t,
                    const UiActions& actions) {
    const bool configured = !snapshot.folderPath.empty();
    const bool missing = snapshot.availability == Availability::FolderMissing;
    const bool scanning = snapshot.scanning;
    const float h = roundFs(kFolderCardH);
    cardStack(ui, "wallpaper.card.folder", w, h, t, [&] {
        cardTitle(ui, "wallpaper.card.folder.title", "壁纸文件夹", t, w);

        // 路径行：mono、省略；目录缺失追加「· 目录不见了」。不显示任何文件名。
        const float selectW = 88.0f;
        const float rescanW = 88.0f;
        const float actionsW = selectW + 8.0f + rescanW;
        const float pathX = 48.0f;
        const float pathW =
            std::max(0.0f, w - kCardPad - pathX - actionsW - 16.0f);
        std::string pathText = configured ? snapshot.folderPath
                                          : std::string("尚未选择文件夹");
        if (configured && missing) pathText += " · 目录不见了";
        ui.text("wallpaper.folder.icon")
            .position(kCardPad, 64.0f)
            .size(20.0f, 20.0f)
            .icon(0xF07B)  // FA folder-open
            .fontSize(fs(16.0f))
            .lineHeight(20.0f)
            .color(t.colors.textTertiary)
            .build();
        auto path = ui.text("wallpaper.folder.path")
                        .position(pathX, 54.0f)
                        .size(pathW, 40.0f)
                        .text(pathText)
                        .fontSize(fs(14.0f))
                        .lineHeight(40.0f)
                        .maxWidth(pathW)
                        .overflow(core::TextOverflow::Ellipsis)
                        .clip()
                        .color(missing ? t.colors.warning
                                       : (configured ? t.colors.textPrimary
                                                     : t.colors.textTertiary));
        if (configured) path.fontFamily(kMonoFontStack);
        path.build();

        const float selectX = w - kCardPad - actionsW;
        ui_kit::ButtonOptions selectOptions;
        ui_kit::ButtonOptions rescanOptions;
        if (!scanning) {
            if (actions.selectFolder) {
                const auto onSelect = actions.selectFolder;
                selectOptions.onClick = [onSelect] { onSelect(); };
            }
            if (actions.rescan) {
                const auto onRescan = actions.rescan;
                rescanOptions.onClick = [onRescan] { onRescan(); };
            }
        }
        ui_kit::button(ui, "wallpaper.folder.select", "选择…",
                       ui_kit::ButtonLevel::Secondary, selectX, 54.0f,
                       selectW, ui_kit::kButtonHeight, t, selectOptions);
        ui_kit::button(ui, "wallpaper.folder.rescan", "重新扫描",
                       ui_kit::ButtonLevel::Text, selectX + selectW + 8.0f,
                       54.0f, rescanW, ui_kit::kButtonHeight, t, rescanOptions);
        if (scanning) {
            // 扫描中按钮禁用（视觉 + 无回调）；统计暂时隐藏。
            ui.rect("wallpaper.folder.select.disabled")
                .position(selectX, 54.0f)
                .size(selectW, ui_kit::kButtonHeight)
                .color(eui::Color{t.colors.surface.r, t.colors.surface.g,
                                  t.colors.surface.b, 0.55f})
                .radius(8.0f)
                .blockPointer()
                .build();
            ui.rect("wallpaper.folder.rescan.disabled")
                .position(selectX + selectW + 8.0f, 54.0f)
                .size(rescanW, ui_kit::kButtonHeight)
                .color(eui::Color{t.colors.surface.r, t.colors.surface.g,
                                  t.colors.surface.b, 0.55f})
                .radius(8.0f)
                .blockPointer()
                .build();
        }

        const bool showStats = configured && !missing && !scanning;
        if (scanning) {
            ui.stack("wallpaper.folder.scanning")
                .position(kCardPad, 102.0f)
                .size(std::max(0.0f, w - kCardPad * 2.0f), 26.0f)
                .content([&] {
                    ui.text("wallpaper.folder.scanning.icon")
                        .position(0.0f, 4.0f)
                        .size(16.0f, 16.0f)
                        .icon(0xF110)  // FA spinner
                        .fontSize(fs(12.0f))
                        .lineHeight(16.0f)
                        .color(t.colors.textTertiary)
                        .build();
                    ui.text("wallpaper.folder.scanning.text")
                        .position(22.0f, 0.0f)
                        .size(std::max(0.0f, w - kCardPad * 2.0f - 22.0f),
                              roundFs(24.0f))
                        .text("正在扫描…")
                        .fontSize(fs(14.0f))
                        .lineHeight(roundFs(24.0f))
                        .color(t.colors.textSecondary)
                        .build();
                })
                .build();
        } else if (showStats) {
            if (kind == WallpaperKind::Video) {
                ui.text("wallpaper.folder.stat.videos")
                    .position(kCardPad, 100.0f)
                    .size(std::max(0.0f, w - kCardPad * 2.0f), 40.0f)
                    .text(presentation::libraryCountLabel(kind,
                                                          snapshot.videoCount))
                    .fontSize(fs(18.0f))
                    .lineHeight(40.0f)
                    .fontWeight(600)
                    .color(t.colors.textPrimary)
                    .build();
            } else {
                // 只画"目录内图片"一格：passing 恒等于 total、filtered 恒 0
                //（image_library.cpp），参与/低于两格是"已过滤"的虚假宣称（审计 F1）。
                drawFolderStatCell(ui, "wallpaper.folder.stat.total", kCardPad,
                                   std::to_string(snapshot.imageTotal),
                                   "目录内图片", t, false);
            }
        }

        hintText(ui, "wallpaper.folder.hint",
                 kind == WallpaperKind::Image
                     ? "仅扫描 jpg / jpeg / png / webp / heic；分辨率筛选将在后续版本生效。"
                     : "仅扫描 mp4 / mov / m4v；其他格式不会出现在片库，也不会自动转码。",
                 t, kCardPad, 154.0f,
                 std::max(0.0f, w - kCardPad * 2.0f), roundFs(34.0f));
    });
}

void drawFormatsCard(eui::Ui& ui, float w, const Tokens& t) {
    cardStack(ui, "wallpaper.card.formats", w, roundFs(kFormatsCardH), t, [&] {
        cardTitle(ui, "wallpaper.card.formats.title", "支持的视频格式", t, w);
        const char* chips[] = {".mp4", ".mov", ".m4v"};
        for (int i = 0; i < 3; ++i) {
            const float chipW = 64.0f;
            const float chipX = kCardPad + static_cast<float>(i) * (chipW + 8.0f);
            chip(ui, "wallpaper.formats.chip." + std::to_string(i), chipX,
                 60.0f, chipW, roundFs(28.0f), chips[i], t, false, false);
        }
        hintText(ui, "wallpaper.card.formats.hint",
                 "目录里的 MKV / AVI / WEBM 不会被扫描。需要其他格式时，请先自行转成 mp4。",
                 t, kCardPad, 100.0f,
                 std::max(0.0f, w - kCardPad * 2.0f), roundFs(34.0f));
    });
}

void drawResolutionCard(eui::Ui& ui, float x, float w, float h,
                        const WallpaperSnapshot& snapshot, const Tokens& t,
                        const UiActions& actions) {
    cardAt(ui, "wallpaper.card.resolution", x, w, h, t, [&] {
        cardTitle(ui, "wallpaper.card.resolution.title", "分辨率筛选", t, w);
        const int selected = resolutionIndex(snapshot.imageMinPixels);
        const auto onPixels = actions.setImageMinPixels;
        segmented(ui, "wallpaper.resolution", kCardPad, 60.0f,
                  std::max(0.0f, w - kCardPad * 2.0f), 36.0f,
                  {"wallpaper.resolution.0", "wallpaper.resolution.1",
                   "wallpaper.resolution.2"},
                  {"1080P", "2K", "4K"}, selected, t,
                  [onPixels](int index) {
                      constexpr int kTiers[] = {2073600, 3686400, 8294400};
                      if (onPixels) onPixels(kTiers[index]);
                  });
        hintText(ui, "wallpaper.card.resolution.hint", resolutionHint(selected),
                 t, kCardPad, 104.0f, std::max(0.0f, w - kCardPad * 2.0f),
                 roundFs(52.0f));
    });
}

void drawFitCard(eui::Ui& ui, float x, float w, float h,
                 const WallpaperSnapshot& snapshot, const Tokens& t,
                 const UiActions& actions) {
    cardAt(ui, "wallpaper.card.fit", x, w, h, t, [&] {
        cardTitle(ui, "wallpaper.card.fit.title", "图片适配", t, w);
        const auto onFit = actions.setImageFit;
        const int selected = static_cast<int>(snapshot.imageFit);
        segmented(ui, "wallpaper.fit", kCardPad, 60.0f,
                  std::max(0.0f, w - kCardPad * 2.0f), 36.0f,
                  {"wallpaper.fit.0", "wallpaper.fit.1", "wallpaper.fit.2",
                   "wallpaper.fit.3"},
                  {"填充", "适应", "居中", "平铺"}, selected, t,
                  [onFit](int index) {
                      if (onFit) onFit(static_cast<ImageFit>(index));
                  });
        hintText(ui, "wallpaper.card.fit.hint", fitHint(snapshot.imageFit), t,
                 kCardPad, 104.0f, std::max(0.0f, w - kCardPad * 2.0f),
                 roundFs(52.0f));
    });
}

void drawLibraryPage(eui::Ui& ui, float contentW,
                     const WallpaperSnapshot& snapshot, WallpaperKind kind,
                     const Tokens& t, const UiActions& actions) {
    drawFolderCard(ui, contentW, snapshot, kind, t, actions);
    if (kind == WallpaperKind::Video) {
        drawFormatsCard(ui, contentW, t);
    } else {
        const float colW = std::max(0.0f, (contentW - kCardGap) * 0.5f);
        const float rowH = roundFs(kLibraryPairCardH);
        ui.stack("wallpaper.library.pair")
            .size(contentW, rowH)
            .content([&] {
                drawResolutionCard(ui, 0.0f, colW, rowH, snapshot, t, actions);
                drawFitCard(ui, colW + kCardGap, colW, rowH, snapshot, t,
                            actions);
            })
            .build();
    }
    ui.text("wallpaper.library.footer")
        .size(contentW, roundFs(kFooterH))
        // 不写 mpv 版本号：mpv.exe 不进 git、正式打包在 P6，写死版本是未经证实的
        // 对外宣称（用户机器版本可变；审计 F5）。
        .text("壁纸引擎 mpv · GPL v2 · 设置即时生效")
        .fontSize(fs(12.0f))
        .lineHeight(roundFs(20.0f))
        .horizontalAlign(eui::HorizontalAlign::Center)
        .color(t.colors.textTertiary)
        .build();
}

void drawHeader(eui::Ui& ui, float contentW, WallpaperKind kind,
                const Tokens& t, UiState& state, const UiActions& actions) {
    ui.stack("wallpaper.header").size(contentW, ui_kit::kHeaderH).content([&] {
        ui.text("wallpaper.title")
            .position(0.0f, 0.0f)
            .size(std::max(0.0f, contentW - 136.0f), ui_kit::kHeaderH)
            .text("壁纸")
            .fontSize(t.sizes.fontSizeTitle)
            .lineHeight(ui_kit::kHeaderH)
            .fontWeight(700)
            .color(t.colors.textPrimary)
            .build();
        const auto onKind = actions.setKind;
        segmented(ui, "wallpaper.source", std::max(0.0f, contentW - 120.0f),
                  2.0f, 120.0f, 36.0f,
                  {"wallpaper.source.video", "wallpaper.source.image"},
                  {"视频", "图片"}, kind == WallpaperKind::Image ? 1 : 0, t,
                  [&state, onKind](int index) {
                      const WallpaperKind next =
                          index == 1 ? WallpaperKind::Image
                                     : WallpaperKind::Video;
                      state.kindDraft = next;
                      state.kindDraftValid = true;
                      if (onKind) onKind(next);
                  });
    });
}

void drawTabs(eui::Ui& ui, float contentW, const Tokens& t, UiState& state) {
    const char* labels[] = {"播放", "片库"};
    ui.stack("wallpaper.tabs").size(contentW, 40.0f).content([&] {
        float tabX = 0.0f;
        for (int i = 0; i < 2; ++i) {
            const std::string label = labels[i];
            const float tabW =
                core::TextPrimitive::measureTextWidth(
                    label, {}, ui_kit::kWtabFontSize, 400) +
                32.0f;
            ui_kit::wtab(ui, "wallpaper.tabs." + std::to_string(i), label,
                         state.tab == i, tabX, 0.0f, tabW, 40.0f, t,
                         [&state, i] { state.tab = i; });
            tabX += tabW + 4.0f;
        }
    });
}

} // namespace

void composeWallpaperUi(eui::Ui& ui, const eui::Screen& screen,
                        const WallpaperSnapshot& snapshot,
                        const UiActions& actions, UiState& state) {
    const auto& t = ::core::theme::current();
    if (!state.kindDraftValid) {
        state.kindDraft = snapshot.kind;
        state.kindDraftValid = true;
    }
    const WallpaperKind kind = state.kindDraft;
    const float contentW = std::max(0.0f, screen.width - kPagePadX * 2.0f);
    const float viewportH = std::max(
        0.0f, screen.height - kPagePadBottom - ui_kit::kHeaderH -
                  kCardGap - 40.0f - kCardGap);
    const auto compTheme = ::core::theme::componentTokensWeChat(t);

    ui.column("wallpaper.page")
        .size(screen.width, screen.height)
        // EUI padding 序 = (left, top, right, bottom)：页头贴顶（与 settings 同口径）。
        .padding(kPagePadX, 0.0f, kPagePadX, kPagePadBottom)
        .gap(kCardGap)
        .content([&] {
            drawHeader(ui, contentW, kind, t, state, actions);
            drawTabs(ui, contentW, t, state);

            const std::string contentKey =
                "wallpaper.t" + std::to_string(state.tab) + ".k" +
                std::to_string(static_cast<int>(kind)) + ".a" +
                std::to_string(static_cast<int>(snapshot.availability)) +
                (state.effectsExpanded ? ".e" : ".E") +
                (state.holdsExpanded ? ".h" : ".H") +
                (snapshot.scanning ? ".s" : ".S") +
                (snapshot.systemHolds != 0 ? ".r" : ".R") + ".f" +
                std::to_string(::core::theme::fontScale());

            components::scrollView(ui, "wallpaper.scroll")
                .size(contentW, viewportH)
                .gap(kCardGap)
                .step(60.0f)
                .theme(compTheme)
                .contentKey(contentKey)
                .offset(state.scrollOffset)
                .onChange([&state](float value) {
                    state.scrollOffset = value;
                })
                .content([&](eui::Ui& inner, float innerW, float viewportHeight) {
                    (void)viewportHeight;
                    if (state.tab == 1) {
                        drawLibraryPage(inner, innerW, snapshot, kind, t,
                                        actions);
                    } else {
                        drawPlayPage(inner, innerW, snapshot, kind, t, actions,
                                     state);
                    }
                })
                .build();
        })
        .build();
}

} // namespace module::wallpaper