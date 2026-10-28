// 任务列表（票据 05）实现：瀑布流分列 / 分页 / 卡片与加载占位全部自绘
//（EUI-NEO 无 chip / skeleton 组件；分列按视觉规范自算，不依赖 virtualMasonry）。
#include "release_list_ui.h"

#include "release_dialogs.h"  // 便捷版：页面 + 新建弹层一次画完
#include "release_logic.h"    // isRegularReleaseDay / taskProgress：单一逻辑源
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

// ---------- 卡片几何 ----------

constexpr float kCardInset = 16.0f;
constexpr float kCardRadius = 12.0f;
constexpr float kCardTitleLineH = 24.0f;   // 日期行
constexpr float kCardRowGap = 10.0f;
constexpr float kChipH = 22.0f;            // 视觉规范：chip 22
constexpr float kProgressRowH = 20.0f;     // 4px 进度条 + 右侧计数文字所在行
constexpr float kProgressH = 4.0f;
constexpr float kEnvRowH = kChipH;
constexpr float kSummaryRowH = 20.0f;
constexpr float kLoadingCardH = 84.0f;
constexpr float kLoadThreshold = 24.0f;    // 触底判定余量

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

void drawChip(eui::Ui& ui, const std::string& id, float x, float y, float w,
              const std::string& text, const eui::Color& bg, const eui::Color& fg,
              const core::theme::Tokens& t, float opacity) {
    ui.rect(id + ".bg")
        .position(x, y)
        .size(w, kChipH)
        .color(bg)
        .radius(t.sizes.radiusPill)
        .opacity(opacity)
        .build();
    ui.text(id + ".text")
        .position(x, y)
        .size(w, kChipH)
        .text(text)
        .fontSize(t.sizes.fontSizeSmall - 1.0f)
        .lineHeight(kChipH)
        .horizontalAlign(eui::HorizontalAlign::Center)
        // 文本盒 == 胶囊高，必须显式垂直居中，否则字从行框顶沿起排（chip 唯一绘制点）
        .verticalAlign(eui::VerticalAlign::Center)
        .color(fg)
        .fontWeight(600)
        .opacity(opacity)
        .build();
}

struct CardLayout {
    float height = 0.0f;
    float chipW = 0.0f;
    float chipX = 0.0f;
    float progressY = 0.0f;
    float progressW = 0.0f;
    float envY = 0.0f;
    float summaryY = 0.0f;
    bool hasEnvRow = false;
    bool hasSummary = false;
};

CardLayout cardLayout(const ReleaseTaskCard& card, float cardW) {
    CardLayout layout;
    layout.chipW = chipWidth(releaseTaskStatusText(card.status),
                             std::max(1.0f, core::theme::current().sizes.fontSizeSmall - 1.0f));
    layout.chipX = std::max(kCardInset, cardW - kCardInset - layout.chipW);
    layout.progressY = kCardInset + kCardTitleLineH + kCardRowGap;
    // 右侧计数文字 44 宽 + 8 间距；极窄卡钳到 0，不产生负宽
    layout.progressW = std::max(0.0f, cardW - kCardInset * 2.0f - 52.0f);
    layout.hasEnvRow = !card.envNames.empty();
    layout.hasSummary = !card.summary.empty();
    float y = layout.progressY + kProgressRowH;
    if (layout.hasEnvRow) {
        y += kCardRowGap;
        layout.envY = y;
        y += kEnvRowH;
    }
    if (layout.hasSummary) {
        y += kCardRowGap;
        layout.summaryY = y;
        y += kSummaryRowH;
    }
    layout.height = y + kCardInset;
    return layout;
}

