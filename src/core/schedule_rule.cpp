// core::schedule 规则引擎实现：六 kind 解析/校验 + nextFire/missedFires 纯函数。
// 推进纪律（不变量）：
//   - 日历日推进 = daysFromCivil 日序号 +1（civilFromDays 回算），mktime 只在
//     「墙钟 → Unix」与「Unix → 墙钟」换算时调用，禁止 +86400（DST 日 23h/25h 会偏）。
//   - 墙钟解析两遍（tm_isdst=1 / 0）：秋季回拨重复墙钟两次都还原一致 → 取更早（第一次）；
//     只有一边还原一致 → 取该边（无歧义日期的 isdst 试探失败边由墙钟比对滤掉）；
//     两边都不一致 = 春季跳变缺口 → 自请求时刻起逐分钟前探同日期内最早存在的墙钟（上限 6h）。
//   - nextFire 严格大于 afterUnix；missedFires 闭区间且按（本地日期, 时分）去重（cron 走
//     firstAndLastOccurrence 只产第一次出现）。
#include "schedule_rule.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <system_error>
#include <utility>

#include "time.h"
#include "yyjson.h"

namespace core::schedule {
namespace {

// ---------- JSON 取值辅助（yyjson；错误文案统一中文） ----------

bool checkKeys(yyjson_val* obj, std::initializer_list<const char*> allowed, std::string& err) {
    yyjson_val* key = nullptr;
    yyjson_val* val = nullptr;
    size_t idx = 0;
    size_t max = 0;
    yyjson_obj_foreach(obj, idx, max, key, val) {
        const char* k = yyjson_get_str(key);
        bool found = false;
        for (const char* a : allowed) {
            if (std::strcmp(k, a) == 0) { found = true; break; }
        }
        if (!found) {
            err = "存在未知字段「" + std::string(k) + "」";
            return false;
        }
    }
    return true;
}

bool getInt(yyjson_val* obj, const char* key, int64_t& out, std::string& err) {
    yyjson_val* v = yyjson_obj_get(obj, key);
    if (v == nullptr) {
        err = "缺少字段「" + std::string(key) + "」";
        return false;
    }
    if (yyjson_is_int(v)) { out = static_cast<int64_t>(yyjson_get_int(v)); return true; }
    if (yyjson_is_uint(v)) {
        const uint64_t u = yyjson_get_uint(v);
        if (u > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            err = "字段「" + std::string(key) + "」超出 64 位整数范围";
            return false;
        }
        out = static_cast<int64_t>(u);
        return true;
    }
    if (yyjson_is_real(v)) {
        out = static_cast<int64_t>(yyjson_get_real(v));
        return true;
    }
    err = "字段「" + std::string(key) + "」必须为数字";
    return false;
}

bool getBool(yyjson_val* obj, const char* key, bool& out, std::string& err) {
    yyjson_val* v = yyjson_obj_get(obj, key);
    if (v == nullptr) {
        err = "缺少字段「" + std::string(key) + "」";
        return false;
    }
    if (!yyjson_is_bool(v)) {
        err = "字段「" + std::string(key) + "」必须为布尔";
        return false;
    }
    out = yyjson_get_bool(v);
    return true;
}

bool getStr(yyjson_val* obj, const char* key, std::string& out, std::string& err) {
    yyjson_val* v = yyjson_obj_get(obj, key);
    if (v == nullptr) {
        err = "缺少字段「" + std::string(key) + "」";
        return false;
    }
    if (!yyjson_is_str(v)) {
        err = "字段「" + std::string(key) + "」必须为字符串";
        return false;
    }
    out.assign(yyjson_get_str(v), yyjson_get_len(v));
    return true;
}

bool hasKey(yyjson_val* obj, const char* key) {
    return yyjson_obj_get(obj, key) != nullptr;
}

// ---------- 日期/时刻字符串解析（严格：全数字、定长） ----------

bool parseDigits(const std::string& s, size_t begin, size_t len, int& out) {
    if (begin + len > s.size()) return false;
    int v = 0;
    const char* p = s.data() + begin;
    const auto res = std::from_chars(p, p + len, v);
    if (res.ec != std::errc() || res.ptr != p + len) return false;
    out = v;
    return true;
}

// "HH:MM"（长度 5，冒号定位于 2）
bool parseTimeOfDay(const std::string& s, int& hour, int& minute) {
    if (s.size() != 5 || s[2] != ':') return false;
    if (!parseDigits(s, 0, 2, hour)) return false;
    if (!parseDigits(s, 3, 2, minute)) return false;
    return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59;
}

// "YYYY-MM-DD"（长度 10，分隔符定位于 4/7）
bool parseDate(const std::string& s, int& year, int& month, int& day) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
    if (!parseDigits(s, 0, 4, year)) return false;
    if (!parseDigits(s, 5, 2, month)) return false;
    if (!parseDigits(s, 8, 2, day)) return false;
    return year >= 1900 && month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

unsigned daysInMonth(int year, unsigned month) {
    static const unsigned kTab[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2) {
        const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
        return leap ? 29u : 28u;
    }
    return kTab[month - 1];
}

// 公历日期真实性校验（parseDate 只查域，2 月 30 日在此拒绝）
bool validCivilDate(int year, int month, int day) {
    if (month < 1 || month > 12 || day < 1) return false;
    return static_cast<unsigned>(day) <= daysInMonth(year, static_cast<unsigned>(month));
}

// ---------- 日历日序号推进（Howard Hinnant 算法，零时区无关） ----------

void civilFromDays(int64_t z, int& year, unsigned& month, unsigned& day) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    year = static_cast<int>(yoe) + static_cast<int>(era) * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    day = doy - (153 * mp + 2) / 5 + 1;
    month = mp + (mp < 10 ? 3u : static_cast<unsigned>(-9));
    year += (month <= 2) ? 1 : 0;
}

int wdayOf(int64_t dayNum) {  // 0=周日（与 std::tm::tm_wday 一致；1970-01-01 为周四）
    const int64_t r = (dayNum % 7 + 7) % 7;
    return static_cast<int>((r + 4) % 7);
}

bool sameWall(const std::tm& a, const std::tm& b) {
    return a.tm_year == b.tm_year && a.tm_mon == b.tm_mon && a.tm_mday == b.tm_mday &&
           a.tm_hour == b.tm_hour && a.tm_min == b.tm_min;
}

// ---------- 墙钟 → Unix 解析（DST 语义核心） ----------

// 请求墙钟在该日存在两次（秋季回拨）时 first/last 为两次出现的 Unix 秒（first 更早）；
// 只存在一次时两者相等；都不存在（春季跳变缺口）返回 false。want 的 isdst 忽略。
bool firstAndLastOccurrence(std::tm want, int64_t& first, int64_t& last) {
    std::tm t1 = want;
    t1.tm_isdst = 1;
    const int64_t u1 = static_cast<int64_t>(std::mktime(&t1));
    std::tm w1{};
    core::time::localTime(static_cast<std::time_t>(u1), &w1);
    std::tm t2 = want;
    t2.tm_isdst = 0;
    const int64_t u2 = static_cast<int64_t>(std::mktime(&t2));
    std::tm w2{};
    core::time::localTime(static_cast<std::time_t>(u2), &w2);
    const bool m1 = sameWall(want, w1);
    const bool m2 = sameWall(want, w2);
    if (m1 && m2) {
        first = u1 < u2 ? u1 : u2;
        last = u1 < u2 ? u2 : u1;
        return true;
    }
    if (m1) { first = last = u1; return true; }
    if (m2) { first = last = u2; return true; }
    return false;
}

// want 墙钟 → 触发时刻：唯一/两次取第一次/缺口兜底「同日期最早存在时刻」。
// 返回 false = 该墙钟在该日不存在且缺口兜底失败（调用方跳过该日）。
bool resolveWallClock(std::tm want, int64_t& out) {
    int64_t first = 0;
    int64_t last = 0;
    if (firstAndLastOccurrence(want, first, last)) {
        out = first;
        return true;
    }
    // 春季跳变缺口：自请求时刻逐分钟前探，同日期内第一个真实存在的墙钟（上限 6 小时，
    // 覆盖历史 30/60/120 分钟跳变；探不到（缺口跨日等病态）跳过该日）。
    // 比较用归一化后的目标墙钟：tm_min += m 不归一（如 02:30+30 → 02:60），直接比会永远不中。
    for (int m = 1; m <= 360; ++m) {
        const int totalMin = want.tm_hour * 60 + want.tm_min + m;
        const int normHour = totalMin / 60;
        const int normMin = totalMin % 60;
        if (normHour >= 24) return false;  // 缺口跨零点：放弃该日
        std::tm t = want;
        t.tm_hour = normHour;
        t.tm_min = normMin;
        t.tm_isdst = -1;
        const int64_t u = static_cast<int64_t>(std::mktime(&t));
        std::tm w{};
        core::time::localTime(static_cast<std::time_t>(u), &w);
        if (w.tm_year == want.tm_year && w.tm_mon == want.tm_mon && w.tm_mday == want.tm_mday &&
            w.tm_hour == normHour && w.tm_min == normMin && w.tm_sec == 0) {
            out = u;
            return true;
        }
    }
    return false;
}

// ---------- 各 kind 解析 ----------

Result<RuleSpec> parseOnce(yyjson_val* obj) {
    std::string err;
    if (!checkKeys(obj, {"at", "at_local"}, err)) {
        return Result<RuleSpec>::fail(ErrorCode::ValidationError, "once 规则" + err);
    }
    const bool hasAt = hasKey(obj, "at");
    const bool hasLocal = hasKey(obj, "at_local");
    if (hasAt == hasLocal) {
        return Result<RuleSpec>::fail(ErrorCode::ValidationError,
                                      "once 规则必须且只能提供 at 或 at_local 之一");
    }
    RuleSpec s;
    s.kind = RuleKind::Once;
    if (hasAt) {
        if (!getInt(obj, "at", s.atUnix, err)) {
            return Result<RuleSpec>::fail(ErrorCode::ValidationError, "once.at 非法：" + err);
        }
        s.hasAt = true;
        return Result<RuleSpec>::ok(std::move(s));
    }
    std::string text;
    if (!getStr(obj, "at_local", text, err)) {
        return Result<RuleSpec>::fail(ErrorCode::ValidationError, "once.at_local 非法：" + err);
    }
    // "YYYY-MM-DD HH:MM" 或 "YYYY-MM-DDTHH:MM"
    const size_t sep = text.find(' ');
    const size_t tsep = text.find('T');
    const size_t cut = sep != std::string::npos ? sep : tsep;
    if (cut == std::string::npos) {
        return Result<RuleSpec>::fail(ErrorCode::ValidationError,
                                      "once.at_local 必须为「YYYY-MM-DD HH:MM」或「YYYY-MM-DDTHH:MM」：" + text);
    }
    const std::string datePart = text.substr(0, cut);
    const std::string timePart = text.substr(cut + 1);
    int year = 0, month = 0, day = 0, hour = 0, minute = 0;
    if (!parseDate(datePart, year, month, day) || !validCivilDate(year, month, day) ||
        !parseTimeOfDay(timePart, hour, minute)) {
        return Result<RuleSpec>::fail(ErrorCode::ValidationError,
                                      "once.at_local 非法日期或时刻（YYYY-MM-DD HH:MM，日期须真实存在）：" + text);
    }
    std::tm local{};
    local.tm_year = year - 1900;
    local.tm_mon = month - 1;
    local.tm_mday = day;
    local.tm_hour = hour;
    local.tm_min = minute;
    local.tm_sec = 0;
    s.atLocal = local;
    s.hasAtLocal = true;
    return Result<RuleSpec>::ok(std::move(s));
}

Result<RuleSpec> parseInterval(yyjson_val* obj) {
    std::string err;
    if (!checkKeys(obj, {"every_sec", "anchor", "window"}, err)) {
        return Result<RuleSpec>::fail(ErrorCode::ValidationError, "interval 规则" + err);
    }
    RuleSpec s;
    s.kind = RuleKind::Interval;
    if (!getInt(obj, "every_sec", s.everySec, err) || s.everySec < 1) {
        return Result<RuleSpec>::fail(ErrorCode::ValidationError,
                                      "interval.every_sec 必须为 ≥1 的整数");
    }
    if (hasKey(obj, "anchor")) {
        if (!getInt(obj, "anchor", s.anchor, err) || s.anchor < 0) {
            return Result<RuleSpec>::fail(ErrorCode::ValidationError,
                                          "interval.anchor 必须为 ≥0 的整数");
        }
    }
    if (hasKey(obj, "window")) {
        if (!getInt(obj, "window", s.windowSec, err) || s.windowSec < 0 || s.windowSec >= s.everySec) {
            return Result<RuleSpec>::fail(ErrorCode::ValidationError,
                                          "interval.window 必须为 ≥0 且 < every_sec 的整数");
        }
    }
    return Result<RuleSpec>::ok(std::move(s));
}

// 校验并回填日历规则的跨字段约束（unit 与选择器/锚点的组合合法性）
Result<RuleSpec> parseCalendar(yyjson_val* obj) {
    std::string err;
    if (!checkKeys(obj, {"unit", "interval", "weekdays", "month_day", "nth_weekday",
                         "last_workday", "time", "anchor_date", "workdays_only"}, err)) {
        return Result<RuleSpec>::fail(ErrorCode::ValidationError, "calendar 规则" + err);
    }
    RuleSpec s;
    s.kind = RuleKind::Calendar;
    CalendarSpec& c = s.calendar;
    std::string unitText;
    if (!getStr(obj, "unit", unitText, err)) {
        return Result<RuleSpec>::fail(ErrorCode::ValidationError, "calendar.unit " + err);
    }
    if (unitText == "day") c.unit = CalendarSpec::Unit::Day;
    else if (unitText == "week") c.unit = CalendarSpec::Unit::Week;
    else if (unitText == "month") c.unit = CalendarSpec::Unit::Month;
    else if (unitText == "year") c.unit = CalendarSpec::Unit::Year;
    else {
        return Result<RuleSpec>::fail(ErrorCode::ValidationError,
                                      "calendar.unit 必须为 day/week/month/year：" + unitText);
    }
    if (hasKey(obj, "interval")) {
        int64_t iv = 0;
        if (!getInt(obj, "interval", iv, err) || iv < 1 || iv > 1000000) {
            return Result<RuleSpec>::fail(ErrorCode::ValidationError,
                                          "calendar.interval 必须为 ≥1 且 ≤1000000 的整数");
        }
        c.interval = static_cast<int>(iv);
    }
    if (hasKey(obj, "workdays_only")) {
        if (!getBool(obj, "workdays_only", c.workdaysOnly, err)) {
            return Result<RuleSpec>::fail(ErrorCode::ValidationError, "calendar.workdays_only " + err);
        }
    }
    if (hasKey(obj, "weekdays")) {
        yyjson_val* arr = yyjson_obj_get(obj, "weekdays");
        if (!yyjson_is_arr(arr)) {
            return Result<RuleSpec>::fail(ErrorCode::ValidationError,
                                          "calendar.weekdays 必须为 0..6 的整数数组");
        }
        size_t idx = 0, max = 0;
        yyjson_val* v = nullptr;
        yyjson_arr_foreach(arr, idx, max, v) {
            if (!yyjson_is_int(v)) {
                return Result<RuleSpec>::fail(ErrorCode::ValidationError,
                                              "calendar.weekdays 元素必须为 0..6 的整数");
            }
            const int d = yyjson_get_int(v);
            if (d < 0 || d > 6) {
                return Result<RuleSpec>::fail(ErrorCode::ValidationError,
                                              "calendar.weekdays 元素必须为 0..6 的整数（0=周日）");
            }
            c.weekdays[static_cast<size_t>(d)] = true;
        }
    }
    if (hasKey(obj, "month_day")) {
        int64_t md = 0;
        if (!getInt(obj, "month_day", md, err) || md < 1 || md > 31) {
            return Result<RuleSpec>::fail(ErrorCode::ValidationError,
                                          "calendar.month_day 必须为 1..31 的整数");
        }
        c.hasMonthDay = true;
        c.monthDay = static_cast<int>(md);
    }
    if (hasKey(obj, "nth_weekday")) {
        yyjson_val* nw = yyjson_obj_get(obj, "nth_weekday");
        if (!yyjson_is_obj(nw)) {
            return Result<RuleSpec>::fail(ErrorCode::ValidationError,
                                          "calendar.nth_weekday 必须为 {\"n\":1..5,\"weekday\":0..6} 对象");
        }
        int64_t n = 0, wd = 0;
        if (!getInt(nw, "n", n, err) || n < 1 || n > 5) {
            return Result<RuleSpec>::fail(ErrorCode::ValidationError,
                                          "calendar.nth_weekday.n 必须为 1..5 的整数");
        }
        if (!getInt(nw, "weekday", wd, err) || wd < 0 || wd > 6) {
            return Result<RuleSpec>::fail(ErrorCode::ValidationError,
                                          "calendar.nth_weekday.weekday 必须为 0..6 的整数");
        }
        c.hasNthWeekday = true;
        c.nth = static_cast<int>(n);
        c.nthWeekday = static_cast<int>(wd);
    }
    if (hasKey(obj, "last_workday")) {
        if (!getBool(obj, "last_workday", c.lastWorkday, err)) {
            return Result<RuleSpec>::fail(ErrorCode::ValidationError, "calendar.last_workday " + err);
        }
    }
    std::string timeText;
    if (!getStr(obj, "time", timeText, err) || !parseTimeOfDay(timeText, c.hour, c.minute)) {
        return Result<RuleSpec>::fail(ErrorCode::ValidationError,
                                      "calendar.time 必须为「HH:MM」");
    }
    const bool isDayWeek = c.unit == CalendarSpec::Unit::Day || c.unit == CalendarSpec::Unit::Week;
    if (isDayWeek) {
        std::string anchorText;
        if (!getStr(obj, "anchor_date", anchorText, err)) {
            return Result<RuleSpec>::fail(ErrorCode::ValidationError,
                                          "calendar 规则 unit=day/week 必须提供 anchor_date（相位第 0 天/周）");
        }
        int y = 0, m = 0, d = 0;
        if (!parseDate(anchorText, y, m, d) || !validCivilDate(y, m, d)) {
            return Result<RuleSpec>::fail(ErrorCode::ValidationError,
                                          "calendar.anchor_date 必须为真实存在的「YYYY-MM-DD」：" + anchorText);
        }
        c.anchorDayNum = core::time::daysFromCivil(y, static_cast<unsigned>(m), static_cast<unsigned>(d));
    } else {
        if (hasKey(obj, "anchor_date")) {
            return Result<RuleSpec>::fail(ErrorCode::ValidationError,
                                          "calendar.anchor_date 仅 unit=day/week 使用");
        }
    }
    if (c.unit == CalendarSpec::Unit::Week) {
        bool any = false;
        for (bool b : c.weekdays) { if (b) { any = true; break; } }
        if (!any) {
            return Result<RuleSpec>::fail(ErrorCode::ValidationError,
                                          "calendar 规则 unit=week 必须提供非空 weekdays[]");
        }
        const int anchorWday = wdayOf(c.anchorDayNum);
        if (!c.weekdays[static_cast<size_t>(anchorWday)]) {
            return Result<RuleSpec>::fail(ErrorCode::ValidationError,
                                          "calendar.anchor_date 所在星期必须命中 weekdays[]（周相位以锚点周为第 0 周）");
        }
    }
    if (c.unit != CalendarSpec::Unit::Week && hasKey(obj, "weekdays")) {
        return Result<RuleSpec>::fail(ErrorCode::ValidationError,
                                      "calendar.weekdays 仅 unit=week 使用");
    }
    const bool hasSelector = c.hasMonthDay || c.hasNthWeekday || c.lastWorkday;
    if (c.unit == CalendarSpec::Unit::Month || c.unit == CalendarSpec::Unit::Year) {
        if (!hasSelector) {
            return Result<RuleSpec>::fail(ErrorCode::ValidationError,
                                          "calendar 规则 unit=month/year 必须提供 month_day / nth_weekday / last_workday 之一");
        }
        const int selectors = (c.hasMonthDay ? 1 : 0) + (c.hasNthWeekday ? 1 : 0) + (c.lastWorkday ? 1 : 0);
        if (selectors > 1) {
            return Result<RuleSpec>::fail(ErrorCode::ValidationError,
                                          "calendar.month_day / nth_weekday / last_workday 三者互斥");
        }
    } else if (hasSelector) {
        return Result<RuleSpec>::fail(ErrorCode::ValidationError,
                                      "calendar.month_day / nth_weekday / last_workday 仅 unit=month/year 使用");
    }
    return Result<RuleSpec>::ok(std::move(s));
}

Result<RuleSpec> parseCronKind(yyjson_val* obj) {
    std::string err;
    if (!checkKeys(obj, {"expr"}, err)) {
        return Result<RuleSpec>::fail(ErrorCode::ValidationError, "cron 规则" + err);
    }
    RuleSpec s;
    s.kind = RuleKind::Cron;
    std::string expr;
    if (!getStr(obj, "expr", expr, err)) {
        return Result<RuleSpec>::fail(ErrorCode::ValidationError, "cron.expr " + err);
    }
    auto parsed = core::cron::parseCron(expr);
    if (parsed.isFail()) {
        return Result<RuleSpec>::fail(ErrorCode::ValidationError,
                                      "cron.expr 非法：" + parsed.errorMessage());
    }
    s.cron = parsed.value();
    return Result<RuleSpec>::ok(std::move(s));
}

Result<RuleSpec> parseEvent(yyjson_val* obj) {
    std::string err;
    if (!checkKeys(obj, {"events", "cooldown_sec"}, err)) {
        return Result<RuleSpec>::fail(ErrorCode::ValidationError, "event 规则" + err);
    }
    RuleSpec s;
    s.kind = RuleKind::Event;
    if (hasKey(obj, "events")) {
        yyjson_val* arr = yyjson_obj_get(obj, "events");
        if (!yyjson_is_arr(arr)) {
            return Result<RuleSpec>::fail(ErrorCode::ValidationError,
                                          "event.events 必须为字符串数组");
        }
        size_t idx = 0, max = 0;
        yyjson_val* v = nullptr;
        yyjson_arr_foreach(arr, idx, max, v) {
            if (!yyjson_is_str(v)) {
                return Result<RuleSpec>::fail(ErrorCode::ValidationError,
                                              "event.events 元素必须为字符串");
            }
            s.events.emplace_back(yyjson_get_str(v), yyjson_get_len(v));
        }
    }
    if (hasKey(obj, "cooldown_sec")) {
        if (!getInt(obj, "cooldown_sec", s.cooldownSec, err) || s.cooldownSec < 0) {
            return Result<RuleSpec>::fail(ErrorCode::ValidationError,
                                          "event.cooldown_sec 必须为 ≥0 的整数");
        }
    }
    return Result<RuleSpec>::ok(std::move(s));
}

// ---------- 时区闸（v1 只本地；ctx 除 tzId 外仅作快照，换算全以实参为准） ----------

Result<void> checkZone(const FireContext& ctx) {
    if (!ctx.tzId.empty()) {
        return Result<void>::fail(ErrorCode::NotImplemented,
                                  "v1 仅支持系统本地时区（tzId 非空未支持：" + ctx.tzId + "）");
    }
    return Result<void>::ok();
}

// ---------- 日历匹配 ----------

// 日选择器（unit=month 当月内 / unit=year 1 月内）
bool monthDaySelects(const CalendarSpec& c, int year, unsigned month, unsigned day, int wday) {
    if (c.hasMonthDay) return static_cast<int>(day) == c.monthDay;
    if (c.hasNthWeekday) {
        if (wday != c.nthWeekday) return false;
        return (static_cast<int>(day) - 1) / 7 + 1 == c.nth;  // 本月第几个该周几
    }
    if (c.lastWorkday) {
        unsigned last = daysInMonth(year, month);
        int lw = wdayOf(core::time::daysFromCivil(year, month, last));
        while (lw == 0 || lw == 6) {
            --last;
            lw = (lw + 6) % 7;
        }
        return day == last;
    }
    return false;
}

bool calendarDayMatches(const CalendarSpec& c, int year, unsigned month, unsigned day, int wday) {
    const bool isWorkday = wday != 0 && wday != 6;
    switch (c.unit) {
        case CalendarSpec::Unit::Day: {
            if (c.workdaysOnly && !isWorkday) return false;
            const int64_t dd = core::time::daysFromCivil(year, month, day) - c.anchorDayNum;
            return dd >= 0 && dd % c.interval == 0;
        }
        case CalendarSpec::Unit::Week: {
            if (c.workdaysOnly && !isWorkday) return false;
            if (!c.weekdays[static_cast<size_t>(wday)]) return false;
            const int64_t dd = core::time::daysFromCivil(year, month, day) - c.anchorDayNum;
            if (dd < 0) return false;
            return (dd / 7) % c.interval == 0;
        }
        case CalendarSpec::Unit::Month: {
            if (c.workdaysOnly && !isWorkday) return false;
            const int64_t months = (static_cast<int64_t>(year) - 1970) * 12 +
                                   (static_cast<int64_t>(month) - 1);
            if (months < 0 || months % c.interval != 0) return false;
            return monthDaySelects(c, year, month, day, wday);
        }
        case CalendarSpec::Unit::Year: {
            if (c.workdaysOnly && !isWorkday) return false;
            if (month != 1) return false;  // v1：年度规则日选择器只应用于 1 月
            if ((year - 1970) % c.interval != 0) return false;
            return monthDaySelects(c, year, month, day, wday);
        }
    }
    return false;
}

// 本地日期起点：ctx.localNow 仅当 afterUnix == ctx.nowUnix 时作快照（引擎同一 tick 已换算）
void localDateStart(int64_t afterUnix, const FireContext& ctx, int64_t& dayNum) {
    std::tm wall{};
    if (afterUnix == ctx.nowUnix) {
        wall = ctx.localNow;
    } else {
        core::time::localTime(static_cast<std::time_t>(afterUnix), &wall);
    }
    dayNum = core::time::daysFromCivil(wall.tm_year + 1900, static_cast<unsigned>(wall.tm_mon) + 1,
                                       static_cast<unsigned>(wall.tm_mday));
}

Result<std::optional<int64_t>> calendarNextFire(const RuleSpec& spec, int64_t afterUnix,
                                                const FireContext& ctx) {
    const CalendarSpec& c = spec.calendar;
    int64_t dayNum = 0;
    localDateStart(afterUnix, ctx, dayNum);
    constexpr int64_t kMaxDays = 146097;  // 400 年覆盖任何相位间隔
    for (int64_t i = 0; i < kMaxDays; ++i, ++dayNum) {
        int y = 0;
        unsigned m = 0, d = 0;
        civilFromDays(dayNum, y, m, d);
        const int wday = wdayOf(dayNum);
        if (!calendarDayMatches(c, y, m, d, wday)) continue;
        std::tm want{};
        want.tm_year = y - 1900;
        want.tm_mon = static_cast<int>(m) - 1;
        want.tm_mday = static_cast<int>(d);
        want.tm_hour = c.hour;
        want.tm_min = c.minute;
        want.tm_sec = 0;
        int64_t u = 0;
        if (!resolveWallClock(want, u)) continue;  // 缺口兜底失败（病态时区），跳过该日
        if (u > afterUnix) return Result<std::optional<int64_t>>::ok(std::optional<int64_t>(u));
    }
    // 契约（schedule_rule.h）：无下次一律 ok(nullopt)，fail 只留给参数非法——与
    // cron 8 窗口无匹配同一口径。引擎把 fail/nullopt 都当休眠，但 UI 保存
    // （buildRuleRecord）把 fail 当拒绝，会误拒合法稀疏 interval（day interval
    // =1000000 首触发在 400 年外）与病态组合（nth=5 周日 + workdays_only 恒空）。
    return Result<std::optional<int64_t>>::ok(std::optional<int64_t>());
}

// ---------- cron kind ----------

// cron 规则下一次触发（严格 > fromSec；重复墙钟只返回第一次出现）。
// nextCronMatch 单窗口 366 天：稀疏表达式（如 2 月 29 日，间隔最长 8 年）需多窗口续扫，
// 共 8 窗口 ≈ 8 年。引擎应缓存 next_fire_at，只在到点后重算。
Result<std::optional<int64_t>> cronNextFireInternal(const RuleSpec& spec, int64_t fromSec) {
    int64_t scan = fromSec;
    for (int round = 0; round < 8; ++round) {
        auto r = core::cron::nextCronMatch(spec.cron, static_cast<std::time_t>(scan));
        if (r.isFail()) {
            scan += 366LL * 86400;
            continue;
        }
        std::tm wall = r.value();
        wall.tm_sec = 0;
        int64_t first = 0, last = 0;
        if (!firstAndLastOccurrence(wall, first, last)) {
            // nextCronMatch 只产真实存在的墙钟，理论不可达；防御性越过该分钟继续
            scan = std::max(scan + 60, last);
            continue;
        }
        if (first > fromSec) return Result<std::optional<int64_t>>::ok(std::optional<int64_t>(first));
        scan = last;  // 第一次已过：从第二次出现之后继续（秋季回拨不重复触发）
    }
    return Result<std::optional<int64_t>>::ok(std::optional<int64_t>());
}

// ---------- nextFire / missedFires 分派 ----------

Result<int64_t> onceFireUnix(const RuleSpec& spec, int64_t& out) {
    if (spec.hasAt) {
        out = spec.atUnix;
        return Result<int64_t>::ok(out);
    }
    std::tm want = spec.atLocal;
    if (!resolveWallClock(want, out)) {
        return Result<int64_t>::fail(ErrorCode::ValidationError,
                                     "once.at_local 时刻在该日不存在（DST 缺口兜底失败）");
    }
    return Result<int64_t>::ok(out);
}

} // namespace

