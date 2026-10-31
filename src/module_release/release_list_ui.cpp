// 发布任务列表页（池 55 重设计）：单列 64px 行卡 / 模式筛选 Tab / 骨架与空态 /
// 分页。行卡与 Tab 全部自绘（EUI-NEO 无对应组件）；Tab 为设计定稿的下划线式
// 模式切换 Tab（池 55 方案卡 D1 裁决：与仓内内容域块面 wtab 不同类，wtab 不动）。
#include "release_list_ui.h"

#include "release_dialogs.h"  // 便捷版：页面 + 新建弹层一次画完
#include "release_logic.h"    // deriveStatus / taskProgress / nextStepLabel / planDateElapsedMinutes
#include "release_store.h"

#include <core/component_theme.h>
#include <core/logging.h>
#include <core/theme.h>
#include <core/time.h>

#include <components/progress.h>
#include <components/scrollview.h>
#include <components/theme.h>

#include <eui/types.h>

#include <ui_kit/button.h>
#include <ui_kit/metrics.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace module::release {

namespace {

// ---------- 日期（纯整数，无时区；与 release_logic 的判定口径一致） ----------

bool isLeapYear(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

bool parseIsoDate(const std::string& isoDate, int& year, int& month, int& day) {
    if (isoDate.size() != 10 || isoDate[4] != '-' || isoDate[7] != '-') return false;
    for (std::size_t i = 0; i < isoDate.size(); ++i) {
        if (i == 4 || i == 7) continue;
        const char c = isoDate[i];
        if (c < '0' || c > '9') return false;
    }
    year = (isoDate[0] - '0') * 1000 + (isoDate[1] - '0') * 100 +
           (isoDate[2] - '0') * 10 + (isoDate[3] - '0');
    month = (isoDate[5] - '0') * 10 + (isoDate[6] - '0');
    day = (isoDate[8] - '0') * 10 + (isoDate[9] - '0');
    if (month < 1 || month > 12 || day < 1) return false;
    static const int kDaysPerMonth[12] = {31, 28, 31, 30, 31, 30,
                                          31, 31, 30, 31, 30, 31};
    int maxDay = kDaysPerMonth[month - 1];
    if (month == 2 && isLeapYear(year)) maxDay = 29;
    return day <= maxDay;
}

// ---------- 视觉常量（池 55 设计 §3.2 / §5，8px 网格；色值一律走 theme token） ----------

constexpr float kInset = 16.0f;             // 卡内边距
constexpr float kPageMargin = 12.0f;        // 页面外边距
constexpr float kTabRowH = 44.0f;           // Tab 行（设计 §四）
constexpr float kShellH = ui_kit::kHeaderH + kTabRowH + 1.0f;  // 白壳含 1px 分割线
constexpr float kMainH = 24.0f;             // 主行高
constexpr float kSubH = 20.0f;              // 副行高
constexpr float kRowH = kReleaseTaskRowH;   // 64 = 10 + 24 + 20 + 10
constexpr float kChipH = 22.0f;             // 状态 chip（池 53 同值）
constexpr float kEnvChipH = 20.0f;          // 环境 chip（副行小一档）
constexpr float kDateW = 72.0f;             // 日期列（跨年 104）
constexpr float kDateWCrossYear = 104.0f;
constexpr float kWeekdayW = 40.0f;
constexpr float kProgW = 80.0f;             // 迷你进度条（池 53 §4.1 同值）
constexpr float kProgH = 4.0f;
constexpr float kCountW = 44.0f;            // n/n 计数右对齐定宽
constexpr float kFooterH = 40.0f;           // 追加加载/失败提示行
constexpr float kContentMaxW = 1040.0f;     // 内容列（池 53 §5.1 同值）
constexpr float kLoadThreshold = 24.0f;     // 触底判定余量
constexpr float kVoidedOpacity = 0.7f;      // 池 53 §七（现状 0.68 统一 0.7）
constexpr float kListCardRadius = 16.0f;    // 外层大卡特例档（废除 12px）

// chip 宽估算（字节级，略偏大只多留白、不裁字；与 todo 的估宽同思路）。
float chipWidth(const std::string& text, float fontSize) {
    float w = fontSize * 1.6f;
    for (unsigned char c : text) {
        w += (c >= 0x80) ? fontSize * 0.43f : fontSize * 0.56f;
    }
    return w;
}

struct ChipColors {
    eui::Color bg;
    eui::Color fg;
};

ChipColors statusChipColors(TaskStatus status, const core::theme::Tokens& t) {
    switch (status) {
        case TaskStatus::Draft:
            return {t.colors.hover, t.colors.textSecondary};
        case TaskStatus::Pending:
            return {eui::mixColor(t.colors.surface, t.colors.info, 0.12f), t.colors.info};
        case TaskStatus::Running:
            return {eui::mixColor(t.colors.surface, t.colors.warning, 0.14f), t.colors.warning};
        case TaskStatus::Done:
            return {eui::mixColor(t.colors.surface, t.colors.success, 0.12f), t.colors.success};
        case TaskStatus::Voided:
            return {t.colors.hover, t.colors.textDisabled};
    }
    return {t.colors.hover, t.colors.textSecondary};
}

void drawChip(eui::Ui& ui, const std::string& id, float x, float y, float w, float h,
              const std::string& text, const eui::Color& bg, const eui::Color& fg,
              const core::theme::Tokens& t, float opacity) {
    ui.rect(id + ".bg")
        .position(x, y)
        .size(w, h)
        .color(bg)
        .radius(t.sizes.radiusPill)
        .opacity(opacity)
        .build();
    ui.text(id + ".text")
        .position(x, y)
        .size(w, h)
        .text(text)
        .fontSize(t.sizes.fontSizeSmall - 1.0f)
        .lineHeight(h)
        .horizontalAlign(eui::HorizontalAlign::Center)
        // 文本盒 == 胶囊高，必须显式垂直居中，否则字从行框顶沿起排（chip 唯一绘制点）
        .verticalAlign(eui::VerticalAlign::Center)
        .color(fg)
        .fontWeight(600)
        .opacity(opacity)
        .build();
}

// ---------- 行卡（64px 双行；设计 §5.1/§5.2 状态矩阵） ----------

void drawTaskRow(eui::Ui& ui, const std::string& id, float x, float y, float w,
                 const ReleaseTaskCard& card, const core::theme::Tokens& t,
                 const ReleaseListActions& actions) {
    const float opacity = card.status == TaskStatus::Voided ? kVoidedOpacity : 1.0f;
    const ChipColors chip = statusChipColors(card.status, t);
    const bool regularDay = releaseIsRegularReleaseDay(card.planDate);
    const float dateW = card.dateWide ? kDateWCrossYear : kDateW;
    const std::string statusText = releaseTaskStatusText(card.status);
    const float chipW = chipWidth(statusText, std::max(1.0f, t.sizes.fontSizeSmall - 1.0f));
    const float chipX = std::max(0.0f, w - kInset - chipW);
    const float nextX = kInset + dateW + 8.0f + kWeekdayW + 16.0f;
    const float nextW = std::max(0.0f, chipX - 8.0f - nextX);

    ui.stack(id)
        .position(x, y)
        .ignoreLayout()
        .size(w, kRowH)
        .content([&] {
            const std::function<void(std::int64_t)> openTask = actions.openTask;
            // 行底常态透明（让分割线透出），hover/按压加深；分割线画在行下层，
            // hover 底色可覆盖分割线区域（设计 §5.3）。整行命中 → 打开详情。
            ui.rect(id + ".bg")
                .position(0.0f, 0.0f)
                .size(w, kRowH)
                .states(eui::Color{0.0f, 0.0f, 0.0f, 0.0f}, t.colors.borderMuted,
                        t.colors.active)
                .onClick([openTask, taskId = card.id] {
                    if (openTask) openTask(taskId);
                })
                .build();
            // Running 左缘 3px 全高绿竖条（扫视锚点；同视口允许多行，行是独立任务）
            if (card.status == TaskStatus::Running) {
                ui.rect(id + ".bar")
                    .position(0.0f, 0.0f)
                    .size(3.0f, kRowH)
                    .color(t.colors.primary)
                    .build();
            }

            // 主行：日期（跨年加宽）/ 星期（周二/四主色加深档）/ 下一步指引 / 状态 chip
            // 日期用 fontSizeLarge（18，旧列表同款；设计 17 档落在该 token 的近似档）
            ui.text(id + ".date")
                .position(kInset, 10.0f)
                .size(dateW, kMainH)
                .text(card.dateLabel)
                .fontSize(t.sizes.fontSizeLarge)
                .lineHeight(kMainH)
                .fontWeight(600)
                .color(t.colors.textPrimary)
                .opacity(opacity)
                .build();
            ui.text(id + ".wd")
                .position(kInset + dateW + 8.0f, 10.0f)
                .size(kWeekdayW, kMainH)
                .text(releaseWeekdayLabel(card.planDate))
                .fontSize(t.sizes.fontSizeSmall)
                .lineHeight(kMainH)
                .fontWeight(600)
                .color(regularDay ? t.colors.primaryText : t.colors.textTertiary)
                .opacity(opacity)
                .build();
            if (!card.nextStepLabel.empty()) {
                ui.text(id + ".next")
                    .position(nextX, 10.0f)
                    .size(nextW, kMainH)
                    .text("下一步 · " + card.nextStepLabel)
                    .fontSize(t.sizes.fontSizeBody)
                    .lineHeight(kMainH)
                    .maxWidth(nextW)
                    .wrap(false)
                    .overflow(core::TextOverflow::Ellipsis)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.textSecondary)
                    .opacity(opacity)
                    .build();
            }
            drawChip(ui, id + ".chip", chipX, 10.0f + (kMainH - kChipH) * 0.5f, chipW,
                     kChipH, statusText, chip.bg, chip.fg, t, opacity);

            // 副行：环境 chips（+N 收纳）+ 进度（Draft 整行不显示，设计 §5.2）
            const bool showProgress = card.status != TaskStatus::Draft;
            const float chipsRight = showProgress
                                         ? w - kInset - kCountW - 8.0f - kProgW - 16.0f
                                         : w - kInset;
            const float available = std::max(0.0f, chipsRight - kInset);
            const float moreW =
                chipWidth("+" + std::to_string(card.envNames.size()), 11.0f);
            float totalW = 0.0f;
            for (std::size_t i = 0; i < card.envNames.size(); ++i) {
                totalW += chipWidth(card.envNames[i], 11.0f) + (i == 0 ? 0.0f : 6.0f);
            }
            const bool overflow = totalW > available;
            const float budget =
                overflow ? std::max(0.0f, available - moreW - 6.0f) : available;
            float cx = 0.0f;
            std::size_t drawn = 0;
            for (std::size_t i = 0; i < card.envNames.size(); ++i) {
                const float cw = chipWidth(card.envNames[i], 11.0f);
                const float need = cw + (drawn == 0 ? 0.0f : 6.0f);
                if (cx + need > budget) break;
                drawChip(ui, id + ".env." + std::to_string(i), kInset + cx, 10.0f + kMainH,
                         cw, kEnvChipH, card.envNames[i], t.colors.hover,
                         t.colors.textSecondary, t, opacity);
                cx += need;
                ++drawn;
            }
            if (overflow && drawn < card.envNames.size()) {
                drawChip(ui, id + ".env.more", kInset + cx + 6.0f, 10.0f + kMainH, moreW,
                         kEnvChipH, "+" + std::to_string(card.envNames.size() - drawn),
                         t.colors.hover, t.colors.textTertiary, t, opacity);
            }

            if (showProgress) {
                const float ratio =
                    card.progressTotal > 0
                        ? std::clamp(static_cast<float>(card.progressDone) /
                                         static_cast<float>(card.progressTotal),
                                     0.0f, 1.0f)
                        : 0.0f;
                const auto componentTheme = core::theme::componentTokensWeChat(t);
                // 进度条（4px 档覆写组件默认）+ n/n 计数右对齐定宽（池 53 §4.1 规格）
                ui.stack(id + ".prog.wrap")
                    .position(w - kInset - kCountW - 8.0f - kProgW,
                              10.0f + kMainH + (kSubH - kProgH) * 0.5f)
                    .size(kProgW, kProgH)
                    .content([&] {
                        components::progress(ui, id + ".prog")
                            .size(kProgW, kProgH)
                            .value(ratio)
                            .theme(componentTheme)
                            .build();
                    })
                    .build();
                const std::string countText = std::to_string(card.progressDone) + "/" +
                                              std::to_string(card.progressTotal);
                ui.text(id + ".prog.count")
                    .position(w - kInset - kCountW, 10.0f + kMainH)
                    .size(kCountW, kSubH)
                    .text(countText)
                    .fontSize(t.sizes.fontSizeSmall - 1.0f)
                    .lineHeight(kSubH)
                    .horizontalAlign(eui::HorizontalAlign::Right)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.textSecondary)
                    .opacity(opacity)
                    .build();
            }
        })
        .build();
}