void drawTaskCard(eui::Ui& ui, const std::string& id, float x, float y, float w,
                  const ReleaseTaskCard& card, const core::theme::Tokens& t,
                  const ReleaseListActions& actions) {
    const CardLayout layout = cardLayout(card, w);
    const float h = layout.height;
    const float opacity = card.status == TaskStatus::Voided ? 0.68f : 1.0f;
    const auto componentTheme = core::theme::componentTokensWeChat(t);
    const ChipColors chip = statusChipColors(card.status, t);
    const bool regularDay = releaseIsRegularReleaseDay(card.planDate);

    ui.stack(id)
        .position(x, y)
        .ignoreLayout()
        .size(w, h)
        .content([&] {
            const std::function<void(std::int64_t)> openTask = actions.openTask;
            ui.rect(id + ".bg")
                .position(0.0f, 0.0f)
                .size(w, h)
                .color(t.colors.surface)
                .border(1.0f, t.colors.borderDefault)
                .radius(kCardRadius)
                .shadow(components::theme::shadow(componentTheme, 3.0f, 1.0f, 0.04f, 0.02f))
                .opacity(opacity)
                .onClick([openTask, taskId = card.id] {
                    if (openTask) openTask(taskId);
                })
                .build();

            // 计划日期 + 星期：周二/周四用主色文字高亮（其余三级灰）
            ui.text(id + ".date")
                .position(kCardInset, kCardInset)
                .size(104.0f, kCardTitleLineH)
                .text(card.planDate)
                .fontSize(t.sizes.fontSizeLarge)
                .lineHeight(kCardTitleLineH)
                .fontWeight(600)
                .color(t.colors.textPrimary)
                .opacity(opacity)
                .build();
            ui.text(id + ".weekday")
                .position(kCardInset + 104.0f, kCardInset)
                .size(40.0f, kCardTitleLineH)
                .text(releaseWeekdayLabel(card.planDate))
                .fontSize(t.sizes.fontSizeSmall)
                .lineHeight(kCardTitleLineH)
                .fontWeight(600)
                .color(regularDay ? t.colors.primaryText : t.colors.textTertiary)
                .opacity(opacity)
                .build();

            drawChip(ui, id + ".status", layout.chipX, kCardInset + 1.0f, layout.chipW,
                     releaseTaskStatusText(card.status), chip.bg, chip.fg, t, opacity);

            // 进度条（4px 档，覆写组件默认 14）+ 右侧 done/total
            const float ratio = card.progressTotal > 0
                ? std::clamp(static_cast<float>(card.progressDone) /
                                 static_cast<float>(card.progressTotal),
                             0.0f, 1.0f)
                : 0.0f;
            ui.stack(id + ".progress.wrap")
                .position(kCardInset, layout.progressY + (kProgressRowH - kProgressH) * 0.5f)
                .size(layout.progressW, kProgressH)
                .content([&] {
                    components::progress(ui, id + ".progress")
                        .size(layout.progressW, kProgressH)
                        .value(ratio)
                        .theme(componentTheme)
                        .build();
                })
                .build();
            const std::string countText = std::to_string(card.progressDone) + "/" +
                                          std::to_string(card.progressTotal);
            ui.text(id + ".progress.count")
                .position(w - kCardInset - 44.0f, layout.progressY)
                .size(44.0f, kProgressRowH)
                .text(countText)
                .fontSize(t.sizes.fontSizeSmall - 1.0f)
                .lineHeight(kProgressRowH)
                .horizontalAlign(eui::HorizontalAlign::Right)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(t.colors.textSecondary)
                .opacity(opacity)
                .build();

            if (layout.hasEnvRow) {
                // 环境 chips 单行；放不下时末尾补「+N」（先预留 +N 宽度，保证不横向溢出）
                const float available = std::max(0.0f, w - kCardInset * 2.0f);
                const float moreW = chipWidth("+" + std::to_string(card.envNames.size()), 11.0f);
                float totalW = 0.0f;
                for (std::size_t i = 0; i < card.envNames.size(); ++i) {
                    totalW += chipWidth(card.envNames[i], 11.0f) +
                              (i == 0 ? 0.0f : 6.0f);
                }
                const bool overflow = totalW > available;
                const float budget = overflow ? std::max(0.0f, available - moreW - 6.0f)
                                              : available;
                float cx = 0.0f;
                std::size_t drawn = 0;
                for (std::size_t i = 0; i < card.envNames.size(); ++i) {
                    const float cw = chipWidth(card.envNames[i], 11.0f);
                    const float need = cw + (drawn == 0 ? 0.0f : 6.0f);
                    if (cx + need > budget) break;
                    drawChip(ui, id + ".env." + std::to_string(i), kCardInset + cx,
                             layout.envY, cw, card.envNames[i], t.colors.hover,
                             t.colors.textSecondary, t, opacity);
                    cx += need;
                    ++drawn;
                }
                if (overflow && drawn < card.envNames.size()) {
                    drawChip(ui, id + ".env.more", kCardInset + cx + 6.0f, layout.envY,
                             moreW, "+" + std::to_string(card.envNames.size() - drawn),
                             t.colors.hover, t.colors.textTertiary, t, opacity);
                }
            }

            if (layout.hasSummary) {
                ui.text(id + ".summary")
                    .position(kCardInset, layout.summaryY)
                    .size(std::max(0.0f, w - kCardInset * 2.0f), kSummaryRowH)
                    .text(card.summary)
                    .fontSize(t.sizes.fontSizeSmall)
                    .lineHeight(kSummaryRowH)
                    .maxWidth(std::max(0.0f, w - kCardInset * 2.0f))
                    .wrap(false)
                    .overflow(core::TextOverflow::Ellipsis)
                    .color(t.colors.textTertiary)
                    .opacity(opacity)
                    .build();
            }
        })
        .build();
}

