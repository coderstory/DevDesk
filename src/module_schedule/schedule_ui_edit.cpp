// 定时调度编辑弹窗（波 S7b）：纯逻辑实现 + 渲染。
// 纯逻辑部分（spec_json 编解码/校验/预览/记录装配）不依赖 EUI 运行时，
// 由 tests/test_modules/test_schedule_ui_edit.cpp 无头直测；渲染部分
// （drawEditDialog）只做控件绘制与事件转发，落库走 EditDialogHost。
#include "schedule_ui_edit.h"

#include "core/component_theme.h"
#include "core/time.h"

#include <ui_kit/dialog_form.h>

#include <components/button.h>
#include <components/dialog.h>
#include <components/input.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <utility>

namespace module::schedule::ui {

namespace {

// ---- 纯逻辑工具 ----

// JSON 字符串转义（值域 = spec_json/action_json 的全部字符串字段）
std::string escapeJsonString(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (unsigned char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

// "YYYY-MM-DD" → y/m/d；格式非法或日期不存在 → false
bool parseDateText(const std::string& text, int& y, int& m, int& d) {
    int yy = 0, mm = 0, dd = 0;
    char s1 = 0, s2 = 0;
    if (std::sscanf(text.c_str(), "%d%c%d%c%d", &yy, &s1, &mm, &s2, &dd) != 5) return false;
    if (s1 != '-' || s2 != '-') return false;
    if (mm < 1 || mm > 12 || dd < 1) return false;
    const int dim = (mm == 2)
                        ? ((yy % 4 == 0 && yy % 100 != 0) || yy % 400 == 0 ? 29 : 28)
                        : (mm == 4 || mm == 6 || mm == 9 || mm == 11 ? 30 : 31);
    if (dd > dim) return false;
    y = yy; m = mm; d = dd;
    return true;
}

// 日期 → 周几（0=周日..6=周六）。1970-01-01 为周四（daysFromCivil=0）。
// 反例锚定：2026-10-27（周二）必须得 2——本函数是「anchor_date 匹配周几」
// 校验的唯一真源，改公式必须先过该断言。
int weekdayOfDate(int y, int m, int d) {
    const int64_t days = core::time::daysFromCivil(y, static_cast<unsigned>(m),
                                                   static_cast<unsigned>(d));
    return static_cast<int>(((days % 7) + 7 + 4) % 7);
}

// "HH:MM" → hour/minute；非法 → false
bool parseTimeText(const std::string& text, int& hour, int& minute) {
    int h = 0, mi = 0;
    char sep = 0;
    if (std::sscanf(text.c_str(), "%d%c%d", &h, &sep, &mi) != 3) return false;
    if (sep != ':') return false;
    if (h < 0 || h > 23 || mi < 0 || mi > 59) return false;
    hour = h; minute = mi;
    return true;
}

std::string formatTime(int hour, int minute) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", hour, minute);
    return buf;
}

std::string formatDate(int y, int m, int d) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m, d);
    return buf;
}

core::Result<core::schedule::RuleSpec> failValidation(const std::string& msg) {
    return core::Result<core::schedule::RuleSpec>::fail(core::ErrorCode::ValidationError, msg);
}

// calendar 各形态的 spec_json：week（weekdays+anchor_date）、day（anchor_date/
// workdays_only）、month/year（三选一选择器）。
core::Result<std::string> buildCalendarJson(const EditFormState& f) {
    const auto failMsg = [](const std::string& m) {
        return core::Result<std::string>::fail(core::ErrorCode::ValidationError, m);
    };
    std::string time;
    {
        int h = 0, mi = 0;
        if (!parseTimeText(formatTime(f.calendarHour, f.calendarMinute), h, mi)) {
            return failMsg("calendar.time 必须为 HH:MM 且小时 0-23、分钟 0-59");
        }
        time = formatTime(h, mi);
    }
    std::string out = "{\"unit\":\"" + f.calendarUnit + "\",\"interval\":" +
                      std::to_string(f.calendarInterval) + ",";
    if (f.calendarUnit == "week") {
        out += "\"weekdays\":[";
        for (size_t i = 0; i < f.calendarWeekdays.size(); ++i) {
            if (i) out += ",";
            out += std::to_string(f.calendarWeekdays[i]);
        }
        out += "],";
    }
    if (f.calendarWorkdaysOnly) out += "\"workdays_only\":true,";
    if (f.calendarUnit == "month" || f.calendarUnit == "year") {
        if (f.calendarHasMonthDay) {
            out += "\"month_day\":" + std::to_string(f.calendarMonthDay) + ",";
        } else if (f.calendarHasNthWeekday) {
            out += "\"nth_weekday\":{\"n\":" + std::to_string(f.calendarNth) +
                   ",\"weekday\":" + std::to_string(f.calendarNthWeekday) + "},";
        } else {
            out += "\"last_workday\":true,";
        }
    }
    out += "\"time\":\"" + time + "\"";
    // anchor_date 仅 unit=day/week 合法（解析器对 month/year 显式拒绝——校验互斥）
    if (f.calendarUnit == "day" || f.calendarUnit == "week") {
        out += ",\"anchor_date\":\"" + escapeJsonString(f.calendarAnchorDate) + "\"";
    }
    out += "}";
    return core::Result<std::string>::ok(out);
}

}  // namespace