// ---------- Tab 行（模式筛选；设计 §四） ----------

void drawTabRow(eui::Ui& ui, float y, float screenW, int selected,
                const std::string& countText, const core::theme::Tokens& t,
                const std::function<void(int)>& onTab) {
    float x = kPageMargin;
    for (int i = 0; i < kReleaseListTabCount; ++i) {
        const char* label = releaseListTabLabel(i);
        const bool active = i == selected;
        const float tabW = chipWidth(label, t.sizes.fontSizeBody) + 8.0f;
        const std::string id = "release.tasks.tab." + std::to_string(i);
        ui.rect(id)
            .position(x, y)
            .size(tabW, kTabRowH)
            .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
            .onClick([onTab, i] {
                if (onTab) onTab(i);
            })
            .build();
        ui.text(id + ".label")
            .position(x, y)
            .size(tabW, kTabRowH)
            .text(label)
            .fontSize(t.sizes.fontSizeBody)
            .lineHeight(kTabRowH)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .verticalAlign(eui::VerticalAlign::Center)
            .fontWeight(active ? 600 : 400)
            .color(active ? t.colors.textPrimary : t.colors.textSecondary)
            .build();
        if (active) {
            // 底部 2px 绿下划线：150ms 位置/透明度过渡（设计 §七动效表）
            ui.rect("release.tasks.tab.indicator")
                .position(x, y + kTabRowH - 2.0f)
                .size(tabW, 2.0f)
                .color(t.colors.primary)
                .transition(core::Transition::make(0.15f, core::Ease::OutCubic))
                .animate(core::AnimProperty::Frame | core::AnimProperty::Color)
                .build();
        }
        x += tabW + 24.0f;
    }
    // 右端计数（随筛选联动；<1100px 隐藏）
    if (!countText.empty()) {
        ui.text("release.tasks.tabcount")
            .position(std::max(0.0f, screenW - kPageMargin - 200.0f), y)
            .size(200.0f, kTabRowH)
            .text(countText)
            .fontSize(t.sizes.fontSizeSmall)
            .lineHeight(kTabRowH)
            .horizontalAlign(eui::HorizontalAlign::Right)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.textTertiary)
            .build();
    }
}