void drawHeader(eui::Ui& ui, float contentW, const core::theme::Tokens& t,
                const ReleaseListActions& actions) {
    const float headerH = ui_kit::kHeaderH;
    ui.stack("release.tasks.header")
        .size(contentW, headerH)
        .content([&] {
            ui.text("release.tasks.title")
                .position(0.0f, 0.0f)
                .size(std::max(0.0f, contentW - 136.0f), headerH)
                .text("任务")
                .fontSize(t.sizes.fontSizeTitle)
                .lineHeight(24.0f)
                .fontWeight(700)
                .color(t.colors.textPrimary)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
            ui_kit::ButtonOptions options;
            options.onClick = actions.openNewTask;
            ui_kit::button(ui, "release.tasks.new", "新建任务",
                           ui_kit::ButtonLevel::Primary,
                           std::max(0.0f, contentW - 120.0f), 0.0f, 120.0f,
                           ui_kit::kButtonHeight, t, options);
        })
        .build();
}

// 空态：文案节点 id 保持 release.tasks.empty（骨架期契约，既有冒烟用例按此定位）。
void drawEmpty(eui::Ui& ui, float contentW, float bodyH, const core::theme::Tokens& t,
               const ReleaseListActions& actions) {
    ui.stack("release.tasks.emptybox")
        .size(contentW, bodyH)
        .content([&] {
            const float textY = std::max(0.0f, bodyH * 0.5f - 44.0f);
            ui.text("release.tasks.empty")
                .position(0.0f, textY)
                .size(contentW, 24.0f)
                .text("暂无数据")
                .fontSize(t.sizes.fontSizeBody)
                .lineHeight(24.0f)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(t.colors.textTertiary)
                .build();
            ui_kit::ButtonOptions options;
            options.onClick = actions.openNewTask;
            ui_kit::button(ui, "release.tasks.empty.new", "新建任务",
                           ui_kit::ButtonLevel::Primary,
                           std::max(0.0f, (contentW - 120.0f) * 0.5f), textY + 40.0f,
                           120.0f, ui_kit::kButtonHeight, t, options);
        })
        .build();
}

// 加载占位：灰块骨架 + 「加载中…」（EUI-NEO 无 skeleton 组件，自绘）。
void drawLoadingCard(eui::Ui& ui, const std::string& id, float x, float y, float w,
                     const core::theme::Tokens& t) {
    ui.stack(id)
        .position(x, y)
        .ignoreLayout()
        .size(w, kLoadingCardH)
        .content([&] {
            ui.rect(id + ".bg")
                .size(w, kLoadingCardH)
                .color(t.colors.surface)
                .border(1.0f, t.colors.borderDefault)
                .radius(kCardRadius)
                .build();
            ui.rect(id + ".bar1")
                .position(kCardInset, 16.0f)
                .size(std::max(0.0f, w - kCardInset * 2.0f - 80.0f), 14.0f)
                .color(t.colors.hover)
                .radius(4.0f)
                .build();
            ui.rect(id + ".bar2")
                .position(kCardInset, 40.0f)
                .size(std::max(0.0f, w - kCardInset * 2.0f - 150.0f), 12.0f)
                .color(t.colors.borderMuted)
                .radius(4.0f)
                .build();
            ui.rect(id + ".bar3")
                .position(kCardInset, 62.0f)
                .size(std::max(0.0f, w - kCardInset * 2.0f - 200.0f), 12.0f)
                .color(t.colors.borderMuted)
                .radius(4.0f)
                .build();
            ui.text(id + ".text")
                .position(std::max(0.0f, w - kCardInset - 64.0f), 16.0f)
                .size(64.0f, 16.0f)
                .text("加载中…")
                .fontSize(t.sizes.fontSizeSmall - 1.0f)
                .lineHeight(16.0f)
                .horizontalAlign(eui::HorizontalAlign::Right)
                .color(t.colors.textTertiary)
                .build();
        })
        .build();
}

// 加载失败提示卡：点击重试。与「加载中…」占位同高，保证内容高度稳定。
void drawRetryCard(eui::Ui& ui, const std::string& id, float x, float y, float w,
                   const core::theme::Tokens& t, const std::function<void()>& retry) {
    ui.stack(id)
        .position(x, y)
        .ignoreLayout()
        .size(w, kLoadingCardH)
        .content([&] {
            ui.rect(id + ".bg")
                .position(0.0f, 0.0f)
                .size(w, kLoadingCardH)
                .color(t.colors.surface)
                .border(1.0f, t.colors.borderDefault)
                .radius(kCardRadius)
                .onClick(retry)
                .build();
            ui.text(id + ".text")
                .position(0.0f, 0.0f)
                .size(w, kLoadingCardH)
                .text("加载失败，点击重试")
                .fontSize(t.sizes.fontSizeSmall)
                .lineHeight(kLoadingCardH)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(t.colors.textSecondary)
                .build();
        })
        .build();
}