std::string kindToText(core::schedule::RuleKind kind) {
    switch (kind) {
        case core::schedule::RuleKind::Once: return "once";
        case core::schedule::RuleKind::Interval: return "interval";
        case core::schedule::RuleKind::Calendar: return "calendar";
        case core::schedule::RuleKind::Cron: return "cron";
        case core::schedule::RuleKind::Event: return "event";
        case core::schedule::RuleKind::Resident: return "resident";
    }
    return "resident";
}

core::Result<core::schedule::RuleKind> kindFromText(const std::string& text) {
    if (text == "once") return core::Result<core::schedule::RuleKind>::ok(core::schedule::RuleKind::Once);
    if (text == "interval") return core::Result<core::schedule::RuleKind>::ok(core::schedule::RuleKind::Interval);
    if (text == "calendar") return core::Result<core::schedule::RuleKind>::ok(core::schedule::RuleKind::Calendar);
    if (text == "cron") return core::Result<core::schedule::RuleKind>::ok(core::schedule::RuleKind::Cron);
    if (text == "event") return core::Result<core::schedule::RuleKind>::ok(core::schedule::RuleKind::Event);
    if (text == "resident") return core::Result<core::schedule::RuleKind>::ok(core::schedule::RuleKind::Resident);
    return core::Result<core::schedule::RuleKind>::fail(core::ErrorCode::ValidationError,
                                                        "未知规则类型: " + text);
}

core::Result<std::string> buildSpecJson(const EditFormState& f) {
    const auto failMsg = [](const std::string& m) {
        return core::Result<std::string>::fail(core::ErrorCode::ValidationError, m);
    };
    switch (f.kind) {
        case core::schedule::RuleKind::Once: {
            if (f.onceUseLocal) {
                return core::Result<std::string>::ok(
                    "{\"at_local\":\"" + escapeJsonString(f.onceAtLocal) + "\"}");
            }
            return core::Result<std::string>::ok("{\"at\":" + std::to_string(f.onceAtUnix) + "}");
        }
        case core::schedule::RuleKind::Interval: {
            std::string out = "{\"every_sec\":" + std::to_string(f.intervalEverySec);
            out += ",\"anchor\":" + std::to_string(f.intervalAnchor);
            out += ",\"window\":" + std::to_string(f.intervalWindow) + "}";
            return core::Result<std::string>::ok(out);
        }
        case core::schedule::RuleKind::Calendar:
            return buildCalendarJson(f);
        case core::schedule::RuleKind::Cron:
            return core::Result<std::string>::ok(
                "{\"expr\":\"" + escapeJsonString(f.cronExpr) + "\"}");
        case core::schedule::RuleKind::Event: {
            std::string out = "{\"events\":[";
            for (size_t i = 0; i < f.eventNames.size(); ++i) {
                if (i) out += ",";
                out += "\"" + escapeJsonString(f.eventNames[i]) + "\"";
            }
            out += "],\"cooldown_sec\":" + std::to_string(f.eventCooldownSec) + "}";
            return core::Result<std::string>::ok(out);
        }
        case core::schedule::RuleKind::Resident:
            return core::Result<std::string>::ok("{}");
    }
    return failMsg("未知规则类型");
}