// ---------- 页壳：页头 + Tab 行连体白壳，内容区灰底内滚 ----------

void composeListShell(eui::Ui& ui, const eui::Screen& screen,
                      const ReleaseListActions& actions, int selectedTab,
                      const std::string& countText,
                      const std::function<void(int)>& onTab,
                      const std::function<void(float, float, float, float)>& body) {
    const auto& t = core::theme::current();
    ui.stack("release.tasks.root")
        .size(screen.width, screen.height)
        .content([&] {
            // 连体白壳（页头 + Tab 行），分割线画在 Tab 行下
            ui.rect("release.tasks.shell")
                .position(0.0f, 0.0f)
                .size(screen.width, kShellH)
                .color(t.colors.surface)
                .build();
            ui.rect("release.tasks.bodybg")
                .position(0.0f, kShellH)
                .size(std::max(0.0f, screen.width), std::max(0.0f, screen.height - kShellH))
                .color(t.colors.background)
                .build();
            ui.text("release.tasks.title")
                .position(kPageMargin, 0.0f)
                .size(std::max(0.0f, screen.width - kPageMargin * 2.0f - 132.0f), ui_kit::kHeaderH)
                .text("上线任务")
                .fontSize(t.sizes.fontSizeTitle)
                .lineHeight(ui_kit::kHeaderH)
                .fontWeight(700)
                .color(t.colors.textPrimary)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
            ui_kit::ButtonOptions options;
            options.onClick = actions.openNewTask;
            ui_kit::button(ui, "release.tasks.new", "新建任务",
                           ui_kit::ButtonLevel::Primary,
                           std::max(0.0f, screen.width - kPageMargin - 120.0f),
                           (ui_kit::kHeaderH - ui_kit::kButtonHeight) * 0.5f, 120.0f,
                           ui_kit::kButtonHeight, t, options);
            drawTabRow(ui, ui_kit::kHeaderH, screen.width, selectedTab, countText, t,
                       onTab);
            ui.rect("release.tasks.tabline")
                .position(0.0f, kShellH - 1.0f)
                .size(screen.width, 1.0f)
                .color(t.colors.borderDefault)
                .build();

            const float contentY = kShellH + kPageMargin;
            const float bodyH = std::max(0.0f, screen.height - contentY - kPageMargin);
            const float availW = std::max(0.0f, screen.width - kPageMargin * 2.0f);
            const float contentW = std::min(kContentMaxW, availW);
            const float contentX = kPageMargin + (availW - contentW) * 0.5f;
            body(contentX, contentY, contentW, bodyH);
        })
        .build();
}

// 外层列表白卡（16px 大卡特例档，1px 边 + 极淡阴影）。
void drawListCardBox(eui::Ui& ui, const std::string& id, float x, float y, float w,
                     float h, const core::theme::Tokens& t) {
    const auto componentTheme = core::theme::componentTokensWeChat(t);
    ui.rect(id)
        .position(x, y)
        .size(std::max(0.0f, w), std::max(0.0f, h))
        .color(t.colors.surface)
        .border(1.0f, t.colors.borderDefault)
        .radius(kListCardRadius)
        .shadow(components::theme::shadow(componentTheme, 3.0f, 1.0f, 0.04f, 0.02f))
        .build();
}

// ---------- 状态区：骨架 / 全部空态 / 筛选空态 / 首屏失败（设计 §六） ----------

// 首屏加载：骨架行 ×3（64px 白行内两根灰条，行间分割线同真实列表）。
void drawSkeletonBody(eui::Ui& ui, float contentX, float contentY, float contentW,
                      float bodyH, const core::theme::Tokens& t) {
    const float boxH = 3.0f * kRowH;
    const float y = contentY + std::max(0.0f, (bodyH - boxH) * 0.5f);
    drawListCardBox(ui, "release.tasks.skeleton", contentX, y, contentW, boxH, t);
    for (int i = 0; i < 3; ++i) {
        const std::string id = "release.tasks.skeleton.row." + std::to_string(i);
        const float rowY = y + static_cast<float>(i) * kRowH;
        ui.rect(id + ".bar1")
            .position(contentX + kInset, rowY + 15.0f)
            .size(std::min(200.0f, std::max(0.0f, contentW - kInset * 2.0f)), 14.0f)
            .color(t.colors.borderMuted)
            .radius(4.0f)
            .build();
        ui.rect(id + ".bar2")
            .position(contentX + kInset, rowY + 37.0f)
            .size(std::min(120.0f, std::max(0.0f, contentW - kInset * 2.0f)), 12.0f)
            .color(t.colors.borderMuted)
            .radius(4.0f)
            .build();
        if (i > 0) {
            ui.rect(id + ".line")
                .position(contentX, rowY)
                .size(contentW, 1.0f)
                .color(t.colors.borderMuted)
                .build();
        }
    }
}