// 首屏失败且没有任何已加载内容：页头保留，主体给可重试提示（不冒充「暂无数据」）。
void drawLoadErrorBody(eui::Ui& ui, float contentW, float bodyH,
                       const core::theme::Tokens& t,
                       const std::function<void()>& retry) {
    ui.stack("release.tasks.errorbox")
        .size(contentW, bodyH)
        .content([&] {
            const float w = 200.0f;
            const float h = 36.0f;
            const float x = std::max(0.0f, (contentW - w) * 0.5f);
            const float y = std::max(0.0f, bodyH * 0.5f - h * 0.5f);
            ui.rect("release.tasks.load.retry.bg")
                .position(x, y)
                .size(w, h)
                .color(t.colors.hover)
                .radius(t.sizes.radiusPill)
                .onClick(retry)
                .build();
            ui.text("release.tasks.load.failed")
                .position(x, y)
                .size(w, h)
                .text("加载失败，点击重试")
                .fontSize(t.sizes.fontSizeBody)
                .lineHeight(h)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(t.colors.textSecondary)
                .build();
        })
        .build();
}

// 页面级分页状态（跨帧存活；数据量不变时不重置）。仅供「卡片已全部预载」的
// 纯卡片重载使用；真实列表页走下面的 ReleaseListCache 按页读库。
struct ReleaseListState {
    ReleaseTaskPager pager;
};

// 便捷版的列表缓存（跨帧存活）：
//   cards      已加载的卡片（首屏 10 条起步，触底按 offset=cards.size() 追加）
//   total      countTasks() 的总数，决定「是否还有更多」
//   loading    下一页请求在飞（帧泵 6 帧，让占位可见）
//   failed     上一次加载失败：保留 cards，显示可重试提示，不自动重试
//   loaded     首屏是否已成功加载（失败时 false，重试后才会再取）
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
};