core::Result<core::schedule::RuleSpec> validateAndParse(const EditFormState& f) {
    // 本层控件约束（红字文案针对控件形态，先于核心解析器）
    if (f.kind == core::schedule::RuleKind::Calendar) {
        if (f.calendarUnit == "week") {
            int y = 0, m = 0, d = 0;
            if (!parseDateText(f.calendarAnchorDate, y, m, d)) {
                return failValidation("锚定日期必须为真实存在的「YYYY-MM-DD」: " +
                                      f.calendarAnchorDate);
            }
            const int wday = weekdayOfDate(y, m, d);
            const bool matched =
                std::find(f.calendarWeekdays.begin(), f.calendarWeekdays.end(), wday) !=
                f.calendarWeekdays.end();
            if (!matched) {
                const char* kNames[7] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
                return failValidation("锚定日期 " + f.calendarAnchorDate + "（" +
                                      kNames[wday] + "）不在所选周几中，请调整锚定日期或勾选" +
                                      kNames[wday]);
            }
        }
        if ((f.calendarUnit == "month" || f.calendarUnit == "year") &&
            !f.calendarHasMonthDay && !f.calendarHasNthWeekday && !f.calendarLastWorkday) {
            return failValidation("每月规则必须选择「第 N 天」「第 N 个周几」「最后一个工作日」之一");
        }
    }
    if (f.actionKind != "remind" && f.actionKind != "remind_open" &&
        f.actionKind != "run_task" && f.actionKind != "webhook_only") {
        return failValidation("未知动作类型: " + f.actionKind);
    }
    if (f.missedPolicy != "once" && f.missedPolicy != "skip" && f.missedPolicy != "all") {
        return failValidation("未知错过策略: " + f.missedPolicy);
    }
    const auto specJson = buildSpecJson(f);
    if (specJson.isFail()) return failValidation(specJson.errorMessage());
    // 核心解析器全量校验（字段缺失/越界/互斥/未知字段，交接文档 §3 冻结契约）
    return core::schedule::parseRuleSpec(f.kind, specJson.value());
}

core::Result<std::vector<int64_t>> previewNextFires(const EditFormState& form,
                                                    int64_t afterUnix,
                                                    const core::schedule::FireContext& ctx) {
    const auto spec = validateAndParse(form);
    if (spec.isFail()) {
        return core::Result<std::vector<int64_t>>::fail(spec.errorCode(), spec.errorMessage());
    }
    std::vector<int64_t> out;
    int64_t cursor = afterUnix;
    for (int i = 0; i < 5; ++i) {
        const auto next = core::schedule::nextFire(spec.value(), cursor, ctx);
        if (next.isFail()) {
            return core::Result<std::vector<int64_t>>::fail(next.errorCode(), next.errorMessage());
        }
        if (!next.value().has_value()) break;  // 无下次时刻（一次性已过/event/resident）
        out.push_back(*next.value());
        cursor = *next.value();
    }
    return core::Result<std::vector<int64_t>>::ok(out);
}

core::Result<RuleRecord> buildRuleRecord(const EditFormState& form,
                                         const core::schedule::FireContext& ctx) {
    const auto spec = validateAndParse(form);
    if (spec.isFail()) {
        return core::Result<RuleRecord>::fail(spec.errorCode(), spec.errorMessage());
    }
    const auto specJson = buildSpecJson(form);
    if (specJson.isFail()) {
        return core::Result<RuleRecord>::fail(specJson.errorCode(), specJson.errorMessage());
    }
    RuleRecord rec;
    rec.name = form.name;
    rec.enabled = true;
    rec.kind = kindToText(form.kind);
    rec.specJson = specJson.value();
    rec.actionKind = form.actionKind;
    rec.actionJson = form.actionJson;
    rec.channelIds = form.channelIds;
    rec.missedPolicy = form.missedPolicy;
    // §4.4 编辑语义：保存载荷构建期重算 next_fire_at（事务落库由宿主模块包）
    const auto next = core::schedule::nextFire(spec.value(), ctx.nowUnix, ctx);
    if (next.isFail()) {
        return core::Result<RuleRecord>::fail(next.errorCode(), next.errorMessage());
    }
    rec.nextFireAt = next.value();
    return core::Result<RuleRecord>::ok(rec);
}