Result<RuleSpec> parseRuleSpec(RuleKind kind, const std::string& specJson) {
    if (specJson.empty()) {
        return Result<RuleSpec>::fail(ErrorCode::ValidationError, "spec_json 为空");
    }
    yyjson_doc* doc = yyjson_read(specJson.data(), specJson.size(), 0);
    if (doc == nullptr) {
        return Result<RuleSpec>::fail(ErrorCode::ValidationError, "spec_json 非法 JSON：" + specJson);
    }
    struct DocGuard {
        yyjson_doc* d;
        ~DocGuard() { if (d != nullptr) yyjson_doc_free(d); }
    } guard{doc};
    yyjson_val* root = yyjson_doc_get_root(doc);
    if (root == nullptr || !yyjson_is_obj(root)) {
        return Result<RuleSpec>::fail(ErrorCode::ValidationError, "spec_json 根必须是 JSON 对象");
    }
    switch (kind) {
        case RuleKind::Once: return parseOnce(root);
        case RuleKind::Interval: return parseInterval(root);
        case RuleKind::Calendar: return parseCalendar(root);
        case RuleKind::Cron: return parseCronKind(root);
        case RuleKind::Event: return parseEvent(root);
        case RuleKind::Resident: {
            RuleSpec s;
            s.kind = RuleKind::Resident;
            std::string err;
            if (!checkKeys(root, {}, err)) {
                return Result<RuleSpec>::fail(ErrorCode::ValidationError,
                                              "resident 规则" + err);
            }
            return Result<RuleSpec>::ok(std::move(s));
        }
    }
    return Result<RuleSpec>::fail(ErrorCode::ValidationError, "未知规则 kind");
}