// 空态节点 id 保持 release.tasks.empty / release.tasks.empty.new（骨架期契约，
// test_release_basic 冒烟与既有用例按此定位）；文案与按钮形态按池 55 更新。
void drawAllEmptyBody(eui::Ui& ui, float contentX, float contentY, float contentW,
                      float bodyH, const core::theme::Tokens& t,
                      const ReleaseListActions& actions) {
    const float boxH = 240.0f;
    const float y = contentY + std::max(0.0f, (bodyH - boxH) * 0.5f);
    drawListCardBox(ui, "release.tasks.emptybox", contentX, y, contentW, boxH, t);
    ui.text("release.tasks.empty")
        .position(contentX, y + boxH * 0.5f - 30.0f)
        .size(contentW, 24.0f)
        .text("还没有上线任务")
        .fontSize(t.sizes.fontSizeBody)
        .lineHeight(24.0f)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textTertiary)
        .build();
    // 空态就近操作降级为次按钮：页头「新建任务」保持同视口唯一绿填充
    ui_kit::ButtonOptions options;
    options.onClick = actions.openNewTask;
    ui_kit::button(ui, "release.tasks.empty.new", "新建任务",
                   ui_kit::ButtonLevel::Secondary,
                   std::max(0.0f, contentX + (contentW - 120.0f) * 0.5f),
                   y + boxH * 0.5f + 2.0f, 120.0f, ui_kit::kButtonHeight, t, options);
}

// 筛选空态：句式随 Tab + 「查看全部」文字钮切回全部。
void drawFilterEmptyBody(eui::Ui& ui, float contentX, float contentY, float contentW,
                         float bodyH, int tab, const core::theme::Tokens& t,
                         const std::function<void()>& onBack) {
    const float boxH = 240.0f;
    const float y = contentY + std::max(0.0f, (bodyH - boxH) * 0.5f);
    drawListCardBox(ui, "release.tasks.filteremptybox", contentX, y, contentW, boxH, t);
    const std::string text =
        std::string("暂无") + releaseListTabLabel(tab) + "的任务";
    const float textW = chipWidth(text, t.sizes.fontSizeBody);
    const float btnW = chipWidth("查看全部", 13.0f) + 16.0f;
    const float totalW = textW + 8.0f + btnW;
    const float startX = contentX + std::max(0.0f, (contentW - totalW) * 0.5f);
    ui.text("release.tasks.filterempty")
        .position(startX, y + boxH * 0.5f - 12.0f)
        .size(textW, 24.0f)
        .text(text)
        .fontSize(t.sizes.fontSizeBody)
        .lineHeight(24.0f)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textTertiary)
        .build();
    ui.rect("release.tasks.filterempty.all")
        .position(startX + textW + 8.0f, y + boxH * 0.5f - 14.0f)
        .size(btnW, 28.0f)
        .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
        .onClick(onBack)
        .build();
    ui.text("release.tasks.filterempty.all.label")
        .position(startX + textW + 8.0f, y + boxH * 0.5f - 14.0f)
        .size(btnW, 28.0f)
        .text("查看全部")
        .fontSize(13.0f)
        .lineHeight(28.0f)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.primaryText)
        .build();
}

// 首屏失败且没有任何已加载内容：白卡内标题 + 原因一行 + 重试次按钮
//（不冒充「暂无任务」；池 53 §九 loadError 同款结构）。
void drawLoadErrorBody(eui::Ui& ui, float contentX, float contentY, float contentW,
                       float bodyH, const std::string& reason,
                       const core::theme::Tokens& t,
                       const std::function<void()>& retry) {
    const float boxH = 240.0f;
    const float y = contentY + std::max(0.0f, (bodyH - boxH) * 0.5f);
    drawListCardBox(ui, "release.tasks.errorbox", contentX, y, contentW, boxH, t);
    ui.text("release.tasks.load.failed")
        .position(contentX, y + 64.0f)
        .size(contentW, 24.0f)
        .text("无法加载任务列表")
        .fontSize(16.0f)
        .lineHeight(24.0f)
        .fontWeight(600)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textPrimary)
        .build();
    ui.text("release.tasks.load.reason")
        .position(contentX + kInset, y + 96.0f)
        .size(std::max(0.0f, contentW - kInset * 2.0f), 20.0f)
        .text(reason.empty() ? std::string("读取任务数据失败") : reason)
        .fontSize(t.sizes.fontSizeBody)
        .lineHeight(20.0f)
        .maxWidth(std::max(0.0f, contentW - kInset * 2.0f))
        .wrap(false)
        .overflow(core::TextOverflow::Ellipsis)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .color(t.colors.textSecondary)
        .build();
    ui_kit::ButtonOptions options;
    options.onClick = retry;
    ui_kit::button(ui, "release.tasks.load.retry", "重试",
                   ui_kit::ButtonLevel::Secondary,
                   std::max(0.0f, contentX + (contentW - 120.0f) * 0.5f), y + 140.0f,
                   120.0f, ui_kit::kButtonHeight, t, options);
}

// ---------- 列表体（单列定高；追加加载/失败 footer） ----------

struct ListPaging {
    std::string keyTag;  // 进 contentKey：Tab 切换等需重锚滚动的场景
    bool loading = false;
    bool failed = false;
    std::function<void()> requestLoad;  // 触底请求（去重由实现方负责）
    std::function<void()> advance;      // 帧泵
    std::function<void()> retry;        // 失败提示点击
};