core::Result<EditFormState> formFromRecord(const RuleRecord& rec) {
    const auto kind = kindFromText(rec.kind);
    if (kind.isFail()) {
        return core::Result<EditFormState>::fail(kind.errorCode(), kind.errorMessage());
    }
    const auto spec = core::schedule::parseRuleSpec(kind.value(), rec.specJson);
    if (spec.isFail()) {
        return core::Result<EditFormState>::fail(spec.errorCode(), spec.errorMessage());
    }
    EditFormState f;
    f.kind = kind.value();
    f.name = rec.name;
    f.actionKind = rec.actionKind;
    f.actionJson = rec.actionJson;
    f.channelIds = rec.channelIds;
    f.missedPolicy = rec.missedPolicy;
    const auto& s = spec.value();
    switch (s.kind) {
        case core::schedule::RuleKind::Once:
            f.onceUseLocal = s.hasAtLocal;
            f.onceAtUnix = s.atUnix;
            if (s.hasAtLocal) {
                f.onceAtLocal = formatDate(s.atLocal.tm_year + 1900, s.atLocal.tm_mon + 1,
                                           s.atLocal.tm_mday) +
                                " " + formatTime(s.atLocal.tm_hour, s.atLocal.tm_min);
            }
            break;
        case core::schedule::RuleKind::Interval:
            f.intervalEverySec = s.everySec;
            f.intervalAnchor = s.anchor;
            f.intervalWindow = s.windowSec;
            break;
        case core::schedule::RuleKind::Calendar: {
            const auto& c = s.calendar;
            f.calendarUnit = (c.unit == core::schedule::CalendarSpec::Unit::Day)
                                 ? "day"
                                 : (c.unit == core::schedule::CalendarSpec::Unit::Week)
                                       ? "week"
                                       : (c.unit == core::schedule::CalendarSpec::Unit::Month)
                                             ? "month"
                                             : "year";
            f.calendarInterval = c.interval;
            f.calendarWeekdays.clear();
            for (int i = 0; i < 7; ++i) {
                if (c.weekdays[static_cast<size_t>(i)]) f.calendarWeekdays.push_back(i);
            }
            f.calendarWorkdaysOnly = c.workdaysOnly;
            f.calendarHasMonthDay = c.hasMonthDay;
            f.calendarMonthDay = c.monthDay;
            f.calendarHasNthWeekday = c.hasNthWeekday;
            f.calendarNth = c.nth;
            f.calendarNthWeekday = c.nthWeekday;
            f.calendarLastWorkday = c.lastWorkday;
            f.calendarHour = c.hour;
            f.calendarMinute = c.minute;
            // anchorDayNum → YYYY-MM-DD：civil_from_days 反推（daysFromCivil 的标准逆运算；
            // 必须用 z = days + 719468 平移原点，直接用 days 会得错年——2026-10-27 往返为证）
            {
                const int64_t z = c.anchorDayNum + 719468;
                const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
                const int64_t doe = z - era * 146097;
                const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
                const int64_t yy = yoe + era * 400;
                const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
                const int64_t mp = (5 * doy + 2) / 153;
                const int64_t dd = doy - (153 * mp + 2) / 5 + 1;
                const int64_t mm = mp + (mp < 10 ? 3 : -9);
                f.calendarAnchorDate =
                    formatDate(static_cast<int>(yy + (mm <= 2 ? 1 : 0)),
                               static_cast<int>(mm), static_cast<int>(dd));
            }
            break;
        }
        case core::schedule::RuleKind::Cron:
            // CronExpr 无稳定文本回显契约，保留原样文本需重读 spec_json；v1 编辑
            // 回填用「分 时 日 月 周」原串由宿主透传，这里置空并让控件显示提示。
            f.cronExpr.clear();
            break;
        case core::schedule::RuleKind::Event:
            f.eventNames = s.events;
            f.eventCooldownSec = s.cooldownSec;
            break;
        case core::schedule::RuleKind::Resident:
            break;
    }
    return core::Result<EditFormState>::ok(f);
}

// ---- 渲染 ----