Result<std::optional<int64_t>> nextFire(const RuleSpec& spec, int64_t afterUnix,
                                        const FireContext& ctx) {
    auto zone = checkZone(ctx);
    if (zone.isFail()) {
        return Result<std::optional<int64_t>>::fail(zone.errorCode(), zone.errorMessage());
    }
    switch (spec.kind) {
        case RuleKind::Once: {
            int64_t u = 0;
            auto r = onceFireUnix(spec, u);
            if (r.isFail()) return Result<std::optional<int64_t>>::fail(r.errorCode(), r.errorMessage());
            if (u > afterUnix) return Result<std::optional<int64_t>>::ok(std::optional<int64_t>(u));
            return Result<std::optional<int64_t>>::ok(std::optional<int64_t>());
        }
        case RuleKind::Interval: {
            const int64_t N = spec.everySec;
            const int64_t A = spec.anchor;
            if (afterUnix < 0 || afterUnix < A) {
                return Result<std::optional<int64_t>>::ok(std::optional<int64_t>(A));
            }
            const int64_t diff = afterUnix - A;
            int64_t k = diff / N;
            if (diff % N != 0) ++k;
            if (k > (std::numeric_limits<int64_t>::max() - A) / N) {
                return Result<std::optional<int64_t>>::ok(std::optional<int64_t>());  // 越界：无下次
            }
            int64_t next = A + k * N;
            if (next <= afterUnix) {
                if (k == std::numeric_limits<int64_t>::max()) {
                    return Result<std::optional<int64_t>>::ok(std::optional<int64_t>());
                }
                ++k;
                if (k > (std::numeric_limits<int64_t>::max() - A) / N) {
                    return Result<std::optional<int64_t>>::ok(std::optional<int64_t>());
                }
                next = A + k * N;
            }
            return Result<std::optional<int64_t>>::ok(std::optional<int64_t>(next));
        }
        case RuleKind::Calendar: return calendarNextFire(spec, afterUnix, ctx);
        case RuleKind::Cron: return cronNextFireInternal(spec, afterUnix);
        case RuleKind::Event:
        case RuleKind::Resident:
            return Result<std::optional<int64_t>>::ok(std::optional<int64_t>());
    }
    return Result<std::optional<int64_t>>::fail(ErrorCode::Internal, "未处理 kind");
}