// 瀑布流分页数据源：把「卡片由谁持有 / 总数 / 可见数 / 在飞态 / 失败态」与绘制
// 解耦——纯卡片重载用内存 pager，store 重载用 SQL 分页缓存。
// 反重构警告：onChange 里只能捕获 ui.state 的长生命周期状态，不得捕获本结构
// （栈上对象在 compose 返回后即失效）。
struct WaterfallPaging {
    std::size_t total = 0;
    std::size_t visible = 0;
    bool loading = false;
    bool failed = false;
    // store 路径：当前页不足一屏时自动再取一页（矮窗没有滚动条，触底回调永不触发）
    bool autoFillShortViewport = false;
    std::function<void()> requestLoad;  // 触底请求下一页（去重由实现方负责）
    std::function<void()> advance;      // 帧泵：每帧调用一次
    std::function<void()> sync;         // advance/fill 后把来源状态回填到本结构
    std::function<void(float, const std::vector<float>&, int, float)> fillViewport;
    std::function<void()> retry;        // 失败提示点击
};

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void drawWaterfall(eui::Ui& ui, float contentW, float bodyH,
                   const std::vector<ReleaseTaskCard>& cards,
                   WaterfallPaging& paging,
                   const core::theme::Tokens& t,
                   const ReleaseListActions& actions) {
    // 帧泵先推进：advance 可能追加卡片（store 按需取页）或增加可见数（内存 pager）。
    if (paging.advance) paging.advance();
    if (paging.sync) paging.sync();

    const float gap = kReleaseTaskCardGap;
    // 列数 = floor((contentW+gap)/(minCardW+gap))；对 cols≥2 由该式可反推
    // cardW = (contentW−(cols−1)·gap)/cols ≥ minCardW；窗口窄到不足最小卡宽时
    // 退化为 1 列（此时卡宽 = contentW）。每帧按 contentW 重算，不缓存。
    const int columns = releaseMasonryColumns(contentW);
    const float cardW = columns > 0
        ? (contentW - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns)
        : contentW;
    std::vector<float> heights;
    heights.reserve(cards.size());
    for (const auto& card : cards) heights.push_back(releaseTaskCardHeight(card, cardW));

    // 矮窗自动续载（内存 pager 口径：首屏不足一屏时补到出现滚动条或数据耗尽）
    if (paging.fillViewport) {
        paging.fillViewport(bodyH, heights, columns, gap);
        if (paging.sync) paging.sync();
    }

    const std::size_t visible = std::min(paging.visible, cards.size());
    const std::vector<float> shown(heights.begin(), heights.begin() +
                                                      static_cast<std::ptrdiff_t>(visible));
    const std::vector<ReleaseMasonrySlot> slots = releaseMasonryLayout(shown, columns, gap);
    const float cardsH = releaseMasonryTotalHeight(shown, slots, gap);
    const bool loading = paging.loading;
    const bool failed = paging.failed;
    // 占位/失败提示与卡片同宽同高，二者互斥；空列表时贴顶不留间距
    const float footerH =
        (loading || failed) ? ((cardsH > 0.0f ? gap : 0.0f) + kLoadingCardH) : 0.0f;
    const float contentH = cardsH + footerH;

    // store 路径矮窗填满：当前页不足一屏且还有更多 → 请求下一页。
    // 没有这一步，矮窗里内容撑不出滚动条、触底回调永远不会触发。
    if (paging.autoFillShortViewport && !loading && !failed &&
        paging.visible < paging.total && cardsH < bodyH - kLoadThreshold) {
        if (paging.requestLoad) paging.requestLoad();
    }

    // 只复制去重后的请求动作进 onChange：捕获 ui.state 里的长生命周期状态，
    // 不能捕获栈上的 paging。
    const std::function<void()> requestLoad = paging.requestLoad;

    ui.stack("release.tasks.body")
        .size(contentW, bodyH)
        .content([&] {
            components::scrollView(ui, "release.tasks.scroll")
                .position(0.0f, 0.0f)
                .size(contentW, bodyH)
                .step(60.0f)
                .theme(core::theme::componentTokensWeChat(t))
                .scrollbarOverlay()
                .contentKey("release.tasks.scroll." + std::to_string(cards.size()) + "." +
                            std::to_string(visible) + "." + (loading ? "1" : "0") +
                            (failed ? "1" : "0"))
                .onChange([requestLoad, contentH, bodyH](float offset) {
                    // 反重构警告：这里只管「是否到内容底」；去重（在飞/失败待重试/
                    // 数据耗尽）由 requestLoad 的实现方负责，去掉会让内容增长前的
                    // 重复滚动回调连续追加两页。
                    if (!requestLoad) return;
                    if (offset + bodyH >= contentH - kLoadThreshold) requestLoad();
                })
                .content([&](eui::Ui& inner, float contentWidth, float viewportHeight) {
                    (void)viewportHeight;
                    // 唯一参与流式布局的子元素：撑出内容总高（卡片全部绝对定位）
                    inner.stack("release.tasks.spacer").size(contentWidth, contentH).build();
                    for (std::size_t i = 0; i < visible; ++i) {
                        const ReleaseTaskCard& card = cards[i];
                        // 列起点 = col·(卡宽+间距)：首列贴内容区左缘（col 0 → x 0），
                        // 相邻列间隙恰为 gap，最右列右缘 = 内容区右缘。
                        // 反重构警告：不可再写成 gap + col·(cardW+gap)——多算一段 gap
                        // 会让列间隙变 2·gap 并把最右列推出内容区。
                        const float cardX =
                            static_cast<float>(slots[i].column) * (cardW + gap);
                        drawTaskCard(inner, "release.tasks.card." + std::to_string(card.id),
                                     cardX, slots[i].y, cardW, card, t, actions);
                    }
                    const float footerY = cardsH + (cardsH > 0.0f ? gap : 0.0f);
                    if (loading) {
                        drawLoadingCard(inner, "release.tasks.loading", 0.0f, footerY,
                                        contentWidth, t);
                    } else if (failed) {
                        drawRetryCard(inner, "release.tasks.load.retry", 0.0f, footerY,
                                      contentWidth, t, paging.retry);
                    }
                })
                .build();

            // 滚动回调不自动重 compose（scrollView 未透传该开关）：
            // 触底置加载态后必须有一帧把占位画出来，这里按元素契约直接置位。
            if (core::dsl::Element* scroll = ui.find("release.tasks.scroll")) {
                scroll->composeOnScrollOffsetChange = true;
            }
            if (loading) {
                // 帧泵（照 todo.archive.anim.driver）：加载中的倒计时/追加由每帧 compose 推进
                ui.stack("release.tasks.load.pump").size(1.0f, 1.0f)
                    .onFrame([](float) {})
                    .build();
            }
        })
        .build();
}

} // namespace

// ---------- 纯逻辑导出 ----------