namespace {

void drawKindTab(eui::Ui& ui, const std::string& id, float x, float w, float h,
                 const std::string& label, bool on, const core::theme::Tokens& t,
                 std::function<void()> onClick) {
    components::button(ui, id)
        .position(x, 0.0f)
        .size(w, h)
        .text(label)
        .fontSize(t.sizes.fontSizeSmall - 1.0f)
        .colors(on ? t.colors.primary : t.colors.background,
                on ? t.colors.primaryHover : t.colors.hover,
                on ? t.colors.primaryPressed : t.colors.primarySoftBg)
        .textColor(on ? t.colors.onPrimary : t.colors.textPrimary)
        .radius(8.0f)
        .border(1.0f, on ? t.colors.primary : t.colors.borderDefault)
        .onClick(std::move(onClick))
        .build();
}

void drawLabel(eui::Ui& ui, const std::string& id, float x, float y, const std::string& text,
               const core::theme::Tokens& t) {
    ui.text(id)
        .position(x, y)
        .size(200.0f, 16.0f)
        .text(text)
        .fontSize(t.sizes.fontSizeSmall - 1.0f)
        .lineHeight(16.0f)
        .color(t.colors.textTertiary)
        .build();
}

// 渠道多选按钮（选中 = 主色底白字；toggle 语义在 onClick 里做）
void drawChannelChip(eui::Ui& ui, const std::string& id, float x, float y, float w, float h,
                     const std::string& label, bool on, const core::theme::Tokens& t,
                     std::function<void()> onClick) {
    components::button(ui, id)
        .position(x, y)
        .size(w, h)
        .text(label)
        .fontSize(t.sizes.fontSizeSmall - 1.0f)
        .colors(on ? t.colors.primary : t.colors.background,
                on ? t.colors.primaryHover : t.colors.hover,
                on ? t.colors.primaryPressed : t.colors.primarySoftBg)
        .textColor(on ? t.colors.onPrimary : t.colors.textPrimary)
        .radius(14.0f)
        .border(1.0f, on ? t.colors.primary : t.colors.borderDefault)
        .onClick(std::move(onClick))
        .build();
}

}  // namespace

