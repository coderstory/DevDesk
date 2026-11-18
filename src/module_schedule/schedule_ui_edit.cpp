// 定时调度编辑弹窗（波 S7b）：纯逻辑实现 + 渲染。
// 纯逻辑部分（spec_json 编解码/校验/预览/记录装配）不依赖 EUI 运行时，
// 由 tests/test_modules/test_schedule_ui_edit.cpp 无头直测；渲染部分
// （drawEditDialog）只做控件绘制与事件转发，落库走 EditDialogHost。
#include "schedule_ui_edit.h"

#include "core/component_theme.h"
#include "core/time.h"
#include "module_schedule/channels/wecom.h"
#include "platform/json.h"

#include <ui_kit/dialog_form.h>
#include <ui_kit/dialog_shell.h>

#include <components/button.h>
#include <components/dialog.h>
#include <components/input.h>
#include <components/scrollview.h>
#include <components/switch.h>

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

// action_json 安全取字符串字段（run_task key / remind_open deeplink 的回填真源）；
// 缺失/非对象/非串 → 空串，不抛。
std::string actionJsonField(const std::string& json, const char* key) {
    const auto parsed = platform::json::parse(json);
    if (parsed.isFail() || !parsed.value().isObject()) return "";
    const auto& obj = parsed.value().asObject();
    if (!obj.has(key)) return "";
    const auto v = obj.get(key);
    return v.isString() ? v.asString() : std::string();
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
    if (f.actionKind == "run_task") {
        // 键名唯一真源 = action_runner parseActionText 的 "key"（禁别名）；
        // 下拉禁自由输入，空 key 不许落库（触发即 NotFound 白跑一次）
        if (actionJsonField(f.actionJson, "key").empty()) {
            return failValidation("静默执行动作必须选择一个已注册任务");
        }
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
    rec.enabled = form.enabled;
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
    f.enabled = rec.enabled;
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
            // need290 cron-P0：编辑回填必须透传表达式原串（真源 = rec.specJson 的
            // "expr" 字段；RuleSpec.cron 是解析后结构，无原文）。前置 parseRuleSpec
            // 已成功 → expr 必在且合法；actionJsonField 缺失时兜底空串，由保存侧
            // 空表达式红字拦截兜住，不会再静默清空。
            f.cronExpr = actionJsonField(rec.specJson, "expr");
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

void drawKindTab(eui::Ui& ui, const std::string& id, float x, float y, float w, float h,
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
        .radius(t.sizes.radius)
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
        .radius(t.sizes.radius)
        .border(1.0f, on ? t.colors.primary : t.colors.borderDefault)
        .onClick(std::move(onClick))
        .build();
}

}  // namespace

void drawEditDialog(eui::Ui& ui, const eui::Screen& screen, const core::theme::Tokens& t,
                    EditDialogState& st, const EditDialogHost& host,
                    const core::schedule::FireContext& ctx) {
    const auto themeTokens = core::theme::componentTokensWeChat(t);
    const float h = 640.0f;

    // need294：迁标准弹窗壳（need156 规范）——头区标题/分隔线、底区细线+右锚按钮
    // 对（40 高 / min96 自适应）归壳；内容区 = (24,69,w-48,h-158)。宽由
    // max(560, screen*0.5) 归一为标准宽档 Wide=640。
    ui_kit::DialogShellSpec shell;
    shell.id = "schedule.edit.dialog";
    shell.width = ui_kit::DialogWidth::Wide;
    shell.height = h;
    shell.open = st.open;
    shell.title = "定时规则";
    // 取消/保存动作抽局部 std::function：底部按钮对与名称框键盘路径（need297
    // E2-a：Enter=保存同链 / Esc=取消，#156 §9）共用同一份逻辑，两入口不漂移。
    const std::function<void()> doCancel = [&host] {
        if (host.close) host.close();
    };
    // 保存 / 取消：保存前 validateAndParse + 事务内重算 next_fire_at（§4.4）
    const std::function<void()> doSave =
        // EUI-NEO 铁律：onClick 回调在点击帧由 updateInteraction 才调用
        //（compose 栈已展开），禁止引用捕获 compose 帧栈局部。ctx 是宿主
        // 每帧栈新对象（schedule_ui_list drawEditDialog 调用点），引用捕获
        // 即悬垂——保存时读出垃圾 tzId（乱码红字 + 拼串 bad_alloc 同根）。
        // 值捕获 = 构造期拷贝，回调帧内读自有副本。st/host 为宿主成员，
        // 生命周期覆盖回调，可引用捕获。（need290 修复，need294 迁壳原样搬入）
        [&st, &host, ctx] {
            if (st.form.actionKind == "webhook_only") {
                // 仅投递动作唯一外发渠道是企微：未勾选直接拦（防静默丢通知）
                bool wecom = false;
                for (const auto& ch : host.channels) {
                    if (ch.key == module::schedule::kChannelWecom &&
                        std::find(st.form.channelIds.begin(),
                                  st.form.channelIds.end(),
                                  ch.id) != st.form.channelIds.end()) {
                        wecom = true;
                        break;
                    }
                }
                if (!wecom) {
                    st.saveError = "仅投递动作必须勾选「企业微信」渠道";
                    return;
                }
            }
            const auto rec = buildRuleRecord(st.form, ctx);
            if (rec.isFail()) {
                st.saveError = rec.errorMessage();
                return;
            }
            RuleRecord out = rec.value();
            out.id = st.ruleId;  // 非 0 = 编辑既有规则（宿主走 updateRule）
            const std::string err = host.saveRule(out);
            if (err.empty()) {
                if (host.close) host.close();
            } else {
                st.saveError = err;
            }
        };
    shell.cancel = {"取消", doCancel};
    shell.confirm = {"保存", doSave};
    // need338：删除钉壳底区行左端（壳 thirdAction 左槽 + dangerThird 危险变
    // 体），几何随壳 clamp 后真实 h——旧手工钮按本函数 h=640 常量偏移，小视口
    // 壳钳高下命中区溢出到 backdrop（点背景=误触删除链，即 #338 症状）。仅编
    // 辑既有规则出现；点击链路不变：requestDelete(ruleId) → 宿主延帧开确认
    // 弹窗（need300 deferredDeleteId_ 机制原样保留）。
    if (st.ruleId != 0 && host.requestDelete) {
        shell.thirdAction = {
            "删除", [&st, &host] { host.requestDelete(st.ruleId); }};
        shell.dangerThird = true;
    }
    // need337：壳高随内容自适应（AW 方案）：provider 供自然内容高（上帧
    // contentBottom 实测、帧间自收敛），请求高 = 69 + 高 + 89；上限仍 640 档
    //（h−158 = 头 69 + 底 73 + 隙 16 投影为内容高上限），小视口由壳视口
    // clamp，内容被压缩时 content 内自包滚动（见下方 scrollView）。首帧未实测
    //（contentBottom=0）回退 640 档；spec.height 此时仅作无 provider 语义的
    // 兜底字段。
    shell.contentHeightProvider = [&st, h](float) {
        const float cap = h - (ui_kit::kDialogHeaderH + ui_kit::kDialogFooterH +
                               ui_kit::kDialogContentBottomGap);
        return st.contentBottom > 0.0f
            ? std::min(st.contentBottom - ui_kit::kDialogHeaderH, cap)
            : cap;
    };
    shell.onOpenChange = [&st](bool v) { st.open = v; };
    // need297 E2-a：打开沿自动聚焦名称框（requestFocus 帧末按元素 id 消费，
    // 本帧树中无该元素则丢弃）。只发一次——每帧重发会抢走用户移到其它输入框
    // 的焦点；关窗复位沿状态。
    if (st.open && !st.focusSeeded) {
        st.focusSeeded = true;
        ui.requestFocus("schedule.edit.name.hit");
    }
    if (!st.open) {
        st.focusSeeded = false;
    }
    ui_kit::drawDialogShell(ui, screen, shell, t, themeTokens,
                            [&](const ui_kit::DialogContentArea& area) {
        const float x0 = area.x;
        const float innerW = area.w;
        // need337：内容层包滚动区（同模块先例 schedule.list.scroll）：锚
        //（0, 56）页签行起（头线下 4px），底 = area.y + area.h = panelH−89
        //（panelH = 壳 clamp 后真实高，非本函数 640 常量）。滚动区占全宽，内容
        // x 维持面板绝对坐标；y 维改为相对锚（页签 0、fy 40 = 旧 56/96 − 56）。
        // 量高走上帧 contentBottom 实测（contentHeightProvider），不做 scratch
        // 重放——content 体沿用外层 ui 引用（实构图与滚动区为同一 Ui 实例，元素
        // 挂当前 open 容器 = 本滚动列，与既有壳 content 契约同构）。滚动条覆盖
        // 式不占内容宽（字段宽不随滚动条出现回缩）。
        const float viewTop = 56.0f;
        components::scrollView(ui, "schedule.edit.scroll")
            .position(0.0f, viewTop)
            .size(area.w + ui_kit::kDialogBodyInsetX, area.y + area.h - viewTop)
            .theme(themeTokens)
            .scrollbarOverlay(true)
            .contentHeightProvider(
                [&st](float, float) { return std::max(0.0f, st.contentBottom - 56.0f); })
            .content([&](eui::Ui&, float, float) {
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

            // 类型页签（六 kind）；y=0 相对滚动锚 56 = 面板绝对 56（压壳头区
            // 分隔线 52 下 4px，原语义不变）
            const struct { core::schedule::RuleKind kind; const char* label; } kTabs[6] = {
                {core::schedule::RuleKind::Once, "一次性"},
                {core::schedule::RuleKind::Interval, "间隔"},
                {core::schedule::RuleKind::Calendar, "周期"},
                {core::schedule::RuleKind::Cron, "表达式"},
                {core::schedule::RuleKind::Event, "事件"},
                {core::schedule::RuleKind::Resident, "常驻"},
            };
            const float tabGap = 6.0f;
            const float tabW = (innerW - tabGap * 5.0f) / 6.0f;
            for (int i = 0; i < 6; ++i) {
                drawKindTab(ui, "schedule.edit.kind." + std::to_string(i),
                            x0 + static_cast<float>(i) * (tabW + tabGap), 0.0f, tabW, 30.0f,
                            kTabs[i].label, st.form.kind == kTabs[i].kind, t,
                            [&st, touch, k = kTabs[i].kind] {
                                st.form.kind = k;
                                touch();
                            });
            }

            float fy = 40.0f;  // = 96−56：相对滚动锚（need337）
            // 启用开关（池140：表单首行「启用」，启停从卡面收进编辑弹窗一步直达）
            drawLabel(ui, "schedule.edit.enabled.label", x0, fy, "启用", t);
            components::toggleSwitch(ui, "schedule.edit.enabled")
                .position(x0 + 60.0f, fy + 2.0f)
                .size(44.0f, 24.0f)
                .checked(st.form.enabled)
                .theme(themeTokens)
                .onChange([&st](bool v) { st.form.enabled = v; })
                .build();
            fy += 44.0f;
            drawLabel(ui, "schedule.edit.name.label", x0, fy, "名称", t);
            components::input(ui, "schedule.edit.name")
                .position(x0, fy + 20.0f)
                .size(innerW, 36.0f)
                .value(st.form.name)
                .placeholder("规则名称")
                .fontSize(t.sizes.fontSizeBody)
                .theme(themeTokens)
                .onChange([&st, touch](const std::string& v) {
                    st.form.name = v;
                    touch();
                })
                // need297 E2-a（#156 §9）：名称框 Enter=保存同链、Esc=取消——
                // 与底部按钮对共用 doSave/doCancel，校验红字/落库失败行为一致
                .onEnter(doSave)
                .onCancel(doCancel)
                .build();
            fy += 72.0f;

            // 各 kind 的参数行（占位布局：字段输入 + 语义标签）
            if (st.form.kind == core::schedule::RuleKind::Once) {
                drawLabel(ui, "schedule.edit.once.label", x0, fy, "触发时刻（YYYY-MM-DD HH:MM）", t);
                components::input(ui, "schedule.edit.once.at_local")
                    .position(x0, fy + 20.0f)
                    .size(innerW, 36.0f)
                    .value(st.form.onceAtLocal)
                    .fontSize(t.sizes.fontSizeBody)
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
                    .fontSize(t.sizes.fontSizeBody)
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
                                x0 + static_cast<float>(i) * ((innerW / 4.0f)), fy + 20.0f, innerW / 4.0f,
                                30.0f, kUnits[i].label, st.form.calendarUnit == kUnits[i].key, t,
                                [&st, k = std::string(kUnits[i].key)] {
                                    st.form.calendarUnit = k;
                                });
                }
                fy += 56.0f;
                // 每 N 周 [周几] HH:MM 控件形态
                if (st.form.calendarUnit == "week") {
                    drawLabel(ui, "schedule.edit.calendar.n.label", x0, fy, "每 N 周（N≥1）", t);
                    components::input(ui, "schedule.edit.calendar.interval")
                        .position(x0, fy + 20.0f)
                        .size(120.0f, 36.0f)
                        .value(std::to_string(st.form.calendarInterval))
                        .fontSize(t.sizes.fontSizeBody)
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
                        .fontSize(t.sizes.fontSizeBody)
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
                        .fontSize(t.sizes.fontSizeBody)
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
                        .fontSize(t.sizes.fontSizeBody)
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
                    .fontSize(t.sizes.fontSizeBody)
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
                    .fontSize(t.sizes.fontSizeBody)
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
                    .fontSize(t.sizes.fontSizeBody)
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
                            x0 + static_cast<float>(i) * (innerW / 4.0f), fy + 20.0f, innerW / 4.0f, 30.0f,
                            kActions[i].label, st.form.actionKind == kActions[i].key, t,
                            [&st, k = std::string(kActions[i].key)] {
                                st.form.actionKind = k;
                            });
            }
            fy += 56.0f;

            // 动作载荷动态区（§4.3）：remind 无载荷；remind_open = deeplink 输入 +
            // 常用页面点选（页面标识[:参数]，点选即整串填入）；run_task = 注册表
            // key 点选，写 {"key":...}（禁自由输入，从控件层消灭键名错写）。
            if (st.form.actionKind == "remind_open") {
                drawLabel(ui, "schedule.edit.action.deeplink.label", x0, fy,
                          "打开页面（页面标识[:参数]）", t);
                components::input(ui, "schedule.edit.action.deeplink")
                    .position(x0, fy + 20.0f)
                    .size(innerW, 36.0f)
                    .value(actionJsonField(st.form.actionJson, "deeplink"))
                    .fontSize(t.sizes.fontSizeBody)
                    .theme(themeTokens)
                    .onChange([&st](const std::string& v) {
                        st.form.actionJson =
                            std::string("{\"deeplink\":\"") + escapeJsonString(v) + "\"}";
                    })
                    .build();
                fy += 72.0f;
                const char* kPages[] = {"home", "markdown", "todo",
                                        "claude", "agent", "settings"};
                float px = x0;
                for (const char* p : kPages) {
                    drawChannelChip(ui, std::string("schedule.edit.action.page.") + p,
                                    px, fy, 96.0f, 30.0f, p, false, t, [&st, p] {
                                        st.form.actionJson =
                                            std::string("{\"deeplink\":\"") + p + "\"}";
                                    });
                    px += 104.0f;
                }
                fy += 56.0f;
            } else if (st.form.actionKind == "run_task") {
                drawLabel(ui, "schedule.edit.action.task.label", x0, fy, "已注册任务", t);
                if (host.taskKeys.empty()) {
                    ui.text("schedule.edit.action.task.empty")
                        .position(x0, fy + 20.0f)
                        .size(innerW, 16.0f)
                        .text("暂无已注册任务（保存将被拦截）")
                        .fontSize(t.sizes.fontSizeSmall - 1.0f)
                        .color(t.colors.textTertiary)
                        .build();
                } else {
                    float tx = x0;
                    for (size_t i = 0; i < host.taskKeys.size(); ++i) {
                        const auto& k = host.taskKeys[i];
                        const bool on =
                            actionJsonField(st.form.actionJson, "key") == k;
                        drawChannelChip(ui,
                                        "schedule.edit.action.task." + std::to_string(i),
                                        tx, fy + 20.0f, 140.0f, 30.0f, k, on, t,
                                        [&st, k] {
                                            st.form.actionJson =
                                                "{\"key\":\"" + escapeJsonString(k) + "\"}";
                                        });
                        tx += 148.0f;
                    }
                }
                fy += 72.0f;
            } else if (st.form.actionKind == "remind") {
                drawLabel(ui, "schedule.edit.action.remind.label", x0, fy,
                          "提醒内容取规则名称，无附加参数", t);
                fy += 28.0f;
            }
            drawLabel(ui, "schedule.edit.policy.label", x0, fy, "错过策略", t);
            const struct { const char* key; const char* label; } kPolicies[3] = {
                {"once", "补一次"}, {"skip", "跳过"}, {"all", "全部补"},
            };
            for (int i = 0; i < 3; ++i) {
                drawKindTab(ui, "schedule.edit.policy." + std::to_string(i),
                            x0 + static_cast<float>(i) * (innerW / 3.0f), fy + 20.0f, innerW / 3.0f, 30.0f,
                            kPolicies[i].label, st.form.missedPolicy == kPolicies[i].key, t,
                            [&st, k = std::string(kPolicies[i].key)] {
                                st.form.missedPolicy = k;
                            });
            }
            fy += 56.0f;

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

            // need337：内容底回写 + 滚动列定高锚（masonry spacer 惯例：定位子
            // 项不参与列 wrap，以显式 spacer 定高 = 本帧真实内容底）。删除钮已
            // 迁壳 thirdAction 左槽 + dangerThird（need338，见上方 spec 装配处），
            // 本层不再手工画。
            st.contentBottom = viewTop + fy;  // 面板绝对底（壳高 provider 按 −69、
                                              // 滚动量高按 −56 消费，两处同源）
            ui.stack("schedule.edit.scroll.spacer")
                .size(innerW, fy)  // fy = 滚动列相对底 = spacer 高（定位子项不参与 wrap）
                .build();
            })
            .build();
        });
}

}  // namespace module::schedule::ui
