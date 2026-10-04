// 首页余量卡片绘制（契约见 home_quota_card.h）。纪律：画不画条只看 showsQuotaBar；
// 状态文案按 kind 在桥里选，这里原样渲染；几何全部来自常量 + 纯函数，probe 可量。
#include "home_quota_card.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "core/component_theme.h"

#include <components/button.h>
#include <components/card.h>
#include <components/progress.h>
#include <components/scrollview.h>
#include <components/theme.h>

namespace module::home::quota_card {

namespace {

using module::claude::quota::QuotaHomeKind;
using module::claude::quota::QuotaHomeWindow;

// 布局常量（16px 网格；改这里 = 改观感，probe 会量到）。
constexpr float kPad = 16.0f;
constexpr float kHeadH = 24.0f;
constexpr float kTitleH = 28.0f;
constexpr float kTitleGap = 10.0f;
constexpr float kRowGap = 10.0f;
constexpr float kWinRowH = 24.0f;
constexpr float kBalRowH = 26.0f;
constexpr float kErrRowH = 18.0f;
constexpr float kBarH = 8.0f;
constexpr float kInnerGap = 8.0f;

constexpr float kNameW = 168.0f;
constexpr float kProviderW = 112.0f;
constexpr float kStaleW = 68.0f;
// 卡头 [刷新] 与行内 [去配置] 的统一按钮宽：button.size 是定宽帧、无内容自适应，
// 等宽只能靠读同一个常量（分开写就是漂移入口）。
constexpr float kQuotaBtnW = 96.0f;
constexpr float kWinLabelW = 52.0f;
constexpr float kPctW = 52.0f;
constexpr float kResetW = 108.0f;

eui::Color tintColor(Tint tint, const core::theme::Tokens& t) {
    switch (tint) {
        case Tint::Red: return t.colors.error;
        case Tint::Orange: return t.colors.warning;
        case Tint::Green: break;
    }
    return t.colors.success;
}

// Unconfigured / Normal / Pending 无错误文案。
bool showsErrorLine(QuotaCardState state) {
    return state == QuotaCardState::Incomplete ||
           state == QuotaCardState::FailedNever ||
           state == QuotaCardState::FailedStale;
}

// 仅未配置/配置不完整两态给「去配置」按钮；失败态（401/套餐过期）给按钮是误导。
bool showsConfigButton(QuotaCardState state) {
    return state == QuotaCardState::Unconfigured ||
           state == QuotaCardState::Incomplete;
}

const char* stateFallbackText(QuotaCardState state) {
    switch (state) {
        case QuotaCardState::Unconfigured: return "未配置余量查询";
        case QuotaCardState::Incomplete: return "配置不完整，去补全";
        case QuotaCardState::Pending: return "等待首次查询";
        case QuotaCardState::Normal: return "";
        case QuotaCardState::FailedStale: return "";
        case QuotaCardState::FailedNever: return "";
    }
    return "";
}

// 状态行文字：有错时给空串——错误文案由专属错误行渲染，head 行再渲染会重复且被
// 固定宽单行盒截断。
std::string statusTextFor(const QuotaHomeRow& row) {
    if (showsErrorLine(row.state)) return std::string();
    return stateFallbackText(row.state);
}

// Howard Hinnant days_from_civil；不用 mktime/timegm 以免本地时区把 UTC 解释偏一个时区。
int64_t daysFromCivil(int64_t y, int64_t m, int64_t d) {
    y -= (m <= 2) ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;                              // [0, 399]
    const int64_t doy = (153 * (m + ((m > 2) ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;      // [0, 146096]
    return era * 146097 + doe - 719468;
}

// 解析 "YYYY-MM-DDTHH:MM:SS"（尾部 Z/毫秒/偏移忽略）；逐字符校验，不猜不容错。
bool parseIsoUtcSeconds(const std::string& s, int64_t* out) {
    if (s.size() < 19) return false;
    const auto digitsAt = [&](std::size_t pos, std::size_t n, int64_t* v) {
        int64_t acc = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const char c = s[pos + i];
            if (c < '0' || c > '9') return false;
            acc = acc * 10 + (c - '0');
        }
        *v = acc;
        return true;
    };
    int64_t y = 0, mo = 0, d = 0, hh = 0, mi = 0, ss = 0;
    if (!digitsAt(0, 4, &y) || s[4] != '-' || !digitsAt(5, 2, &mo) ||
        s[7] != '-' || !digitsAt(8, 2, &d)) {
        return false;
    }
    if (s[10] != 'T' && s[10] != ' ') return false;
    if (!digitsAt(11, 2, &hh) || s[13] != ':' || !digitsAt(14, 2, &mi) ||
        s[16] != ':' || !digitsAt(17, 2, &ss)) {
        return false;
    }
    if (mo < 1 || mo > 12 || d < 1 || d > 31) return false;
    if (hh > 23 || mi > 59 || ss > 60) return false;
    *out = daysFromCivil(y, mo, d) * 86400 + hh * 3600 + mi * 60 + ss;
    return true;
}

}  // namespace

bool showsQuotaBar(QuotaCardState state) {
    // 「画条」= 有过成功值（Normal / FailedStale）。没配/不完整/从未成功时画条必是假 0%。
    return state == QuotaCardState::Normal ||
           state == QuotaCardState::FailedStale;
}

Tint tintForUsedPercent(double usedPercent) {
    if (usedPercent >= 90.0) return Tint::Red;
    if (usedPercent >= 70.0) return Tint::Orange;
    return Tint::Green;
}

std::string formatQuotaPercent(double usedPercent) {
    char buf[32];
    if (usedPercent >= 10.0) {
        std::snprintf(buf, sizeof(buf), "%.0f%%", usedPercent);
    } else {
        std::snprintf(buf, sizeof(buf), "%.1f%%", usedPercent);
    }
    return std::string(buf);
}

std::string formatQuotaBalance(double remaining, const std::string& currency) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "余额 %.2f", remaining);
    std::string out(buf);
    if (!currency.empty()) out += " " + currency;
    return out;
}

std::string formatQuotaResetCountdown(const std::string& resetsAtIso,
                                     int64_t nowSec) {
    int64_t target = 0;
    if (!parseIsoUtcSeconds(resetsAtIso, &target)) return std::string();
    const int64_t delta = target - nowSec;
    if (delta <= 0) return "已过期";
    if (delta < 60) return "即将重置";
    if (delta < 3600) {
        return std::to_string(delta / 60) + " 分钟后重置";
    }
    if (delta < 86400) {
        return std::to_string(delta / 3600) + " 小时后重置";
    }
    return std::to_string(delta / 86400) + " 天后重置";
}

float quotaRowHeight(const QuotaHomeRow& row) {
    float h = kHeadH;
    if (row.kind == QuotaHomeKind::Balance) {
        h += kBalRowH;
    } else {
        h += static_cast<float>(row.windows.size()) * kWinRowH;
    }
    if (showsErrorLine(row.state)) h += kErrRowH;
    return h;
}

QuotaCardMetrics computeQuotaCardMetrics(const std::vector<QuotaHomeRow>& rows,
                                         float contentWidth,
                                         float maxBodyHeight) {
    (void)contentWidth;   // 当前布局不随宽度变高度（行内元素全部按宽自适应）
    QuotaCardMetrics m;
    if (rows.empty()) {
        m.bodyHeight = kQuotaCardEmptyH;
        m.cardHeight = kPad * 2.0f + kTitleH + kTitleGap + m.bodyHeight;
        return m;
    }
    float contentH = 0.0f;
    for (const auto& row : rows) contentH += quotaRowHeight(row);
    contentH += kRowGap * static_cast<float>(rows.size() - 1);
    // maxBodyHeight 由调用方按窗口剩余空间给；负值（窗口太矮）兜成 0。
    const float cap = std::max(0.0f, maxBodyHeight);
    m.bodyHeight = std::min(contentH, cap);
    m.overflows = contentH > m.bodyHeight + 0.5f;
    m.cardHeight = kPad * 2.0f + kTitleH + kTitleGap + m.bodyHeight;
    return m;
}

HomeQuotaBudget computeHomeQuotaBudget(float screenHeight, float statCardHeight,
                                       float pad, float gap) {
    HomeQuotaBudget out;
    // 除「余量卡行内容 + 图表卡」外，本页其余各块的固定占用（与 home_module 绘制一一对应）。
    const float fixed = pad * 2.0f                    // 上下内边距
                        + kHomeWelcomeH + gap          // 欢迎区
                        + statCardHeight + gap        // 统计网格
                        + kQuotaCardChromeH + gap     // 余量卡外壳（不含行内容）
                        + gap                          // 图表卡与快捷按钮行之间
                        + kHomeActionsH;               // 快捷按钮行

    // ⓪ 页面本来装不下 → 滚动模式：图表一分不压、余量卡行内容给到上限，超出由
    // 整页 home.scroll 吸收（pageStillOverflows 恒 false——那是 scrollView 的本职）。
    const float pageWithoutQuotaBody = fixed + kHomeChartH;
    if (screenHeight < pageWithoutQuotaBody) {
        out.chartHeight = kHomeChartH;            // 图表一分不压
        out.quotaBodyHeight = kQuotaCardMaxBodyH; // 行内容给上限（内容少时取 min）
        out.drawsQuotaCard = true;
        out.pageStillOverflows = false;
        return out;
    }

    // ① 先按「图表不压缩」算一遍。
    float chart = kHomeChartH;
    float quotaBody = screenHeight - fixed - chart;
    // ② 只有压缩图表真能换到一张画得出来的余量卡时才压缩，否则只是把图表压成一条缝。
    const bool compressionCanHelp =
        screenHeight - fixed - kHomeChartMinH >= kQuotaCardMinBodyH;
    if (quotaBody < kQuotaCardMinBodyH && compressionCanHelp) {
        const float need = kQuotaCardMinBodyH - quotaBody;   // 还差多少
        chart = std::max(kHomeChartMinH, chart - need);
        quotaBody = screenHeight - fixed - chart;
    }
    out.chartHeight = chart;
    out.quotaBodyHeight = std::max(0.0f, quotaBody);
    out.drawsQuotaCard = quotaBody >= kQuotaCardMinBodyH;
    // 压到底仍装不下：余量卡整张不画，图表保持完整高度。
    out.pageStillOverflows = !out.drawsQuotaCard;
    return out;
}

namespace {

// 一行窗口（进度条 + 百分比 [+ 倒计时]）。pct 列按「reset 列在场」预留：
// 固定四列后 pct 的 x 与 reset 有无无关。
void drawWindowRow(eui::Ui& ui, const core::theme::Tokens& t, float cw,
                   const std::string& rowId, const QuotaHomeWindow& w,
                   int64_t nowSec) {
    const std::string countdown = formatQuotaResetCountdown(w.resetsAt, nowSec);
    const bool hasReset = !countdown.empty();
    const std::string barId = rowId + ".bar." + w.key;
    const float gaps = kInnerGap * 3.0f;
    const float barW = std::max(
        24.0f, cw - kWinLabelW - kPctW - kResetW - gaps);

    ui.row(barId + ".row").size(cw, kWinRowH).alignItems(eui::Align::CENTER)
        .gap(kInnerGap).content([&] {
            ui.text(barId + ".label")
                .size(kWinLabelW, 20.0f)
                .text(w.label)
                .fontSize(t.sizes.fontSizeSmall)
                .lineHeight(20.0f)
                .color(t.colors.textSecondary)
                .build();

            components::ProgressStyle ps;
            const auto ct = core::theme::componentTokens(t);
            ps.track = ct.surfaceHover;
            ps.fill = tintColor(tintForUsedPercent(w.usedPercent), t);
            components::progress(ui, barId)
                .size(barW, kBarH)
                .value(static_cast<float>(w.usedPercent / 100.0))
                .style(ps)
                .build();

            ui.text(barId + ".pct")
                .size(kPctW, 20.0f)
                .text(formatQuotaPercent(w.usedPercent))
                .fontSize(t.sizes.fontSizeSmall)
                .lineHeight(20.0f)
                .horizontalAlign(eui::HorizontalAlign::Right)
                .color(tintColor(tintForUsedPercent(w.usedPercent), t))
                .build();

            if (hasReset) {
                ui.text(barId + ".reset")
                    .size(kResetW, 20.0f)
                    .text(countdown)
                    .fontSize(t.sizes.fontSizeSmall)
                    .lineHeight(20.0f)
                    .horizontalAlign(eui::HorizontalAlign::Right)
                    .color(t.colors.textTertiary)
                    .build();
            }
        })
        .build();
}

}  // namespace

void drawQuotaCard(eui::Ui& ui, const core::theme::Tokens& t, float contentWidth,
                   const std::vector<QuotaHomeRow>& rows, const Hooks& hooks,
                   float maxBodyHeight) {
    const QuotaCardMetrics m =
        computeQuotaCardMetrics(rows, contentWidth, maxBodyHeight);
    const int64_t nowSec = static_cast<int64_t>(std::time(nullptr));
    // 卡壳 / 卡头行滚动缩进 / scrollView 共用同一份组件 token（分开取就是漂移入口）。
    const auto ct = core::theme::componentTokens(t);

    components::card(ui, "home.quota")
        .size(contentWidth, m.cardHeight)
        .theme(ct)
        .padding(kPad)
        .content([&] {
            const float cw = contentWidth - kPad * 2.0f;
            // 标题行 + [刷新]。⚠ .gap(kInnerGap) 必须在（否则按钮零间距、右缘短 8px）。
            // 卡头行宽须与滚动内容列同宽：scrollView 溢出时收窄 width−(scrollbar+gap)，
            // 卡头行不跟就会比 [去配置] 突出（inset 取自同一份 ct.metrics，永不漂移）。
            const float sbInset =
                m.overflows
                    ? ct.metrics.control.scrollbar + ct.metrics.spacing.section
                    : 0.0f;
            const float headW = cw - sbInset;
            ui.row("home.quota.head").size(headW, kTitleH)
                .alignItems(eui::Align::CENTER).gap(kInnerGap).content([&] {
                    ui.text("home.quota.title")
                        .size(headW - (kQuotaBtnW + kInnerGap), 24.0f)
                        .text("供应商余量")
                        .fontSize(t.sizes.fontSizeLarge - 1.0f)
                        .lineHeight(24.0f)
                        .fontWeight(600)
                        .color(t.colors.textPrimary)
                        .build();
                    // ⚠ 按值捕获：compose 栈上的临时 hooks 不会延活到 onClick 触发那一刻；
                    // 换成捕获 this 的变体会真悬垂。`auto x = hooks.x` 是显式快照意图。
                    auto refreshAll = hooks.refreshAll;
                    components::button(ui, "home.quota.refresh")
                        .size(kQuotaBtnW, 28.0f)
                        .text("刷新")
                        .icon(0xF021)   // fa-arrows-rotate
                        .fontSize(t.sizes.fontSizeSmall)
                        .colors(t.colors.surfaceElevated, t.colors.hover,
                                t.colors.active)
                        .textColor(t.colors.textSecondary)
                        .iconColor(t.colors.textSecondary)
                        .radius(t.sizes.radius)
                        .border(1.0f, t.colors.borderDefault)
                        .onClick(refreshAll)
                        .build();
                })
                .build();

            // 空态与读库失败分开：查询失败说成「没配」是静默失败面。
            if (rows.empty()) {
                ui.text("home.quota.empty")
                    .size(cw, 40.0f)
                    .text(hooks.loadError.empty()
                              ? std::string("暂无 Claude Profile")
                              : ("余量加载失败：" + hooks.loadError))
                    .fontSize(t.sizes.fontSizeBody)
                    .lineHeight(40.0f)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .color(hooks.loadError.empty() ? t.colors.textTertiary
                                                    : t.colors.error)
                    .build();
                return;
            }

            components::scrollView(ui, "home.quota.scroll")
                .position(0.0f, kTitleH + kTitleGap)
                .size(cw, m.bodyHeight)
                .gap(kRowGap)
                .theme(ct)
                .content([&](eui::Ui& inner, float innerW, float) {
                    for (const QuotaHomeRow& row : rows) {
                        const std::string rowId =
                            "home.quota.row." + std::to_string(row.profileId);
                        const bool showBar = showsQuotaBar(row.state);
                        const bool showErr = showsErrorLine(row.state);
                        const bool showCfg = showsConfigButton(row.state);
                        const bool showStale =
                            row.state == QuotaCardState::FailedStale;

                        inner.column(rowId)
                            .size(innerW, quotaRowHeight(row))
                            .content([&] {
                                const float nameW =
                                    std::min(kNameW, innerW * 0.24f);
                                const float staleW = showStale ? kStaleW : 0.0f;
                                const float cfgW = showCfg ? kQuotaBtnW : 0.0f;
                                // gap 数 = 子元素数 − 1（不是可选徽标数），
                                // 按子元素数算否则 .status 右缘恒越出内容列右缘。
                                const int headChildCount =
                                    3 + (staleW > 0.0f ? 1 : 0) +
                                    (cfgW > 0.0f ? 1 : 0);
                                const float gaps =
                                    kInnerGap * static_cast<float>(headChildCount - 1);
                                const float statusW = std::max(
                                    40.0f,
                                    innerW - nameW - kProviderW - staleW - cfgW -
                                        gaps);

                                inner.row(rowId + ".head")
                                    .size(innerW, kHeadH)
                                    .alignItems(eui::Align::CENTER)
                                    .gap(kInnerGap)
                                    .content([&] {
                                        // ⚠ .name/.provider/.status 是固定宽单行盒不能 wrap（否则撑破
                                        // quotaRowHeight 预算），而名长不受控：ellipsis 管排版宽（需
                                        // 显式 maxWidth 才生效），clip 管绘制范围，两层不冲突。
                                        inner.text(rowId + ".name")
                                            .size(nameW, kHeadH)
                                            .text(row.profileName)
                                            .fontSize(t.sizes.fontSizeBody)
                                            .lineHeight(kHeadH)
                                            .fontWeight(600)
                                            .color(t.colors.textPrimary)
                                            .maxWidth(nameW)
                                            .overflow(
                                                core::TextOverflow::Ellipsis)
                                            .clip()
                                            .build();
                                        inner.text(rowId + ".provider")
                                            .size(kProviderW, kHeadH)
                                            .text(row.providerDisplayName)
                                            .fontSize(t.sizes.fontSizeSmall)
                                            .lineHeight(kHeadH)
                                            .color(t.colors.textSecondary)
                                            .maxWidth(kProviderW)
                                            .overflow(
                                                core::TextOverflow::Ellipsis)
                                            .clip()
                                            .build();
                                        if (showStale) {
                                            inner.text(rowId + ".stale")
                                                .size(staleW, 18.0f)
                                                .text("可能过期")
                                                .fontSize(
                                                    t.sizes.fontSizeSmall)
                                                .lineHeight(18.0f)
                                                .horizontalAlign(
                                                    eui::HorizontalAlign::
                                                        Center)
                                                .color(t.colors.warning)
                                                .build();
                                        }
                                        inner.text(rowId + ".status")
                                            .size(statusW, kHeadH)
                                            .text(statusTextFor(row))
                                            .fontSize(t.sizes.fontSizeSmall)
                                            .lineHeight(kHeadH)
                                            // 单行盒不许把字画出盒：Ellipsis 需 wrap==false 且 maxWidth>0，故加 clip。
                                            .clip()
                                            .color(showErr ? t.colors.error
                                                           : t.colors
                                                                 .textTertiary)
                                            .build();
                                        if (showCfg) {
                                            // ⚠ 按值捕获：onClick 在 compose 返回后才触发，hooks 是
                                            // 栈上临时对象，按引用捕获就是悬垂，点一下直接崩。
                                            auto openConfig = hooks.openConfig;
                                            const int64_t profileId =
                                                row.profileId;
                                            // ⚠ 按钮高 = kHeadH，与 head 行同高，CENTER 后不溢出。
                                            components::button(
                                                inner, rowId + ".config")
                                                .size(cfgW, kHeadH)
                                                .text("去配置")
                                                .fontSize(
                                                    t.sizes.fontSizeSmall)
                                                .colors(
                                                    t.colors.surfaceElevated,
                                                    t.colors.hover,
                                                    t.colors.active)
                                                .textColor(
                                                    t.colors.primaryText)
                                                .radius(t.sizes.radius)
                                                .border(
                                                    1.0f,
                                                    t.colors.borderDefault)
                                                .onClick([openConfig,
                                                          profileId] {
                                                    if (openConfig) {
                                                        openConfig(profileId);
                                                    }
                                                })
                                                .build();
                                        }
                                    })
                                    .build();
                                // 数据区（唯一画条的地方）。
                                if (showBar) {
                                    if (row.kind == QuotaHomeKind::Balance) {
                                        inner.row(rowId + ".balance")
                                            .size(innerW, kBalRowH)
                                            .alignItems(eui::Align::CENTER)
                                            .content([&] {
                                                inner.text(
                                                    rowId + ".balance.amount")
                                                    .size(innerW * 0.6f, 24.0f)
                                                    .text(formatQuotaBalance(
                                                        row.balanceRemaining,
                                                        row.currency))
                                                    .fontSize(
                                                        t.sizes.fontSizeBody)
                                                    .lineHeight(24.0f)
                                                    .fontWeight(600)
                                                    .color(t.colors
                                                               .textPrimary)
                                                    .build();
                                                if (!row.planName.empty()) {
                                                    inner.text(rowId +
                                                               ".balance.plan")
                                                        .size(
                                                            innerW * 0.4f,
                                                            20.0f)
                                                        .text(row.planName)
                                                        .fontSize(
                                                            t.sizes
                                                                .fontSizeSmall)
                                                        .lineHeight(20.0f)
                                                        .horizontalAlign(
                                                            eui::HorizontalAlign::
                                                                Right)
                                                        .color(t.colors
                                                                   .textTertiary)
                                                        .build();
                                                }
                                            })
                                            .build();
                                    } else {
                                        for (const QuotaHomeWindow& w :
                                             row.windows) {
                                            drawWindowRow(inner, t, innerW,
                                                          rowId, w, nowSec);
                                        }
                                    }
                                }

                                // 错误行（错误文案的唯一出处）。
                                if (showErr) {
                                    inner.text(rowId + ".error")
                                        .size(innerW, kErrRowH)
                                        .text(row.lastError)
                                        .fontSize(t.sizes.fontSizeSmall)
                                        .lineHeight(kErrRowH)
                                        // 省略号是单行语义：只有 wrap==false 且 maxWidth>0 才生效，本格固定高
                                        // 单行，显式 maxWidth 是生效前提（漏了 ellipsis 静默失效）；.clip() 兜底。
                                        .maxWidth(innerW)
                                        .overflow(core::TextOverflow::Ellipsis)
                                        .clip()
                                        .color(t.colors.error)
                                        .build();
                                }
                            })
                            .build();
                    }
                })
                .build();
        })
        .build();
}

}  // namespace module::home::quota_card