void drawEditDialog(eui::Ui& ui, const eui::Screen& screen, const core::theme::Tokens& t,
                    EditDialogState& st, const EditDialogHost& host,
                    const core::schedule::FireContext& ctx) {
    const auto themeTokens = core::theme::componentTokensWeChat(t);
    const float w = std::max(560.0f, screen.width * 0.5f);
    const float h = 640.0f;
    const float x0 = ui_kit::kDialogInsetX;
    const float innerW = w - ui_kit::kDialogBodyInsetX;

    components::dialog(ui, "schedule.edit.dialog")
        .open(st.open)
        .screen(screen.width, screen.height)
        .size(w, h)
        .theme(themeTokens)
        .zIndex(1000)
        .onOpenChange([&st](bool v) { st.open = v; })
        .content([&] {
            // 每帧按当前表单重算：校验红字 + 预览缓存同一真源（nextFire 迭代）
            st.form.errorText.clear();
            {
                const auto preview = previewNextFires(st.form, ctx.nowUnix, ctx);
                if (preview.isFail()) {
                    st.form.errorText = preview.errorMessage();
                } else {
                    st.previewCache = preview.value();
                }
            }
            const auto touch = [&st] { st.saveError.clear(); };

            ui_kit::drawDialogTitle(ui, "schedule.edit.title", w, "定时规则",
                                    20.0f, t.colors.textPrimary,
                                    {.y = 16.0f, .height = 28.0f, .lineHeight = 28.0f});

            // 类型页签（六 kind）
            const struct { core::schedule::RuleKind kind; const char* label; } kTabs[6] = {
                {core::schedule::RuleKind::Once, "一次性"},
                {core::schedule::RuleKind::Interval, "间隔"},
                {core::schedule::RuleKind::Calendar, "周期"},
                {core::schedule::RuleKind::Cron, "日程"},
                {core::schedule::RuleKind::Event, "事件"},
                {core::schedule::RuleKind::Resident, "常驻"},
            };
            const float tabGap = 6.0f;
            const float tabW = (innerW - tabGap * 5.0f) / 6.0f;
            for (int i = 0; i < 6; ++i) {
                drawKindTab(ui, "schedule.edit.kind." + std::to_string(i),
                            x0 + static_cast<float>(i) * (tabW + tabGap), tabW, 30.0f,
                            kTabs[i].label, st.form.kind == kTabs[i].kind, t,
                            [&st, touch, k = kTabs[i].kind] {
                                st.form.kind = k;
                                touch();
                            });
            }

            float fy = 96.0f;
            drawLabel(ui, "schedule.edit.name.label", x0, fy, "名称", t);
            components::input(ui, "schedule.edit.name")
                .position(x0, fy + 20.0f)
                .size(innerW, 36.0f)
                .value(st.form.name)
                .placeholder("规则名称")
                .theme(themeTokens)
                .onChange([&st, touch](const std::string& v) {
                    st.form.name = v;
                    touch();
                })
                .build();
            fy += 72.0f;

            // 各 kind 的参数行（占位布局：字段输入 + 语义标签）
            if (st.form.kind == core::schedule::RuleKind::Once) {
                drawLabel(ui, "schedule.edit.once.label", x0, fy, "触发时刻（YYYY-MM-DD HH:MM）", t);
                components::input(ui, "schedule.edit.once.at_local")
                    .position(x0, fy + 20.0f)
                    .size(innerW, 36.0f)
                    .value(st.form.onceAtLocal)
                    .theme(themeTokens)
                    .onChange([&st](const std::string& v) { st.form.onceAtLocal = v; })
                    .build();
                fy += 72.0f;
            } else if (st.form.kind == core::schedule::RuleKind::Interval) {
                drawLabel(ui, "schedule.edit.interval.label", x0, fy, "间隔秒数（≥1）", t);
                components::input(ui, "schedule.edit.interval.every_sec")
                    .position(x0, fy + 20.0f)
                    .size(innerW, 36.0f)
                    .value(std::to_string(st.form.intervalEverySec))
                    .theme(themeTokens)
                    .onChange([&st](const std::string& v) {
                        st.form.intervalEverySec = std::atoll(v.c_str());
                    })
                    .build();
                fy += 72.0f;
            } else if (st.form.kind == core::schedule::RuleKind::Calendar) {
                drawLabel(ui, "schedule.edit.calendar.unit.label", x0, fy, "周期单位", t);
                const struct { const char* key; const char* label; } kUnits[4] = {
                    {"day", "每天"}, {"week", "每周"}, {"month", "每月"}, {"year", "每年"},
                };
                for (int i = 0; i < 4; ++i) {
                    drawKindTab(ui, "schedule.edit.calendar.unit." + std::to_string(i),
                                x0 + static_cast<float>(i) * ((innerW / 4.0f)), innerW / 4.0f,
                                30.0f, kUnits[i].label, st.form.calendarUnit == kUnits[i].key, t,
                                [&st, k = std::string(kUnits[i].key)] {
                                    st.form.calendarUnit = k;
                                });
                }
                fy += 44.0f;
                // 每 N 周 [周几] HH:MM 控件形态
                if (st.form.calendarUnit == "week") {
                    drawLabel(ui, "schedule.edit.calendar.n.label", x0, fy, "每 N 周（N≥1）", t);
                    components::input(ui, "schedule.edit.calendar.interval")
                        .position(x0, fy + 20.0f)
                        .size(120.0f, 36.0f)
                        .value(std::to_string(st.form.calendarInterval))
                        .theme(themeTokens)
                        .onChange([&st](const std::string& v) {
                            st.form.calendarInterval = std::max(1, std::atoi(v.c_str()));
                        })
                        .build();
                    const char* kDayNames[7] = {"日", "一", "二", "三", "四", "五", "六"};
                    for (int i = 0; i < 7; ++i) {
                        const bool on = std::find(st.form.calendarWeekdays.begin(),
                                                  st.form.calendarWeekdays.end(),
                                                  i) != st.form.calendarWeekdays.end();
                        drawChannelChip(ui, "schedule.edit.calendar.weekday." + std::to_string(i),
                                        x0 + 140.0f + static_cast<float>(i) * 46.0f, fy + 20.0f,
                                        40.0f, 36.0f, std::string("周") + kDayNames[i], on, t,
                                        [&st, i] {
                                            auto& v = st.form.calendarWeekdays;
                                            const auto it = std::find(v.begin(), v.end(), i);
                                            if (it == v.end()) v.push_back(i);
                                            else v.erase(it);
                                        });
                    }
                    fy += 72.0f;
                    drawLabel(ui, "schedule.edit.calendar.anchor.label", x0, fy,
                              "锚定日期（其周几须在所选周几中）", t);
                    components::input(ui, "schedule.edit.calendar.anchor_date")
                        .position(x0, fy + 20.0f)
                        .size(innerW, 36.0f)
                        .value(st.form.calendarAnchorDate)
                        .theme(themeTokens)
                        .onChange([&st](const std::string& v) {
                            st.form.calendarAnchorDate = v;
                        })
                        .build();
                    fy += 72.0f;
                }
                if (st.form.calendarUnit == "month" || st.form.calendarUnit == "year") {
                    drawLabel(ui, "schedule.edit.calendar.monthday.label", x0, fy,
                              "第 N 天（1..31）", t);
                    components::input(ui, "schedule.edit.calendar.month_day")
                        .position(x0, fy + 20.0f)
                        .size(120.0f, 36.0f)
                        .value(std::to_string(st.form.calendarMonthDay))
                        .theme(themeTokens)
                        .onChange([&st](const std::string& v) {
                            st.form.calendarMonthDay = std::max(1, std::atoi(v.c_str()));
                            st.form.calendarHasMonthDay = true;
                            st.form.calendarHasNthWeekday = false;
                            st.form.calendarLastWorkday = false;
                        })
                        .build();
                    drawLabel(ui, "schedule.edit.calendar.nth.label", x0 + 140.0f, fy,
                              "第 N 个周几", t);
                    components::input(ui, "schedule.edit.calendar.nth")
                        .position(x0 + 140.0f, fy + 20.0f)
                        .size(60.0f, 36.0f)
                        .value(std::to_string(st.form.calendarNth))
                        .theme(themeTokens)
                        .onChange([&st](const std::string& v) {
                            st.form.calendarNth = std::max(1, std::atoi(v.c_str()));
                            st.form.calendarHasMonthDay = false;
                            st.form.calendarHasNthWeekday = true;
                            st.form.calendarLastWorkday = false;
                        })
                        .build();
                    fy += 72.0f;
                }
                if (st.form.calendarUnit == "day") {
                    drawLabel(ui, "schedule.edit.calendar.workdays.label", x0, fy,
                              "工作日（勾选 = 过滤周六日）", t);
                    const bool on = st.form.calendarWorkdaysOnly;
                    drawChannelChip(ui, "schedule.edit.calendar.workdays", x0, fy + 20.0f,
                                    120.0f, 36.0f, "仅工作日", on, t, [&st] {
                                        st.form.calendarWorkdaysOnly = !st.form.calendarWorkdaysOnly;
                                    });
                    fy += 72.0f;
                }
                drawLabel(ui, "schedule.edit.calendar.time.label", x0, fy, "时刻 HH:MM", t);
                components::input(ui, "schedule.edit.calendar.time")
                    .position(x0, fy + 20.0f)
                    .size(120.0f, 36.0f)
                    .value(formatTime(st.form.calendarHour, st.form.calendarMinute))
                    .theme(themeTokens)
                    .onChange([&st](const std::string& v) {
                        int h = 0, mi = 0;
                        if (parseTimeText(v, h, mi)) {
                            st.form.calendarHour = h;
                            st.form.calendarMinute = mi;
                        }
                    })
                    .build();
                fy += 72.0f;
            } else if (st.form.kind == core::schedule::RuleKind::Cron) {
                drawLabel(ui, "schedule.edit.cron.label", x0, fy, "cron 表达式（分 时 日 月 周）", t);
                components::input(ui, "schedule.edit.cron.expr")
                    .position(x0, fy + 20.0f)
                    .size(innerW, 36.0f)
                    .value(st.form.cronExpr)
                    .theme(themeTokens)
                    .onChange([&st](const std::string& v) { st.form.cronExpr = v; })
                    .build();
                fy += 72.0f;
            } else if (st.form.kind == core::schedule::RuleKind::Event) {
                std::string names;
                for (size_t i = 0; i < st.form.eventNames.size(); ++i) {
                    if (i) names += ",";
                    names += st.form.eventNames[i];
                }
                drawLabel(ui, "schedule.edit.event.label", x0, fy, "事件名（逗号分隔）", t);
                components::input(ui, "schedule.edit.event.names")
                    .position(x0, fy + 20.0f)
                    .size(innerW, 36.0f)
                    .value(names)
                    .theme(themeTokens)
                    .onChange([&st](const std::string& v) {
                        st.form.eventNames.clear();
                        std::string cur;
                        for (char c : v) {
                            if (c == ',') {
                                if (!cur.empty()) st.form.eventNames.push_back(cur);
                                cur.clear();
                            } else {
                                cur += c;
                            }
                        }
                        if (!cur.empty()) st.form.eventNames.push_back(cur);
                    })
                    .build();
                fy += 72.0f;
            }

            // 动作四选（§4.3）+ 错过策略三选（§4.6）
            drawLabel(ui, "schedule.edit.action.label", x0, fy, "动作", t);
            const struct { const char* key; const char* label; } kActions[4] = {
                {"remind", "仅提醒"}, {"remind_open", "提醒并打开"},
                {"run_task", "静默执行"}, {"webhook_only", "仅投递"},
            };
            for (int i = 0; i < 4; ++i) {
                drawKindTab(ui, "schedule.edit.action." + std::to_string(i),
                            x0 + static_cast<float>(i) * (innerW / 4.0f), innerW / 4.0f, 30.0f,
                            kActions[i].label, st.form.actionKind == kActions[i].key, t,
                            [&st, k = std::string(kActions[i].key)] {
                                st.form.actionKind = k;
                            });
            }
            fy += 44.0f;
            drawLabel(ui, "schedule.edit.policy.label", x0, fy, "错过策略", t);
            const struct { const char* key; const char* label; } kPolicies[3] = {
                {"once", "补一次"}, {"skip", "跳过"}, {"all", "全部补"},
            };
            for (int i = 0; i < 3; ++i) {
                drawKindTab(ui, "schedule.edit.policy." + std::to_string(i),
                            x0 + static_cast<float>(i) * (innerW / 3.0f), innerW / 3.0f, 30.0f,
                            kPolicies[i].label, st.form.missedPolicy == kPolicies[i].key, t,
                            [&st, k = std::string(kPolicies[i].key)] {
                                st.form.missedPolicy = k;
                            });
            }
            fy += 44.0f;

            // 渠道多选（列表项来自 host.channels = ChannelRegistry + schedule_channel）
            drawLabel(ui, "schedule.edit.channels.label", x0, fy, "通知渠道", t);
            {
                float cx = x0;
                for (size_t i = 0; i < host.channels.size(); ++i) {
                    const auto& ch = host.channels[i];
                    const bool on = std::find(st.form.channelIds.begin(),
                                              st.form.channelIds.end(),
                                              ch.id) != st.form.channelIds.end();
                    const float cw = 96.0f;
                    drawChannelChip(ui, "schedule.edit.channel." + std::to_string(i), cx,
                                    fy + 20.0f, cw, 36.0f, ch.label, on, t, [&st, ch] {
                                        auto& v = st.form.channelIds;
                                        const auto it = std::find(v.begin(), v.end(), ch.id);
                                        if (it == v.end()) v.push_back(ch.id);
                                        else v.erase(it);
                                    });
                    cx += cw + 8.0f;
                }
            }
            fy += 72.0f;

            // 实时预览接下来 5 次（经 nextFire 迭代；非法 spec 显示红字不落库）
            drawLabel(ui, "schedule.edit.preview.label", x0, fy, "接下来 5 次", t);
            {
                std::string lines;
                const std::string error = !st.saveError.empty() ? st.saveError : st.form.errorText;
                if (!error.empty()) {
                    lines = error;
                } else {
                    for (size_t i = 0; i < st.previewCache.size(); ++i) {
                        std::tm local{};
                        core::time::localTime(static_cast<std::time_t>(st.previewCache[i]), &local);
                        if (i) lines += "\n";
                        lines += formatDate(local.tm_year + 1900, local.tm_mon + 1,
                                            local.tm_mday) +
                                 " " + formatTime(local.tm_hour, local.tm_min);
                    }
                    if (st.previewCache.empty() && st.form.kind != core::schedule::RuleKind::Event &&
                        st.form.kind != core::schedule::RuleKind::Resident) {
                        lines = "无下次触发时刻";
                    }
                }
                ui.text("schedule.edit.preview")
                    .position(x0, fy + 20.0f)
                    .size(innerW, 88.0f)
                    .text(lines)
                    .fontSize(t.sizes.fontSizeSmall)
                    .lineHeight(18.0f)
                    .color(error.empty() ? t.colors.textSecondary : t.colors.errorText)
                    .build();
            }
            fy += 120.0f;

            // 保存 / 取消：保存前 validateAndParse + 事务内重算 next_fire_at（§4.4）
            components::button(ui, "schedule.edit.save")
                .position(x0, h - 60.0f)
                .size(120.0f, 36.0f)
                .text("保存")
                .colors(t.colors.primary, t.colors.primaryHover, t.colors.primaryPressed)
                .textColor(t.colors.onPrimary)
                .radius(8.0f)
                .onClick([&st, &host, &ctx] {
                    const auto rec = buildRuleRecord(st.form, ctx);
                    if (rec.isFail()) {
                        st.saveError = rec.errorMessage();
                        return;
                    }
                    const std::string err = host.saveRule(rec.value());
                    if (err.empty()) {
                        if (host.close) host.close();
                    } else {
                        st.saveError = err;
                    }
                })
                .build();
            components::button(ui, "schedule.edit.cancel")
                .position(x0 + 136.0f, h - 60.0f)
                .size(120.0f, 36.0f)
                .text("取消")
                .radius(8.0f)
                .border(1.0f, t.colors.borderDefault)
                .onClick([&host] {
                    if (host.close) host.close();
                })
                .build();
        })
        .build();
}

}  // namespace module::schedule::ui