void drawListBody(eui::Ui& ui, float contentX, float contentY, float contentW,
                  float bodyH, const std::vector<const ReleaseTaskCard*>& rows,
                  const ListPaging& paging, const core::theme::Tokens& t,
                  const ReleaseListActions& actions) {
    if (paging.advance) paging.advance();

    const float rowsH = static_cast<float>(rows.size()) * kRowH;
    const float footerH = (paging.loading || paging.failed) ? kFooterH : 0.0f;
    const float contentH = rowsH + footerH;
    const std::function<void()> requestLoad = paging.requestLoad;

    ui.stack("release.tasks.body")
        .position(contentX, contentY)
        .size(contentW, bodyH)
        .content([&] {
            components::scrollView(ui, "release.tasks.scroll")
                .position(0.0f, 0.0f)
                .size(contentW, bodyH)
                .step(60.0f)
                .theme(core::theme::componentTokensWeChat(t))
                .scrollbarOverlay()
                .contentKey("release.tasks.scroll." + paging.keyTag + "." +
                            std::to_string(rows.size()) + "." +
                            (paging.loading ? "1" : "0") + (paging.failed ? "1" : "0"))
                .onChange([requestLoad, contentH, bodyH](float offset) {
                    // 反重构警告：这里只管「是否到内容底」；去重（在飞/失败待重试/
                    // 数据耗尽）由 requestLoad 的实现方负责，去掉会让内容增长前的
                    // 重复滚动回调连续追加两页。
                    if (!requestLoad) return;
                    if (offset + bodyH >= contentH - kLoadThreshold) requestLoad();
                })
                .content([&](eui::Ui& inner, float innerW, float innerVh) {
                    (void)innerVh;
                    // 唯一参与流式布局的子元素：撑出内容总高（行全部绝对定位）
                    inner.stack("release.tasks.spacer").size(innerW, contentH).build();
                    drawListCardBox(inner, "release.tasks.cardbox", 0.0f, 0.0f, innerW,
                                    std::max(contentH, kRowH), t);
                    // 分割线画在行下层：行 hover 底色覆盖分割线区域（设计 §5.3）
                    for (std::size_t i = 1; i < rows.size(); ++i) {
                        inner.rect("release.tasks.divider." + std::to_string(i))
                            .position(0.0f, static_cast<float>(i) * kRowH - 0.5f)
                            .size(innerW, 1.0f)
                            .color(t.colors.borderMuted)
                            .build();
                    }
                    for (std::size_t i = 0; i < rows.size(); ++i) {
                        drawTaskRow(inner,
                                    "release.tasks.card." + std::to_string(rows[i]->id),
                                    0.0f, static_cast<float>(i) * kRowH, innerW,
                                    *rows[i], t, actions);
                    }
                    const float footerY = rowsH;
                    if (paging.loading) {
                        // 一行「加载中…」，不冒充数据行（设计 §六）
                        inner.text("release.tasks.loading")
                            .position(0.0f, footerY)
                            .size(innerW, kFooterH)
                            .text("加载中…")
                            .fontSize(t.sizes.fontSizeSmall)
                            .lineHeight(kFooterH)
                            .horizontalAlign(eui::HorizontalAlign::Center)
                            .verticalAlign(eui::VerticalAlign::Center)
                            .color(t.colors.textTertiary)
                            .build();
                    } else if (paging.failed) {
                        // 「加载失败」红字（告警）+ 8px「重试」绿字钮（操作）；
                        // 已加载内容保留（失败不清空契约）
                        const std::string failText = "加载失败";
                        const float failW = chipWidth(failText, 12.0f);
                        const float btnW = chipWidth("重试", 13.0f) + 16.0f;
                        const float startX =
                            std::max(0.0f, (innerW - failW - 8.0f - btnW) * 0.5f);
                        inner.text("release.tasks.load.failed")
                            .position(startX, footerY)
                            .size(failW, kFooterH)
                            .text(failText)
                            .fontSize(12.0f)
                            .lineHeight(kFooterH)
                            .verticalAlign(eui::VerticalAlign::Center)
                            .color(t.colors.errorText)
                            .build();
                        inner.rect("release.tasks.load.retry")
                            .position(startX + failW + 8.0f, footerY + 6.0f)
                            .size(btnW, 28.0f)
                            .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
                            .onClick(paging.retry)
                            .build();
                        inner.text("release.tasks.load.retry.label")
                            .position(startX + failW + 8.0f, footerY + 6.0f)
                            .size(btnW, 28.0f)
                            .text("重试")
                            .fontSize(13.0f)
                            .lineHeight(28.0f)
                            .horizontalAlign(eui::HorizontalAlign::Center)
                            .verticalAlign(eui::VerticalAlign::Center)
                            .color(t.colors.primaryText)
                            .build();
                    }
                })
                .build();

            // 滚动回调不自动重 compose（scrollView 未透传该开关）：
            // 触底置加载态后必须有一帧把占位画出来，这里按元素契约直接置位。
            if (core::dsl::Element* scroll = ui.find("release.tasks.scroll")) {
                scroll->composeOnScrollOffsetChange = true;
            }
            if (paging.loading) {
                // 帧泵（照 todo.archive.anim.driver）：追加倒计时由每帧 compose 推进
                ui.stack("release.tasks.load.pump").size(1.0f, 1.0f)
                    .onFrame([](float) {})
                    .build();
            }
        })
        .build();
}

// ---------- 页面级状态（跨帧存活） ----------

// 便捷版（卡片已全部预载）的分页状态。
struct ReleaseListState {
    ReleaseTaskPager pager;
};

// store 版列表缓存（跨帧存活）：
//   cards      已加载的卡片（首屏 10 条起步，触底按 offset=cards.size() 追加）
//   total      countTasks() 的总数，决定「是否还有更多」
//   loading    请求在飞（帧泵；首屏 1 帧延迟装填让骨架可见——池 55 D4 裁决）
//   failed     上一次加载失败：保留 cards，显示可重试提示，不自动重试
//   loaded     首屏是否已成功加载（失败时 false，重试后才会再取）
//   tab        当前筛选 Tab（ReleaseListTab 的 int 值）
//   groupCounts 各 Tab 组计数（[0]=全部=total；countTasksByStatuses 库值口径）
// compose 每帧都会调用，禁止每帧全量查库。
struct ReleaseListCache {
    std::vector<ReleaseTaskCard> cards;
    std::size_t total = 0;
    std::int64_t loadedAtMs = 0;
    bool loaded = false;
    bool loading = false;
    int framesLeft = 0;
    bool failed = false;
    bool dialogWasOpen = false;
    int tab = 0;
    int groupCounts[kReleaseListTabCount] = {0, 0, 0, 0};
    bool countsValid = false;
    std::string lastError;
};

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

// ---------- 纯逻辑导出 ----------

std::string releaseTaskStatusText(TaskStatus status) {
    switch (status) {
        case TaskStatus::Draft: return "草稿";
        case TaskStatus::Pending: return "待执行";
        case TaskStatus::Running: return "执行中";
        case TaskStatus::Done: return "已完成";
        case TaskStatus::Voided: return "已作废";
    }
    return "草稿";
}

bool releaseIsIsoDate(const std::string& isoDate) {
    int year = 0;
    int month = 0;
    int day = 0;
    return parseIsoDate(isoDate, year, month, day);
}