std::string releaseTaskStatusText(TaskStatus status) {
    switch (status) {
        case TaskStatus::Draft: return "配置中";
        case TaskStatus::Pending: return "待执行";
        case TaskStatus::Running: return "执行中";
        case TaskStatus::Done: return "已完成";
        case TaskStatus::Voided: return "已作废";
    }
    return "配置中";
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

int releaseMasonryColumns(float availableWidth, float minCardWidth, float gap) {
    if (minCardWidth <= 0.0f) minCardWidth = kReleaseTaskCardMinWidth;
    if (gap < 0.0f) gap = 0.0f;
    if (availableWidth <= 0.0f) return 1;
    const int columns =
        static_cast<int>(std::floor((availableWidth + gap) / (minCardWidth + gap)));
    return std::max(1, columns);
}

std::vector<ReleaseMasonrySlot> releaseMasonryLayout(
    const std::vector<float>& cardHeights, int columns, float gapY) {
    if (columns < 1) columns = 1;
    std::vector<float> columnHeights(static_cast<std::size_t>(columns), 0.0f);
    std::vector<ReleaseMasonrySlot> slots(cardHeights.size());
    for (std::size_t i = 0; i < cardHeights.size(); ++i) {
        int target = 0;
        for (int c = 1; c < columns; ++c) {
            if (columnHeights[static_cast<std::size_t>(c)] <
                columnHeights[static_cast<std::size_t>(target)]) {
                target = c;
            }
        }
        slots[i].column = target;
        slots[i].y = columnHeights[static_cast<std::size_t>(target)];
        columnHeights[static_cast<std::size_t>(target)] +=
            std::max(0.0f, cardHeights[i]) + gapY;
    }
    return slots;
}

float releaseMasonryTotalHeight(const std::vector<float>& cardHeights,
                                const std::vector<ReleaseMasonrySlot>& slots,
                                float gapY) {
    (void)gapY;
    float bottom = 0.0f;
    for (std::size_t i = 0; i < slots.size() && i < cardHeights.size(); ++i) {
        bottom = std::max(bottom, slots[i].y + std::max(0.0f, cardHeights[i]));
    }
    return bottom;
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

bool ReleaseTaskPager::fillViewport(float viewportHeight,
                                    const std::vector<float>& cardHeights,
                                    int columns, float gapY) {
    bool appended = false;
    while (hasMore()) {
        const std::size_t shown = std::min(visible_, cardHeights.size());
        const std::vector<float> visibleHeights(
            cardHeights.begin(), cardHeights.begin() + static_cast<std::ptrdiff_t>(shown));
        const std::vector<ReleaseMasonrySlot> slots =
            releaseMasonryLayout(visibleHeights, columns, gapY);
        if (releaseMasonryTotalHeight(visibleHeights, slots, gapY) >= viewportHeight) break;
        if (!loadMore()) break;
        appended = true;
    }
    return appended;
}

float releaseTaskCardHeight(const ReleaseTaskCard& card, float cardWidth) {
    (void)cardWidth;
    return cardLayout(card, cardWidth).height;
}

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
    // 整页共用一个时间快照：状态 chip 全部按同一时刻推导，不随逐卡装配漂移。
    const std::int64_t nowUnix = static_cast<std::int64_t>(core::time::nowUnix());
    for (const Task& task : tasks.value()) {
        ReleaseTaskCard card;
        card.id = task.id;
        card.planDate = task.planDate;

        auto envs = store.listTaskEnvs(task.id);
        if (envs.isFail()) {
            return core::Result<std::vector<ReleaseTaskCard>>::fail(
                envs.errorCode(), envs.errorMessage());
        }
        std::vector<TaskEnv> liveEnvs;
        std::vector<TaskService> liveServices;
        std::vector<TaskConfig> configs;
        std::vector<TaskStep> steps;
        for (const TaskEnv& env : envs.value()) {
            if (env.removed) continue;
            liveEnvs.push_back(env);
            card.envNames.push_back(env.envName);

            auto services = store.listTaskServices(env.id);
            if (services.isFail()) {
                return core::Result<std::vector<ReleaseTaskCard>>::fail(
                    services.errorCode(), services.errorMessage());
            }
            for (const TaskService& service : services.value()) {
                if (service.removed) continue;
                liveServices.push_back(service);
                auto serviceConfigs = store.listTaskConfigs(service.id);
                if (serviceConfigs.isFail()) {
                    return core::Result<std::vector<ReleaseTaskCard>>::fail(
                        serviceConfigs.errorCode(), serviceConfigs.errorMessage());
                }
                for (const TaskConfig& config : serviceConfigs.value()) {
                    configs.push_back(config);
                }
                auto serviceSteps = store.listTaskSteps(service.id);
                if (serviceSteps.isFail()) {
                    return core::Result<std::vector<ReleaseTaskCard>>::fail(
                        serviceSteps.errorCode(), serviceSteps.errorMessage());
                }
                for (const TaskStep& step : serviceSteps.value()) steps.push_back(step);
            }
        }

        // 进度与执行页同源：固定步骤（合并/部署/验证）+ 未跳过配置
        const Progress progress =
            taskProgress(task, liveEnvs, liveServices, configs, steps);
        card.progressDone = progress.done;
        card.progressTotal = progress.total;
        // 状态 chip 只按时间推导（spec §7 状态只跟时间走），不写库：到了时间窗还
        // 显示「待执行」、过了时间窗还显示「执行中」的库值旧状态不再回流到列表。
        card.status = deriveStatus(task, liveEnvs, nowUnix);
        card.summary = std::to_string(liveEnvs.size()) + " 个环境 · " +
                       std::to_string(liveServices.size()) + " 个服务";
        cards.push_back(std::move(card));
    }

    return core::Result<std::vector<ReleaseTaskCard>>::ok(std::move(cards));
}

// ---------- 页面组合 ----------

namespace {

// 页面骨架：根列 + 页头；body 回调拿到内容宽与主体高后自绘（空态/瀑布流/失败提示）。
void composeListShell(eui::Ui& ui, const eui::Screen& screen,
                      const ReleaseListActions& actions,
                      const std::function<void(float, float)>& body) {
    const auto& t = core::theme::current();
    const float pad = t.sizes.spacingLarge * 2.0f;  // 32（视觉规范：页面主 padding）
    const float contentW = std::max(0.0f, screen.width - pad * 2.0f);
    const float bodyTop = ui_kit::kHeaderH + t.sizes.spacingLarge;
    const float bodyH = std::max(0.0f, screen.height - pad - bodyTop);

    ui.column("release.tasks.root")
        .size(screen.width, screen.height)
        .padding(pad, 0.0f, pad, pad)  // 上=0 标题顶格（EUI padding 序 = left/top/right/bottom）
        .gap(t.sizes.spacingLarge)
        .content([&] {
            drawHeader(ui, contentW, t, actions);
            body(contentW, bodyH);
        })
        .build();
}

} // namespace

void composeReleaseListPage(eui::Ui& ui, const eui::Screen& screen,
                            const std::vector<ReleaseTaskCard>& cards,
                            const ReleaseListActions& actions) {
    composeListShell(ui, screen, actions, [&](float contentW, float bodyH) {
        const auto& t = core::theme::current();
        if (cards.empty()) {
            // 空列表也要把分页状态归零：否则「25 条 → 清空 → 25 条」会带着旧可见数
            ui.state<ReleaseListState>("release.tasks.list.state").pager.reset(0);
            drawEmpty(ui, contentW, bodyH, t, actions);
            return;
        }

        // 内存分页（卡片已全部预载）：pager 决定可见数，触底/矮窗续载只改可见数。
        ReleaseListState& st = ui.state<ReleaseListState>("release.tasks.list.state");
        st.pager.setTotal(cards.size());

        WaterfallPaging paging;
        paging.total = st.pager.total();
        paging.visible = st.pager.visible();
        paging.loading = st.pager.loading();
        paging.failed = false;
        paging.autoFillShortViewport = false;
        paging.requestLoad = [&st] {
            if (st.pager.loading() || !st.pager.hasMore()) return;
            st.pager.requestLoad();
        };
        paging.advance = [&st] {
            if (st.pager.loading()) st.pager.advanceLoad();
        };
        paging.fillViewport = [&st](float viewportHeight,
                                    const std::vector<float>& heights, int columns,
                                    float gapY) {
            st.pager.fillViewport(viewportHeight, heights, columns, gapY);
        };
        paging.sync = [&st, &paging] {
            paging.total = st.pager.total();
            paging.visible = st.pager.visible();
            paging.loading = st.pager.loading();
        };
        drawWaterfall(ui, contentW, bodyH, cards, paging, t, actions);
    });
}

void composeReleaseListPage(eui::Ui& ui, const eui::Screen& screen,
                            ReleaseStore& store,
                            ReleaseNewTaskDialogState& newTaskState,
                            const ReleaseListActions& actions) {
    ReleaseListCache& cache = ui.state<ReleaseListCache>("release.tasks.cache");

    const auto loadTotal = [&cache, &store]() -> bool {
        auto count = store.countTasks();
        if (count.isFail()) {
            LOG_ERROR("release: 读取任务总数失败：" + count.errorMessage());
            cache.failed = true;
            return false;
        }
        cache.total = static_cast<std::size_t>(count.value());
        return true;
    };

    // replace=true：以本页整体替换已加载内容（首屏/重锚）；false：追加下一页。
    // 失败一律保留 cache.cards（不清空），置 failed 让界面显示可重试提示。
    const auto loadPage = [&cache, &store](int offset, int limit, bool replace) -> bool {
        auto page = releaseLoadTaskCards(store, offset, limit);
        if (page.isFail()) {
            LOG_ERROR("release: 读取任务列表失败：" + page.errorMessage());
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
        if (!loadTotal()) return false;
        return loadPage(0, static_cast<int>(kReleaseTaskPageSize), true);
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
        if (loadTotal()) {
            if (cache.cards.empty() || cache.total != before) {
                cache.loaded = loadPage(0, static_cast<int>(kReleaseTaskPageSize), true);
            } else {
                cache.loaded = loadPage(0, static_cast<int>(cache.cards.size()), true);
            }
        }
    } else if (!cache.loaded) {
        // 首屏：仅在未失败时自动加载；失败后等用户点「重试」，不每帧重打库
        if (!cache.failed && !cache.loading) {
            cache.loaded = loadFirstPage();
        }
    } else if (!cache.failed && !cache.loading && now - cache.loadedAtMs >= 1000) {
        // 陈旧刷新：总数变了就重锚首屏；否则只重取已加载条数（下限一页），
        // 不把已翻出的页塌回首屏。
        const std::size_t before = cache.total;
        if (loadTotal()) {
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

    WaterfallPaging paging;
    paging.total = cache.total;
    paging.visible = cache.cards.size();
    paging.loading = cache.loading;
    paging.failed = cache.failed;
    paging.autoFillShortViewport = true;
    paging.requestLoad = [&cache] {
        // 去重三闸：失败待重试 / 已有请求在飞 / 已到总数。达到总数后不再进入
        // 加载态，界面也就不会再出现「加载中…」。
        if (cache.failed || cache.loading) return;
        if (cache.cards.size() >= cache.total) return;
        cache.loading = true;
        cache.framesLeft = 6;  // ≈100ms：让「加载中…」占位可被看见
    };
    paging.advance = [&cache, &store] {
        if (!cache.loading) return;
        if (--cache.framesLeft > 0) return;
        cache.loading = false;
        cache.framesLeft = 0;

        // 下一页 offset = 已加载数；已加载内容原样保留（失败也不清空）。
        auto page = releaseLoadTaskCards(store, static_cast<int>(cache.cards.size()),
                                         static_cast<int>(kReleaseTaskPageSize));
        if (page.isFail()) {
            LOG_ERROR("release: 加载下一页任务失败：" + page.errorMessage());
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
        }
        if (page.value().empty() && cache.cards.size() < cache.total) {
            cache.total = cache.cards.size();
        }
        cache.loadedAtMs = nowMs();
    };
    paging.sync = [&cache, &paging] {
        paging.total = cache.total;
        paging.visible = cache.cards.size();
        paging.loading = cache.loading;
        paging.failed = cache.failed;
    };
    paging.retry = [&cache] {
        cache.failed = false;
        if (cache.cards.empty()) {
            cache.loaded = false;  // 无内容：下一帧按首屏 offset=0/limit=10 重取
            return;
        }
        cache.loaded = true;       // 有内容：保留已加载页，从已加载数继续取
        cache.loading = true;
        cache.framesLeft = 6;
    };

    // 「新建任务」未注入时默认打开本页弹层；注入自定义回调（如先切页）则以调用方为准
    ReleaseListActions effective = actions;
    if (!effective.openNewTask) {
        effective.openNewTask = [&newTaskState] { newTaskState.open = true; };
    }

    if (!cache.cards.empty() || (!cache.failed && cache.loaded)) {
        // 常规：有内容画瀑布流（visible = 已加载数，不再二次切片）；无内容且
        // 首屏加载成功画空态。
        composeListShell(ui, screen, effective, [&](float contentW, float bodyH) {
            const auto& t = core::theme::current();
            if (cache.cards.empty()) {
                drawEmpty(ui, contentW, bodyH, t, effective);
            } else {
                drawWaterfall(ui, contentW, bodyH, cache.cards, paging, t, effective);
            }
        });
    } else {
        // 首屏失败且没有任何已加载内容：页头保留，主体给可重试提示（不冒充空态）
        composeListShell(ui, screen, effective, [&](float contentW, float bodyH) {
            drawLoadErrorBody(ui, contentW, bodyH, core::theme::current(), paging.retry);
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