Result<std::vector<int64_t>> missedFires(const RuleSpec& spec, int64_t fromUnix,
                                         int64_t toUnix, int cap, const FireContext& ctx) {
    auto zone = checkZone(ctx);
    if (zone.isFail()) {
        return Result<std::vector<int64_t>>::fail(zone.errorCode(), zone.errorMessage());
    }
    if (cap < 1) {
        return Result<std::vector<int64_t>>::fail(ErrorCode::ValidationError, "cap 必须 ≥1");
    }
    if (fromUnix > toUnix) {
        return Result<std::vector<int64_t>>::fail(ErrorCode::ValidationError,
                                                  "fromUnix 必须 ≤ toUnix");
    }
    std::vector<int64_t> out;
    switch (spec.kind) {
        case RuleKind::Once: {
            int64_t u = 0;
            auto r = onceFireUnix(spec, u);
            if (r.isFail()) return Result<std::vector<int64_t>>::fail(r.errorCode(), r.errorMessage());
            if (u >= fromUnix && u <= toUnix) out.push_back(u);
            break;
        }
        case RuleKind::Interval: {
            const int64_t N = spec.everySec;
            const int64_t A = spec.anchor;
            if (toUnix < A) break;
            int64_t kStart = 0;
            if (fromUnix > A) {
                kStart = (fromUnix - A) / N;
                if ((fromUnix - A) % N != 0) ++kStart;
            }
            const int64_t span = toUnix - A;
            int64_t kEnd = span / N;
            const int64_t kMax = (std::numeric_limits<int64_t>::max() - A) / N;
            if (kEnd > kMax) kEnd = kMax;
            int64_t k = kStart;
            while (k <= kEnd && static_cast<int>(out.size()) < cap) {
                out.push_back(A + k * N);
                ++k;
            }
            break;
        }
        case RuleKind::Calendar: {
            const CalendarSpec& c = spec.calendar;
            int64_t fromDay = 0;
            {
                std::tm wall{};
                core::time::localTime(static_cast<std::time_t>(fromUnix), &wall);
                fromDay = core::time::daysFromCivil(wall.tm_year + 1900,
                                                    static_cast<unsigned>(wall.tm_mon) + 1,
                                                    static_cast<unsigned>(wall.tm_mday));
            }
            std::tm toWall{};
            core::time::localTime(static_cast<std::time_t>(toUnix), &toWall);
            const int64_t toDay = core::time::daysFromCivil(toWall.tm_year + 1900,
                                                            static_cast<unsigned>(toWall.tm_mon) + 1,
                                                            static_cast<unsigned>(toWall.tm_mday));
            constexpr int64_t kMaxDays = 146097;  // 400 年
            if (toDay - fromDay > kMaxDays) {
                return Result<std::vector<int64_t>>::fail(ErrorCode::ValidationError,
                                                          "missedFires 日历区间超过 400 年不支持");
            }
            for (int64_t dayNum = fromDay; dayNum <= toDay && static_cast<int>(out.size()) < cap; ++dayNum) {
                int y = 0;
                unsigned m = 0, d = 0;
                civilFromDays(dayNum, y, m, d);
                const int wday = wdayOf(dayNum);
                if (!calendarDayMatches(c, y, m, d, wday)) continue;
                std::tm want{};
                want.tm_year = y - 1900;
                want.tm_mon = static_cast<int>(m) - 1;
                want.tm_mday = static_cast<int>(d);
                want.tm_hour = c.hour;
                want.tm_min = c.minute;
                want.tm_sec = 0;
                int64_t u = 0;
                if (!resolveWallClock(want, u)) continue;
                if (u >= fromUnix && u <= toUnix) out.push_back(u);
            }
            break;
        }
        case RuleKind::Cron: {
            int64_t t = fromUnix - 1;
            while (static_cast<int>(out.size()) < cap) {
                auto r = cronNextFireInternal(spec, t);
                if (r.isFail()) return Result<std::vector<int64_t>>::fail(r.errorCode(), r.errorMessage());
                if (!r.value().has_value()) break;
                const int64_t f = *r.value();
                if (f > toUnix) break;
                out.push_back(f);
                t = f;
            }
            break;
        }
        case RuleKind::Event:
        case RuleKind::Resident:
            break;
    }
    return Result<std::vector<int64_t>>::ok(std::move(out));
}

} // namespace core::schedule