int releaseWeekdayIndex(const std::string& isoDate) {
    int year = 0;
    int month = 0;
    int day = 0;
    if (!parseIsoDate(isoDate, year, month, day)) return -1;
    // Zeller 同余（Gregorian；0 = 周日）
    int y = year;
    int m = month;
    if (m < 3) {
        m += 12;
        --y;
    }
    const int k = y % 100;
    const int j = y / 100;
    const int h = (day + (13 * (m + 1)) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;
    return (h + 6) % 7;  // Zeller: 0=周六 → 本接口 0=周日
}

std::string releaseWeekdayLabel(const std::string& isoDate) {
    static const char* kLabels[7] = {"周日", "周一", "周二", "周三",
                                     "周四", "周五", "周六"};
    const int index = releaseWeekdayIndex(isoDate);
    if (index < 0 || index > 6) return {};
    return kLabels[index];
}

bool releaseIsRegularReleaseDay(const std::string& isoDate) {
    return isRegularReleaseDay(isoDate);
}

const char* releaseListTabLabel(int tab) {
    switch (tab) {
        case 0: return "全部";
        case 1: return "发布中";
        case 2: return "准备中";
        case 3: return "已归档";
    }
    return "";
}

ReleaseListTab releaseListGroupOf(TaskStatus status) {
    switch (status) {
        case TaskStatus::Pending:
        case TaskStatus::Running:
            return ReleaseListTab::Publishing;
        case TaskStatus::Draft:
            return ReleaseListTab::Preparing;
        case TaskStatus::Done:
        case TaskStatus::Voided:
            return ReleaseListTab::Archived;
    }
    return ReleaseListTab::Preparing;
}

ReleaseTaskPager::ReleaseTaskPager(std::size_t pageSize)
    : pageSize_(pageSize == 0 ? kReleaseTaskPageSize : pageSize) {}

void ReleaseTaskPager::reset(std::size_t total) {
    total_ = total;
    visible_ = std::min(pageSize_, total_);
    loading_ = false;
    framesLeft_ = 0;
}

void ReleaseTaskPager::setTotal(std::size_t total) {
    if (total != total_) reset(total);
}

bool ReleaseTaskPager::loadMore() {
    if (!hasMore()) return false;
    visible_ = std::min(visible_ + pageSize_, total_);
    return true;
}

bool ReleaseTaskPager::requestLoad() {
    if (!hasMore() || loading_) return false;
    loading_ = true;
    framesLeft_ = 6;  // ≈100ms：让「加载中…」占位可被看见，而不是同帧被新数据顶掉
    return true;
}

bool ReleaseTaskPager::advanceLoad() {
    if (!loading_) return false;
    --framesLeft_;
    if (framesLeft_ > 0) return false;
    loading_ = false;
    framesLeft_ = 0;
    return loadMore();
}

// ---------- 装配 ----------

core::Result<std::vector<ReleaseTaskCard>> releaseLoadTaskCards(ReleaseStore& store,
                                                                int offset, int limit) {
    // 分页下推给数据层（SQL LIMIT/OFFSET）；这里只为这一页的任务装配卡片视图，
    // 不在内存里截取全量列表。
    auto tasks = store.listTasksPage(offset, limit);
    if (tasks.isFail()) {
        return core::Result<std::vector<ReleaseTaskCard>>::fail(
            tasks.errorCode(), tasks.errorMessage());
    }

    std::vector<ReleaseTaskCard> cards;
    cards.reserve(tasks.value().size());
    // 整页共用一个时间快照：状态 chip / 下一步指引全部按同一时刻推导，不随逐卡装配漂移。
    const std::int64_t nowUnix = static_cast<std::int64_t>(core::time::nowUnix());
    std::tm nowTm{};
    core::time::localTime(static_cast<std::time_t>(nowUnix), &nowTm);
    const int currentYear = nowTm.tm_year + 1900;
    for (const Task& task : tasks.value()) {
        ReleaseTaskCard card;
        card.id = task.id;
        card.planDate = task.planDate;

        auto envs = store.listTaskEnvs(task.id);
        if (envs.isFail()) {
            return core::Result<std::vector<ReleaseTaskCard>>::fail(
                envs.errorCode(), envs.errorMessage());
        }
        // 每任务 4 条 SQL 批量取全（环境 1 + 服务/配置/步骤各 1），再按外键在内存
        // 归组——替换逐环境逐服务的 N+1 循环。归组后各组内顺序与旧逐条查询一致
        // （store 侧 ORDER BY 沿用各 listTask* 的口径），nextStepLabel 的固定序
        // 消费同一份顺序（D3 裁决：库序）。
        auto allServices = store.listTaskServicesByTask(task.id);
        if (allServices.isFail()) {
            return core::Result<std::vector<ReleaseTaskCard>>::fail(
                allServices.errorCode(), allServices.errorMessage());
        }
        auto allConfigs = store.listTaskConfigsByTask(task.id);
        if (allConfigs.isFail()) {
            return core::Result<std::vector<ReleaseTaskCard>>::fail(
                allConfigs.errorCode(), allConfigs.errorMessage());
        }
        auto allSteps = store.listTaskStepsByTask(task.id);
        if (allSteps.isFail()) {
            return core::Result<std::vector<ReleaseTaskCard>>::fail(
                allSteps.errorCode(), allSteps.errorMessage());
        }
        std::vector<TaskEnv> liveEnvs;
        std::vector<TaskService> liveServices;
        std::vector<TaskConfig> configs;
        std::vector<TaskStep> steps;
        for (const TaskEnv& env : envs.value()) {
            if (env.removed) continue;
            liveEnvs.push_back(env);

            for (const TaskService& service : allServices.value()) {
                if (service.taskEnvId != env.id) continue;
                if (service.removed) continue;
                liveServices.push_back(service);
                for (const TaskConfig& config : allConfigs.value()) {
                    if (config.taskServiceId != service.id) continue;
                    configs.push_back(config);
                }
                for (const TaskStep& step : allSteps.value()) {
                    if (step.taskServiceId != service.id) continue;
                    steps.push_back(step);
                }
            }
        }
        card.envNames.reserve(liveEnvs.size());
        for (const TaskEnv& env : liveEnvs) card.envNames.push_back(env.envName);

        // 进度与执行页同源：固定步骤（合并/部署/验证）+ 未跳过配置
        const Progress progress =
            taskProgress(task, liveEnvs, liveServices, configs, steps);
        card.progressDone = progress.done;
        card.progressTotal = progress.total;
        // 状态 chip 只按时间推导（spec §7 状态只跟时间走），不写库：到了时间窗还
        // 显示「待执行」、过了时间窗还显示「执行中」的库值旧状态不再回流到列表。
        card.status = deriveStatus(task, liveEnvs, nowUnix);

        // 日期短标「M/D」；跨年「YYYY/M/D」（列宽 72→104 自适应）
        int py = 0;
        int pm = 0;
        int pd = 0;
        if (parseIsoDate(task.planDate, py, pm, pd)) {
            card.dateWide = py != currentYear;
            card.dateLabel = card.dateWide ? std::to_string(py) + "/" +
                                                 std::to_string(pm) + "/" +
                                                 std::to_string(pd)
                                           : std::to_string(pm) + "/" + std::to_string(pd);
        } else {
            card.dateLabel = task.planDate;
            card.dateWide = true;  // 非法日期原样展示并按宽档防溢出
        }

        // 「下一步 · {步名}」：与详情页页头主动作同一信息源（release_logic::
        // nextStepLabel）；仅发布态装配，Draft/Done/Voided 留白。零新增 SQL——
        // 消费上面已批量取回的内存向量，walk 首中即停。
        if (card.status == TaskStatus::Pending || card.status == TaskStatus::Running) {
            std::int64_t minute64 = 0;
            if (planDateElapsedMinutes(task.planDate, nowUnix, minute64)) {
                const int nowMinute = static_cast<int>(
                    std::clamp(minute64, static_cast<std::int64_t>(INT32_MIN),
                               static_cast<std::int64_t>(INT32_MAX)));
                card.nextStepLabel =
                    nextStepLabel(task, liveEnvs, liveServices, configs, steps, nowMinute);
            }
        }

        cards.push_back(std::move(card));
    }

    return core::Result<std::vector<ReleaseTaskCard>>::ok(std::move(cards));
}

// ---------- 页面组合 ----------

namespace {

// 过滤后的可见行（Tab=状态分组，内存过滤；store 分页契约不变）。
std::vector<const ReleaseTaskCard*> filterRows(const std::vector<ReleaseTaskCard>& cards,
                                               int tab) {
    std::vector<const ReleaseTaskCard*> rows;
    rows.reserve(cards.size());
    for (const ReleaseTaskCard& card : cards) {
        if (tab == 0 ||
            releaseListGroupOf(card.status) == static_cast<ReleaseListTab>(tab)) {
            rows.push_back(&card);
        }
    }
    return rows;
}

std::string tabCountText(const ReleaseListCache& cache) {
    if (!cache.countsValid) return {};
    if (cache.tab == 0) return "共 " + std::to_string(cache.total) + " 个任务";
    return std::string(releaseListTabLabel(cache.tab)) + " · " +
           std::to_string(cache.groupCounts[cache.tab]) + " 个";
}

} // namespace

void composeReleaseListPage(eui::Ui& ui, const eui::Screen& screen,
                            const std::vector<ReleaseTaskCard>& cards,
                            const ReleaseListActions& actions) {
    const auto& t = core::theme::current();
    if (cards.empty()) {
        // 空列表也要把分页状态归零：否则「25 条 → 清空 → 25 条」会带着旧可见数
        ui.state<ReleaseListState>("release.tasks.list.state").pager.reset(0);
        composeListShell(ui, screen, actions, 0, {}, {}, [&](float cx, float cy, float cw,
                                                             float bh) {
            drawAllEmptyBody(ui, cx, cy, cw, bh, t, actions);
        });
        return;
    }

    // 内存分页（卡片已全部预载）：pager 决定可见数，触底续载只改可见数。
    ReleaseListState& st = ui.state<ReleaseListState>("release.tasks.list.state");
    st.pager.setTotal(cards.size());
    composeListShell(ui, screen, actions, 0, {}, {}, [&](float cx, float cy, float cw,
                                                         float bh) {
        ListPaging paging;
        paging.keyTag = "cards";
        paging.loading = st.pager.loading();
        paging.requestLoad = [&st] {
            if (st.pager.loading() || !st.pager.hasMore()) return;
            st.pager.requestLoad();
        };
        paging.advance = [&st] { st.pager.advanceLoad(); };

        const std::size_t visible =
            std::min(st.pager.visible(), cards.size());
        std::vector<const ReleaseTaskCard*> rows;
        rows.reserve(visible);
        for (std::size_t i = 0; i < visible && i < cards.size(); ++i) {
            rows.push_back(&cards[i]);
        }
        drawListBody(ui, cx, cy, cw, bh, rows, paging, t, actions);
    });
}

void composeReleaseListPage(eui::Ui& ui, const eui::Screen& screen,
                            ReleaseStore& store,
                            ReleaseNewTaskDialogState& newTaskState,
                            const ReleaseListActions& actions) {
    ReleaseListCache& cache = ui.state<ReleaseListCache>("release.tasks.cache");

    // 总数 + 三组状态组计数（Tab 右端计数）；total 失败按失败路径走，
    // 组计数失败只隐藏计数位（countsValid=false），不阻断列表。
    const auto loadCounts = [&cache, &store]() -> bool {
        auto count = store.countTasks();
        if (count.isFail()) {
            LOG_ERROR("release: 读取任务总数失败：" + count.errorMessage());
            cache.lastError = count.errorMessage();
            cache.countsValid = false;
            cache.failed = true;
            return false;
        }
        cache.total = static_cast<std::size_t>(count.value());
        static const std::vector<std::vector<TaskStatus>> kGroups = {
            {TaskStatus::Pending, TaskStatus::Running},
            {TaskStatus::Draft},
            {TaskStatus::Done, TaskStatus::Voided},
        };
        cache.countsValid = true;
        for (int g = 0; g < 3; ++g) {
            auto c = store.countTasksByStatuses(kGroups[static_cast<std::size_t>(g)]);
            if (c.isFail()) {
                LOG_ERROR("release: 读取任务组计数失败：" + c.errorMessage());
                cache.countsValid = false;
                break;
            }
            cache.groupCounts[static_cast<std::size_t>(g) + 1] = c.value();
        }
        cache.groupCounts[0] = static_cast<int>(cache.total);
        return true;
    };

    // replace=true：以本页整体替换已加载内容（首屏/重锚）；false：追加下一页。
    // 失败一律保留 cache.cards（不清空），置 failed 让界面显示可重试提示。
    const auto loadPage = [&cache, &store](int offset, int limit, bool replace) -> bool {
        auto page = releaseLoadTaskCards(store, offset, limit);
        if (page.isFail()) {
            LOG_ERROR("release: 读取任务列表失败：" + page.errorMessage());
            cache.lastError = page.errorMessage();
            cache.failed = true;
            return false;
        }
        if (replace) {
            cache.cards = std::move(page.value());
        } else {
            cache.cards.insert(cache.cards.end(), page.value().begin(), page.value().end());
        }
        cache.failed = false;
        cache.loadedAtMs = nowMs();
        return true;
    };

    const auto loadFirstPage = [&]() -> bool {
        if (!loadCounts()) return false;
        return loadPage(0, static_cast<int>(kReleaseTaskPageSize), true);
    };

    // 下一页 offset = 已加载数；已加载内容原样保留（失败也不清空）。
    const auto appendNextPage = [&cache, &store]() {
        auto page = releaseLoadTaskCards(store, static_cast<int>(cache.cards.size()),
                                         static_cast<int>(kReleaseTaskPageSize));
        if (page.isFail()) {
            LOG_ERROR("release: 加载下一页任务失败：" + page.errorMessage());
            cache.lastError = page.errorMessage();
            cache.failed = true;
            return;
        }
        cache.cards.insert(cache.cards.end(), page.value().begin(), page.value().end());

        // 总数以 countTasks 为准（翻页期间可能有新建/作废）；只更新总数，不重排
        // 已加载页。若取回为空但总数更大（口径漂移），把总数收敛到已加载数，
        // 防止永远触底重取。
        auto count = store.countTasks();
        if (count.isFail()) {
            LOG_ERROR("release: 读取任务总数失败：" + count.errorMessage());
        } else {
            cache.total = static_cast<std::size_t>(count.value());
            cache.groupCounts[0] = static_cast<int>(cache.total);
        }
        if (page.value().empty() && cache.cards.size() < cache.total) {
            cache.total = cache.cards.size();
            cache.groupCounts[0] = static_cast<int>(cache.total);
        }
        cache.loadedAtMs = nowMs();
    };

    // 帧泵：首屏 1 帧延迟装填（本帧画骨架，下一帧装数据——D4 裁决）；追加 6 帧。
    const auto pump = [&]() {
        if (!cache.loading) return;
        if (--cache.framesLeft > 0) return;
        cache.loading = false;
        cache.framesLeft = 0;
        if (!cache.loaded) {
            cache.loaded = loadFirstPage();
        } else {
            appendNextPage();
        }
    };

    const bool dialogClosed = cache.dialogWasOpen && !newTaskState.open;
    cache.dialogWasOpen = newTaskState.open;
    const std::int64_t now = nowMs();

    if (dialogClosed) {
        // 新建/复用任务后总数与顺序都可能变：数量变了就从首屏重锚（offset 分页
        // 在插入后必须重锚）；只是取消弹层（数量不变）则原地刷新已加载页，
        // 不把用户翻到的页塌回首屏。
        cache.failed = false;
        cache.loading = false;
        cache.framesLeft = 0;
        const std::size_t before = cache.total;
        if (loadCounts()) {
            if (cache.cards.empty() || cache.total != before) {
                cache.loaded = loadPage(0, static_cast<int>(kReleaseTaskPageSize), true);
            } else {
                cache.loaded = loadPage(0, static_cast<int>(cache.cards.size()), true);
            }
        }
    } else if (!cache.loaded) {
        // 首屏：仅在未失败时进入「骨架帧」（下一帧泵装填）；失败后等用户点
        // 「重试」，不每帧重打库。framesLeft=2：本帧画骨架，下一帧泵装填（D4）。
        if (!cache.failed && !cache.loading) {
            cache.loading = true;
            cache.framesLeft = 2;
        }
    } else if (!cache.failed && !cache.loading && now - cache.loadedAtMs >= 1000) {
        // 陈旧刷新：总数变了就重锚首屏；否则只重取已加载条数（下限一页），
        // 不把已翻出的页塌回首屏。
        const std::size_t before = cache.total;
        if (loadCounts()) {
            if (cache.total != before) {
                loadPage(0, static_cast<int>(kReleaseTaskPageSize), true);
            } else if (!cache.cards.empty()) {
                loadPage(0, static_cast<int>(cache.cards.size()), true);
            }
        }
        // 即使总数未变、没有重取某页（如空库），也要推后下一次刷新，
        // 否则此分支会逐帧查 countTasks。
        cache.loadedAtMs = now;
    }
    pump();

    // 「新建任务」未注入时默认打开本页弹层；注入自定义回调（如先切页）则以调用方为准
    ReleaseListActions effective = actions;
    if (!effective.openNewTask) {
        effective.openNewTask = [&newTaskState] { newTaskState.open = true; };
    }

    const auto onTab = [&cache](int tab) { cache.tab = tab; };
    const std::string countText = tabCountText(cache);

    if (!cache.cards.empty() || cache.loaded) {
        // 常规：有内容画行卡列表（内存按 Tab 过滤）；无内容且首屏加载成功画空态。
        composeListShell(ui, screen, effective, cache.tab, countText, onTab,
                         [&](float cx, float cy, float cw, float bh) {
                             const auto& th = core::theme::current();
                             std::vector<const ReleaseTaskCard*> rows =
                                 filterRows(cache.cards, cache.tab);
                             ListPaging paging;
                             paging.keyTag = std::to_string(cache.tab);
                             paging.loading = cache.loading;
                             paging.failed = cache.failed;
                             paging.requestLoad = [&cache] {
                                 // 去重三闸：失败待重试 / 已有请求在飞 / 已到总数。
                                 if (cache.failed || cache.loading) return;
                                 if (cache.cards.size() >= cache.total) return;
                                 cache.loading = true;
                                 cache.framesLeft = 6;  // ≈100ms：让「加载中…」占位可见
                             };
                             paging.retry = [&cache] {
                                 cache.failed = false;
                                 if (cache.cards.empty()) {
                                     cache.loaded = false;  // 无内容：走骨架帧重取首屏
                                     return;
                                 }
                                 cache.loaded = true;  // 有内容：保留已加载页继续取
                                 cache.loading = true;
                                 cache.framesLeft = 6;
                             };
                             // 矮窗填满：过滤后内容不足一屏且还有更多 → 请求下一页
                             //（没有这一步，矮窗里内容撑不出滚动条、触底回调永不触发）。
                             // 必须先于空态判空：匹配行可能整页落在未加载区，先判空
                             // 会把「发布中·N 个 + 暂无…」的矛盾呈现钉死在屏上，自动
                             // 补页与触底追加双双不可达（池 55 BUG-1）。
                             const float rowsH =
                                 static_cast<float>(rows.size()) * kRowH;
                             if (!cache.loading && !cache.failed &&
                                 cache.cards.size() < cache.total &&
                                 rowsH < bh - kLoadThreshold) {
                                 paging.requestLoad();
                             }
                             if (rows.empty()) {
                                 if (cache.tab != 0) {
                                     // 筛选空态：组计数为 0 或已耗尽仍无匹配行
                                     drawFilterEmptyBody(ui, cx, cy, cw, bh, cache.tab, th,
                                                         [&cache] { cache.tab = 0; });
                                 } else {
                                     drawAllEmptyBody(ui, cx, cy, cw, bh, th, effective);
                                 }
                                 return;
                             }
                             drawListBody(ui, cx, cy, cw, bh, rows, paging, th, effective);
                         });
    } else if (cache.failed) {
        // 首屏失败且没有任何已加载内容：页头保留，主体给可重试提示（不冒充空态）
        composeListShell(ui, screen, effective, cache.tab, countText, onTab,
                         [&](float cx, float cy, float cw, float bh) {
                             drawLoadErrorBody(ui, cx, cy, cw, bh, cache.lastError,
                                               core::theme::current(), [&cache] {
                                                   cache.failed = false;
                                                   cache.loaded = false;  // 走骨架帧重取
                                               });
                         });
    } else {
        // 首屏骨架帧（D4：本帧画骨架，下一帧泵装填）
        composeListShell(ui, screen, effective, cache.tab, countText, onTab,
                         [&](float cx, float cy, float cw, float bh) {
                             drawSkeletonBody(ui, cx, cy, cw, bh, core::theme::current());
                         });
    }

    // 弹层独立于列表绘制：新建/复用成功的跳转由 actions.openTask 注入
    drawNewTaskDialog(ui, screen, store, newTaskState, effective.openTask);
}

void composeReleaseListPage(eui::Ui& ui, const eui::Screen& screen) {
    const auto& t = core::theme::current();
    const float pad = t.sizes.spacingLarge * 2.0f;
    ui.stack("release.tasks.root")
        .size(screen.width, screen.height)
        .content([&] {
            ui.text("release.tasks.title")
                .position(pad, pad)
                .size(std::max(0.0f, screen.width - pad * 2.0f), 32.0f)
                .text("任务")
                .fontSize(t.sizes.fontSizeTitle)
                .fontWeight(600)
                .color(t.colors.textPrimary)
                .build();
            ui.text("release.tasks.empty")
                .position(pad, pad + 48.0f)
                .size(std::max(0.0f, screen.width - pad * 2.0f), 24.0f)
                .text("暂无数据")
                .fontSize(t.sizes.fontSizeBody)
                .color(t.colors.textTertiary)
                .build();
        })
        .build();
}

} // namespace module::release
