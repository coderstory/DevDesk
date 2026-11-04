#include "module_schedule/schedule_ui_list.h"

#include "module_schedule/schedule_ui_history.h"

#include "core/component_theme.h"
#include "core/logging.h"
#include "core/theme.h"
#include "core/time.h"
#include "module_schedule/schedule_engine.h"
#include "platform/json.h"
#include "ui_kit/button.h"
#include "ui_kit/dialog_form.h"
#include "ui_kit/glass.h"
#include "ui_kit/jelly.h"
#include "ui_kit/metrics.h"
#include "ui_kit/wtab.h"

#include <components/button.h>
#include <components/dialog.h>
#include <components/scrollview.h>
#include <components/switch.h>
#include <components/theme.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <utility>

namespace module::schedule {

namespace {

// 页面几何（池140 §1/§1.3：页 pad 20 / 页头 40 / TAB 36，固定区不滚）
constexpr float kPagePad = 20.0f;      // 页边（§3.1：内容列宽 W = 窗宽 − 页边 40）
constexpr int kHistoryLimit = 50;      // 抽屉装载与展示的 runs 上限（assembleHistory 截断）
constexpr unsigned int kLockGlyph = 0xF023;      // fa-lock（系统卡锁图标）
constexpr unsigned int kChevronDown = 0xF107;    // fa-angle-down
constexpr unsigned int kChevronRight = 0xF105;   // fa-angle-right

// 瀑布卡面几何（池140 §4，100% 基线；绘制时全部经 scaled() 派生）
constexpr float kCardPad = 16.0f;        // 卡内边距（全站基线 16）
constexpr float kNameRowH = 20.0f;       // 头区名称行
constexpr float kSummaryTopGap = 6.0f;   // 摘要上距
constexpr float kSummaryLineH = 18.0f;   // 摘要行高（12px 字）
constexpr float kDataTopGap = 12.0f;     // 头区→分割线
constexpr float kDataBottomGap = 10.0f;  // 分割线→引导字行
constexpr float kGuideRowH = 16.0f;      // 引导字行（11px）
constexpr float kGuideValueGap = 4.0f;   // 引导字→值行
constexpr float kValueRowH = 22.0f;      // 值行（15px 下次 / 22 高 pill）
constexpr int kSummaryMaxLines = 2;      // 摘要行数上限
constexpr float kCardGap = 12.0f;        // 列间距 = 卡纵距（8px 网格半档）

// 上次结果状态 → 中文（与 schedule_run.status 值域对齐，§4.3 pill 值域）
std::string runStatusZh(const std::string& status) {
    if (status == "success") return "成功";
    if (status == "failed") return "失败";
    if (status == "pending") return "待执行";
    if (status == "running") return "运行中";
    if (status == "skipped") return "已跳过";
    if (status == "missed") return "已错过";
    if (status == "suppressed") return "已抑制";
    return status;
}

// 渠道 kind → 中文展示名（schedule_channel.kind → 运行历史里的 chip 文案）
std::string channelKindZh(const std::string& kind) {
    if (kind == "fullscreen") return "全屏提醒";
    if (kind == "toast") return "应用内提示";
    if (kind == "os") return "系统通知";
    if (kind == "silent") return "静默";
    if (kind == "wecom") return "企业微信";
    return kind;
}

// 规则 kind → 卡面类型 chip 文案（池140 §4.1：单次/间隔/周期/cron/事件/常驻）
std::string kindChipText(const std::string& kind) {
    if (kind == "once") return "单次";
    if (kind == "interval") return "间隔";
    if (kind == "calendar") return "周期";
    if (kind == "cron") return "cron";
    if (kind == "event") return "事件";
    if (kind == "resident") return "常驻";
    return kind;
}

std::string twoDigits(int v) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02d", v);
    return buf;
}

// 本地墙钟 HH:MM（nextFireText/lastRunText 共用）
std::string clockText(std::time_t sec) {
    std::tm local{};
    core::time::localTime(sec, &local);
    return twoDigits(local.tm_hour) + ":" + twoDigits(local.tm_min);
}

// 本地墙钟 MM-DD
std::string monthDayText(std::time_t sec) {
    std::tm local{};
    core::time::localTime(sec, &local);
    return twoDigits(local.tm_mon + 1) + "-" + twoDigits(local.tm_mday);
}

std::string weekdayName(int wday) {
    static const char* kNames[] = {"周日", "周一", "周二", "周三",
                                   "周四", "周五", "周六"};
    const int i = (wday < 0 || wday > 6) ? 0 : wday;
    return kNames[i];
}

// spec_json 安全取字符串字段；缺失/非串 → 空串
std::string specString(const platform::json::JsonObject& obj, const char* key) {
    if (!obj.has(key)) return "";
    const auto v = obj.get(key);
    if (!v.isString()) return "";
    return v.asString();
}

// spec_json 安全取整数字段；缺失/非数 → fallback
int64_t specInt(const platform::json::JsonObject& obj, const char* key,
                int64_t fallback = 0) {
    if (!obj.has(key)) return fallback;
    const auto v = obj.get(key);
    if (!v.isNumber()) return fallback;
    return v.asInt();
}

// spec_json 安全取布尔字段；缺失/非布尔 → false
bool specBool(const platform::json::JsonObject& obj, const char* key) {
    if (!obj.has(key)) return false;
    const auto v = obj.get(key);
    return v.isBool() && v.asBool();
}

// month/year 形态的「第 n 个周 x」：spec 键是对象 nth_weekday{n,weekday}
// （weekday 数字 0=周日，经 weekdayName 换算）；缺字段 → 空串
std::string nthWeekdaySummary(const platform::json::JsonObject& obj) {
    if (!obj.has("nth_weekday")) return "";
    const auto v = obj.get("nth_weekday");
    if (!v.isObject()) return "";
    const auto o = v.asObject();
    const int64_t n = specInt(o, "n", 0);
    const int64_t wd = specInt(o, "weekday", -1);
    if (n <= 0 || wd < 0 || wd > 6) return "";
    return "第 " + std::to_string(n) + " 个" + weekdayName(static_cast<int>(wd));
}

// spec_json 安全取字符串数组字段；缺失/非数组 → 空
std::vector<std::string> specStringArray(const platform::json::JsonObject& obj,
                                         const char* key) {
    std::vector<std::string> out;
    if (!obj.has(key)) return out;
    const auto v = obj.get(key);
    if (!v.isArray()) return out;
    for (int i = 0; i < v.size(); ++i) {
        const auto elem = v[i];
        if (elem.isString()) {
            out.push_back(elem.asString());
        } else if (elem.isNumber()) {
            // weekdays 等数组存的是数字（[2] = 周二），统一转字符串
            out.push_back(std::to_string(elem.asInt()));
        }
    }
    return out;
}

// "2" → "周二"；非数字原样（防御未来扩展）
std::string weekdaysSummary(const std::vector<std::string>& days) {
    std::string out;
    for (size_t i = 0; i < days.size(); ++i) {
        const bool isDigit =
            !days[i].empty() &&
            std::isdigit(static_cast<unsigned char>(days[i][0])) != 0;
        out += isDigit ? weekdayName(std::atoi(days[i].c_str())) : days[i];
        if (i + 1 < days.size()) out += "、";
    }
    return out;
}

// 时间串「HH:MM」；不满足 → 空串（不渲染时间段）
std::string clockFromTimeField(const std::string& time) {
    return time.size() >= 5 && time[2] == ':' ? time.substr(0, 5) : std::string();
}

// ---- 瀑布流布局辅助 ----

// 文本宽度估算（release chipWidth 同式）：瀑布卡高须在绘制前确定，按内容
// 宽掐算摘要行数用。CJK 字节 0.43 / ASCII 0.56 × 字号。
float textWidth(const std::string& text, float fontSize) {
    float w = fontSize * 1.6f;
    for (unsigned char c : text) {
        w += (c >= 0x80) ? fontSize * 0.43f : fontSize * 0.56f;
    }
    return w;
}

// 摘要行数估算（§4.1：最多 2 行省略）
int estimateLines(const std::string& text, float availW, float fontSize) {
    if (availW <= 0.0f) return kSummaryMaxLines;
    const float w = textWidth(text, fontSize);
    const int lines = static_cast<int>(std::ceil(w / availW));
    return std::clamp(lines, 1, kSummaryMaxLines);
}

// 卡高（§4：头区 + 数据区，随摘要行数 140~176 档；全经 scaled 派生）
float cardHeightForLines(int lines) {
    const int l = std::clamp(lines, 1, kSummaryMaxLines);
    return core::theme::scaled(
        kCardPad + kNameRowH + kSummaryTopGap + kSummaryLineH * l +
        kDataTopGap + 1.0f + kDataBottomGap + kGuideRowH + kGuideValueGap +
        kValueRowH + kCardPad);
}

// 颜色混合（fg 按 α 叠在 base 上）
eui::Color blendColor(const eui::Color& base, const eui::Color& fg, float a) {
    return eui::Color{base.r + (fg.r - base.r) * a,
                      base.g + (fg.g - base.g) * a,
                      base.b + (fg.b - base.b) * a, base.a};
}

// 七态 pill 配色（池140 §4.3；中性底走 hover token，失败底 = errorText
// 按 0.10 叠卡面）
void runStatusPillColors(const std::string& status,
                         const core::theme::Tokens& t, eui::Color& bg,
                         eui::Color& fg) {
    if (status == "success" || status == "running") {
        bg = t.colors.primarySoftBg;
        fg = t.colors.primaryText;
    } else if (status == "failed") {
        bg = blendColor(ui_kit::glassFill(t), t.colors.errorText, 0.10f);
        fg = t.colors.errorText;
    } else if (status == "pending") {
        bg = t.colors.hover;
        fg = t.colors.textSecondary;
    } else {  // skipped / missed / suppressed
        bg = t.colors.hover;
        fg = t.colors.textTertiary;
    }
}

// 瀑布卡槽位
struct CardSlot {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

// 瀑布排布（§3.1 实现落点）：按列数断点分列，新卡入最短列（高度均衡）。
// 列间距 = 卡纵距 = kCardGap；返回逐卡槽位（顺序与入参一致）。
std::vector<CardSlot> masonryLayout(const std::vector<float>& heights,
                                    float contentW, float gap) {
    const int cols = columnCountForWidth(contentW);
    const float usable =
        std::max(0.0f, contentW - gap * static_cast<float>(cols - 1));
    const float colW = usable / static_cast<float>(cols);
    std::vector<float> colY(static_cast<std::size_t>(cols), 0.0f);
    std::vector<CardSlot> slots(heights.size());
    for (std::size_t i = 0; i < heights.size(); ++i) {
        std::size_t best = 0;
        for (std::size_t c = 1; c < colY.size(); ++c) {
            if (colY[c] < colY[best]) best = c;
        }
        slots[i] = {static_cast<float>(best) * (colW + gap), colY[best], colW,
                    heights[i]};
        colY[best] += heights[i] + gap;
    }
    return slots;
}

const eui::Color kTransparent{0.0f, 0.0f, 0.0f, 0.0f};

} // namespace

std::string nextFireText(const std::optional<int64_t>& nextFireAt,
                         int64_t nowSec) {
    if (!nextFireAt.has_value() || nextFireAt.value() <= 0) return "—";
    const std::time_t at = static_cast<std::time_t>(nextFireAt.value());
    const int64_t nowDay =
        core::time::localDayNumber(static_cast<std::time_t>(nowSec));
    const int64_t atDay = core::time::localDayNumber(at);
    const std::string hm = clockText(at);
    if (atDay == nowDay) return "今天 " + hm;
    if (atDay == nowDay + 1) return "明天 " + hm;
    return monthDayText(at) + " " + hm;
}

std::string lastRunText(const ListRow& row, int64_t nowSec) {
    (void)nowSec;
    if (!row.lastRunAt.has_value() || row.lastRunStatus.empty()) return "无记录";
    const std::time_t at = static_cast<std::time_t>(row.lastRunAt.value());
    return runStatusZh(row.lastRunStatus) + " · " + monthDayText(at) + " " +
           clockText(at);
}

std::string ruleSummary(const RuleRecord& rule) {
    // 摘要精确性不凌驾列表页可用性：解析失败一律回退 kind 名，不抛。
    const auto parsed = platform::json::parse(rule.specJson);
    if (parsed.isFail() || !parsed.value().isObject()) {
        return rule.kind.empty() ? "未知规则" : rule.kind;
    }
    const platform::json::JsonObject spec = parsed.value().asObject();

    if (rule.kind == "once") {
        const std::string atLocal = specString(spec, "at_local");
        if (!atLocal.empty()) return "单次 " + atLocal;
        const int64_t at = specInt(spec, "at", 0);
        if (at > 0) {
            const std::time_t t = static_cast<std::time_t>(at);
            return "单次 " + monthDayText(t) + " " + clockText(t);
        }
        return "单次";
    }
    if (rule.kind == "interval") {
        const int64_t everySec = specInt(spec, "every_sec", 0);
        if (everySec <= 0) return "间隔";
        if (everySec % 3600 == 0) {
            return "每 " + std::to_string(everySec / 3600) + " 小时";
        }
        if (everySec % 60 == 0) {
            return "每 " + std::to_string(everySec / 60) + " 分钟";
        }
        return "每 " + std::to_string(everySec) + " 秒";
    }
    if (rule.kind == "calendar") {
        const std::string unit = specString(spec, "unit");
        const std::string time = clockFromTimeField(specString(spec, "time"));
        const std::string at = time.empty() ? std::string() : " " + time;
        if (unit == "day") {
            const auto wo = spec.get("workdays_only");
            const bool workdays = wo.isBool() && wo.asBool();
            return std::string(workdays ? "工作日" : "每天") + at;
        }
        if (unit == "week") {
            const int64_t interval = specInt(spec, "interval", 1);
            const std::string days =
                weekdaysSummary(specStringArray(spec, "weekdays"));
            const std::string prefix =
                interval <= 1
                    ? "每周"
                    : "每 " + std::to_string(interval) + " 周";
            return prefix + (days.empty() ? std::string() : " " + days) + at;
        }
        if (unit == "month") {
            const int64_t monthDay = specInt(spec, "month_day", 0);
            if (monthDay > 0) {
                return "每月 " + std::to_string(monthDay) + " 日" + at;
            }
            if (specBool(spec, "last_workday")) {
                return "每月最后一个工作日" + at;
            }
            const std::string nth = nthWeekdaySummary(spec);
            if (!nth.empty()) {
                return "每月" + nth + at;
            }
            return "每月" + at;
        }
        if (unit == "year") {
            // year spec 键与 month 同一套（month_day/nth_weekday/last_workday，
            // buildCalendarJson 不落 "month" 数字），别按「每年 M 月 D 日」读
            const int64_t monthDay = specInt(spec, "month_day", 0);
            if (monthDay > 0) {
                return "每年 " + std::to_string(monthDay) + " 日" + at;
            }
            if (specBool(spec, "last_workday")) {
                return "每年最后一个工作日" + at;
            }
            const std::string nth = nthWeekdaySummary(spec);
            if (!nth.empty()) {
                return "每年" + nth + at;
            }
            return "每年" + at;
        }
        return "周期" + at;
    }
    if (rule.kind == "cron") {
        const std::string expr = specString(spec, "expr");
        return "cron " + expr;
    }
    if (rule.kind == "event") return "事件触发";
    if (rule.kind == "resident") return "常驻";
    return rule.kind;
}

int columnCountForWidth(float contentW) {
    if (contentW < 520.0f) return 1;
    if (contentW < 1080.0f) return 2;
    if (contentW < 1660.0f) return 3;
    return 4;
}

namespace {

// 各段行内部排序：next_fire_at 升序；无值排后。stable_sort 保证无值行
// 保持输入序（用户项 = store.listRules 的 id 升序，系统项 = 注册序）。
void sortRows(std::vector<ListRow>& rows) {
    std::stable_sort(rows.begin(), rows.end(),
                     [](const ListRow& a, const ListRow& b) {
        if (a.nextFireAt.has_value() != b.nextFireAt.has_value()) {
            return a.nextFireAt.has_value();  // 有值的在前
        }
        if (a.nextFireAt.has_value()) {
            return a.nextFireAt.value() < b.nextFireAt.value();
        }
        return a.ruleId < b.ruleId;  // 均无值：用户项按 id；系统项 id 恒 0 → 保输入序
    });
}

ListRow fromRule(const RuleRecord& r) {
    ListRow row;
    row.isSystem = false;
    row.ruleId = r.id;
    row.name = r.name;
    row.kind = r.kind;
    row.summary = ruleSummary(r);
    row.nextFireAt = r.nextFireAt;
    row.enabled = r.enabled;
    row.archived = r.archivedAt.has_value();
    return row;
}

ListRow fromSystemItem(const ScheduleItemView& item) {
    ListRow row;
    row.isSystem = true;
    row.source = item.source;
    row.key = item.key;
    row.name = item.title;
    row.summary = item.summary;
    row.nextFireAt = item.nextFireAt;
    row.enabled = item.enabled;
    row.sourceRoute = item.sourceRoute;
    row.executable = item.executable;
    return row;
}

} // namespace

ListAssembly assembleList(const std::vector<RuleRecord>& rules,
                          const std::vector<ScheduleItemView>& systemItems) {
    ListAssembly out;
    for (const auto& item : systemItems) {
        out.systemRows.push_back(fromSystemItem(item));
    }
    for (const auto& rule : rules) {
        ListRow row = fromRule(rule);
        if (row.archived) {
            out.archivedRows.push_back(std::move(row));
        } else {
            out.userRows.push_back(std::move(row));
        }
    }
    sortRows(out.systemRows);
    sortRows(out.userRows);
    sortRows(out.archivedRows);
    return out;
}

// ---- ScheduleListUi ----

ScheduleListUi::ScheduleListUi(ScheduleStore& store,
                               ScheduleProviderRegistry& providers,
                               InternalTaskRegistry* tasks,
                               ScheduleEngine* engine)
    : store_(store), providers_(providers), tasks_(tasks), engine_(engine) {
    historyHost_.close = [this] { historyState_.open = false; };
    // 重试按钮：引擎在位才接（无引擎时抽屉隐藏重试按钮）。
    if (engine_ != nullptr) {
        historyHost_.retryRun = [this](int64_t runId) { retryHistoryRun(runId); };
    }
    // 保存落库点（drawEditDialog 保存按钮 → host.saveRule）：编辑 = 业务字段
    // 覆盖（getRule 取现状保 created/archived 等不可写字段），新建 = createRule；
    // 成功 reload 后回空串（弹窗关闭），失败回红字（弹窗保留不丢表单）。
    editHost_.saveRule = [this](const RuleRecord& rec) -> std::string {
        if (rec.id > 0) {
            const auto cur = store_.getRule(rec.id);
            if (cur.isFail()) {
                return "规则不存在或已删除：" + cur.errorMessage();
            }
            RuleRecord updated = cur.value();
            updated.name = rec.name;
            updated.enabled = rec.enabled;
            updated.kind = rec.kind;
            updated.specJson = rec.specJson;
            updated.actionKind = rec.actionKind;
            updated.actionJson = rec.actionJson;
            updated.channelIds = rec.channelIds;
            updated.missedPolicy = rec.missedPolicy;
            updated.nextFireAt = rec.nextFireAt;
            const auto r = store_.updateRule(updated);
            if (r.isFail()) return r.errorMessage();
        } else {
            const auto r = store_.createRule(rec);
            if (r.isFail()) return r.errorMessage();
        }
        reload(static_cast<int64_t>(core::time::nowUnix()));
        // 收弹窗放宿主侧（draw 层保存成功也调 host.close，幂等）
        if (editHost_.close) editHost_.close();
        return {};
    };
    editHost_.close = [this] {
        editState_.open = false;
        editState_.saveError.clear();
    };
    // 弹窗左下「删除」（池140 §5）：收编辑弹窗 → 走既有删除确认链路不变。
    editHost_.requestDelete = [this](int64_t ruleId) {
        editState_.open = false;
        editState_.saveError.clear();
        requestDeleteRule(ruleId);
    };
}

void ScheduleListUi::reload(int64_t nowSec) {
    (void)nowSec;
    error_.clear();
    const auto systemItems = providers_.allItems();
    const auto rules = store_.listRules();
    if (rules.isFail()) {
        error_ = "定时列表加载失败：" + rules.errorMessage();
        LOG_WARN("schedule 列表：规则加载失败 — " + rules.errorMessage());
        assembly_ = assembleList({}, systemItems);
        return;
    }
    ListAssembly next = assembleList(rules.value(), systemItems);
    // 每条用户规则补「上次结果」（最近一次 run；planned_at 倒序取第一条）。
    // 读失败按「无记录」降级——列表可用性优先于历史完整性。
    for (auto& row : next.userRows) {
        const auto runs = store_.listRunsByRule(row.ruleId, 1);
        if (runs.isOk() && !runs.value().empty()) {
            row.lastRunAt = runs.value().front().plannedAt;
            row.lastRunStatus = runs.value().front().status;
        }
    }
    for (auto& row : next.archivedRows) {
        const auto runs = store_.listRunsByRule(row.ruleId, 1);
        if (runs.isOk() && !runs.value().empty()) {
            row.lastRunAt = runs.value().front().plannedAt;
            row.lastRunStatus = runs.value().front().status;
        }
    }
    assembly_ = std::move(next);
}

core::Result<void> ScheduleListUi::setRuleEnabled(int64_t ruleId, bool enabled) {
    const auto rule = store_.getRule(ruleId);
    if (rule.isFail()) {
        return core::Result<void>::fail(rule.errorCode(),
                                        "规则不存在或已删除：" + rule.errorMessage());
    }
    RuleRecord updated = rule.value();
    updated.enabled = enabled;
    const auto saved = store_.updateRule(updated);
    if (saved.isFail()) {
        return core::Result<void>::fail(saved.errorCode(), saved.errorMessage());
    }
    return core::Result<void>::ok();
}

void ScheduleListUi::setTab(int tab) {
    const int clamped = std::clamp(tab, 0, 1);
    if (clamped == activeTab_) {
        return;
    }
    activeTab_ = clamped;
    // §1.2：TAB 切换 = 滚动区回顶 + 120ms 淡入；两标记由下一帧 compose 消费
    //（setScrollOffset 帧末取件；opacity 目标值翻转走 120ms 过渡）。
    pendingScrollReset_ = true;
    tabFadePending_ = true;
}

void ScheduleListUi::refreshEditHost() {
    editHost_.channels.clear();
    const auto chans = store_.listChannels();
    if (chans.isOk()) {
        for (const auto& c : chans.value()) {
            ui::ChannelOption opt;
            opt.id = c.id;
            opt.key = c.kind;
            opt.label = c.name;
            editHost_.channels.push_back(std::move(opt));
        }
    } else {
        error_ = "渠道选项加载失败：" + chans.errorMessage();
    }
    editHost_.taskKeys.clear();
    if (tasks_) editHost_.taskKeys = tasks_->taskKeys();
}

void ScheduleListUi::openNewRule() {
    error_.clear();
    refreshEditHost();
    editState_ = ui::EditDialogState{};  // 默认表单：Once / remind / 策略 once
    editState_.ruleId = 0;
    {  // 默认触发时刻 = 明天 09:00（空串会卡校验，过去时刻会让预览恒空）
        std::time_t tomorrow = static_cast<std::time_t>(core::time::nowUnix()) + 86400;
        std::tm lt{};
        core::time::localTime(tomorrow, &lt);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d 09:00",
                      lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
        editState_.form.onceAtLocal = buf;
    }
    editState_.open = true;
}

void ScheduleListUi::openEditRule(int64_t ruleId) {
    error_.clear();
    const auto rule = store_.getRule(ruleId);
    if (rule.isFail()) {
        error_ = "规则读取失败：" + rule.errorMessage();
        return;
    }
    const auto form = ui::formFromRecord(rule.value());
    if (form.isFail()) {
        error_ = "规则回填失败：" + form.errorMessage();
        return;
    }
    refreshEditHost();
    editState_ = ui::EditDialogState{};
    editState_.ruleId = ruleId;
    editState_.form = form.value();
    editState_.open = true;
}

void ScheduleListUi::requestDeleteRule(int64_t ruleId) {
    error_.clear();
    const auto rule = store_.getRule(ruleId);
    if (rule.isFail()) {
        error_ = "规则读取失败：" + rule.errorMessage();
        return;
    }
    pendingDeleteId_ = ruleId;
    pendingDeleteName_ = rule.value().name;
    pendingDeletePurge_ = false;  // 默认软删（§4.4：配置与历史保留）
}

void ScheduleListUi::confirmPendingDelete() {
    const auto r = pendingDeletePurge_ ? store_.purgeRule(pendingDeleteId_)
                                       : store_.deleteRule(pendingDeleteId_);
    if (r.isFail()) {
        error_ = "删除失败：" + r.errorMessage();
        return;  // 红字出列表页 toast，确认弹窗保留
    }
    cancelPendingDelete();
    reload(static_cast<int64_t>(core::time::nowUnix()));
}

void ScheduleListUi::cancelPendingDelete() {
    pendingDeleteId_ = 0;
    pendingDeleteName_.clear();
    pendingDeletePurge_ = false;
}

void ScheduleListUi::openHistory() {    error_.clear();
    // 系统项 run 的 rule_id 为空且 store 无 origin_key 维度查询——抽屉 v1 只
    // 聚合用户/归档规则的历史（listRules 含归档行），系统项历史随来源注册波接。
    const auto rules = store_.listRules();
    if (rules.isFail()) {
        error_ = "运行历史加载失败：" + rules.errorMessage();
        LOG_WARN("schedule 历史抽屉：规则清单读取失败 — " + rules.errorMessage());
        return;
    }
    std::vector<RunRecord> runs;
    std::vector<DeliveryRecord> deliveries;
    for (const auto& rule : rules.value()) {
        const auto rs = store_.listRunsByRule(rule.id, kHistoryLimit);
        if (rs.isFail()) {
            error_ = "运行历史加载失败：" + rs.errorMessage();
            LOG_WARN("schedule 历史抽屉：run 读取失败 — " + rs.errorMessage());
            continue;  // 单规则失败降级已载入部分，整页可用性优先
        }
        for (auto& run : rs.value()) {
            const auto ds = store_.listDeliveriesByRun(run.id);
            if (ds.isFail()) {
                error_ = "运行历史加载失败：" + ds.errorMessage();
                LOG_WARN("schedule 历史抽屉：投递读取失败 — " + ds.errorMessage());
                continue;
            }
            runs.push_back(std::move(run));
            deliveries.insert(deliveries.end(),
                              std::make_move_iterator(ds.value().begin()),
                              std::make_move_iterator(ds.value().end()));
        }
    }
    historyHost_.items = ui::assembleHistory(runs, deliveries, kHistoryLimit);
    // 渠道展示名：schedule_delivery.channel_id 存的是 schedule_channel.id（数字），
    // 直接给用户看「#3」无意义——查渠道表换成可读名（自定义名优先，回落 kind 中文）。
    std::map<int64_t, std::string> channelLabels;
    const auto channels = store_.listChannels();
    if (channels.isOk()) {
        for (const auto& ch : channels.value()) {
            channelLabels[ch.id] =
                !ch.name.empty() ? ch.name : channelKindZh(ch.kind);
        }
    }
    historyHost_.channelLabel = [channelLabels](int64_t id) -> std::string {
        const auto it = channelLabels.find(id);
        return it == channelLabels.end() ? std::string() : it->second;
    };
    historyState_.open = true;
}

void ScheduleListUi::retryHistoryRun(int64_t runId) {
    error_.clear();
    if (engine_ == nullptr) {
        error_ = "重跑入口不可用";
        return;
    }
    const auto got = store_.getRun(runId);
    if (got.isFail()) {
        error_ = "运行记录读取失败：" + got.errorMessage();
        return;
    }
    if (!got.value().has_value() || !got.value()->ruleId.has_value()) {
        error_ = "该运行记录无关联规则，无法重跑";
        return;
    }
    const auto res =
        engine_->rerunNow(*got.value()->ruleId, static_cast<int64_t>(core::time::nowUnix()));
    if (res.isFail()) {
        error_ = "重跑失败：" + res.errorMessage();
        return;
    }
    // 重跑会新增一条 run 行与投递记录：重装历史让用户立刻看到结果。
    openHistory();
}

void ScheduleListUi::resetForTest() {
    archiveExpanded_ = false;
    error_.clear();
    assembly_ = ListAssembly{};
    historyState_ = ui::HistoryDrawerState{};
    editState_ = ui::EditDialogState{};
    activeTab_ = 0;
    pendingScrollReset_ = false;
    tabFadePending_ = false;
    hoveredCardIds_.clear();
    cancelPendingDelete();
}

void ScheduleListUi::compose(eui::Ui& ui, const eui::Screen& screen) {
    const auto& t = core::theme::current();
    const auto comp = core::theme::componentTokensWeChat(t);
    const int64_t nowSec = static_cast<int64_t>(core::time::nowUnix());

    // ---- 固定区几何（池140 §1.1/§1.3）：页 pad 20 / 页头 40 / gap 8 / TAB 36 ----
    const float pad = kPagePad;
    const float pageW = screen.width;
    const float contentW = std::max(0.0f, pageW - pad * 2.0f);
    const float headerH = core::theme::scaled(ui_kit::kHeaderH);  // 40 全站口径（池139）
    const float headerY = pad;                                    // 标题顶格，无额外顶距
    const float newBtnW = core::theme::scaled(88.0f);
    const float historyW = core::theme::scaled(96.0f);
    const float tabRowH = core::theme::scaled(ui_kit::kWtabHeight);
    const float tabY = headerY + headerH + core::theme::scaled(8.0f);
    const float toastH = error_.empty() ? 0.0f : core::theme::scaled(20.0f);
    const float toastGap = error_.empty() ? 0.0f : core::theme::scaled(8.0f);
    const float scrollY = tabY + tabRowH + core::theme::scaled(8.0f) + toastH + toastGap;
    const float scrollH = std::max(0.0f, screen.height - scrollY - pad);

    // 瀑布卡绘制（系统/用户/归档卡同构；isSystem 决定锁图标、来源块与只读态）。
    // ⚠ ui 必须作参数逐层透传：scrollView.build() 会把 content lambda 跑到临时
    // measureUi 上测内容高（components/scrollview.h measureContentHeight），lambda
    // 形参 ui 遮蔽本函数同名参数——若绘制 lambda 直捕外层 ui，测量路径会把卡片
    // 画进主树 root 直下（幽灵副本盖住页首交互面，TAB 点击被吞，bug 150）。
    auto drawCard = [&](eui::Ui& ui, const ListRow& row, const std::string& id,
                        const CardSlot& slot) {
        const float cardW = slot.w;
        const float cardH = slot.h;
        const float padX = core::theme::scaled(kCardPad);
        const float padY = core::theme::scaled(kCardPad);
        const bool hovered = hoveredCardIds_.count(id) > 0;
        const bool interactive = !row.isSystem;
        // 停用/归档灰化只罩数据区（§4.4：头区不灰，名称可读）
        const float dataOpacity =
            interactive ? (row.archived ? 0.70f : (row.enabled ? 1.0f : 0.55f))
                        : 1.0f;
        const float innerW = std::max(0.0f, cardW - padX * 2.0f);
        const float rightX = padX + innerW * 0.55f;  // 左块 55% / 右块 45%
        const float rightW = innerW * 0.45f;

        ui.stack(id)
            .position(slot.x, slot.y)
            .size(cardW, cardH)
            // 绝对定位脱离流式（池133 同款：column 静默忽略未 ignoreLayout 的 position）
            .ignoreLayout()
            .content([&] {
                // 卡面（L2 glassFill）。§6/§7：hover 只混黑不放大不变形（专业感档），
                // 按下果冻经命中面 visualStateFrom 驱动（缩至 0.965 回弹）。
                ui_kit::JellyOptions opt;
                opt.radius = t.sizes.radiusLarge;
                opt.hoverRadius = t.sizes.radiusLarge;
                opt.hoverScale = 1.0f;
                opt.pressedScale = 0.965f;  // 设计 §7 按下缩放 0.965（默认 0.97）
                if (interactive) opt.pressedFrom = id + ".hit";
                ui_kit::jellyCard(ui, id + ".bg", 0.0f, 0.0f, cardW, cardH, t,
                                  opt, hovered);

                // ---- 头区：（锁）名称 + 特殊态 tag + 类型 chip 右顶 ----
                const float chipFont = t.sizes.fontSizeSmall - 1.0f;  // 11
                const float chipH = core::theme::scaled(20.0f);
                const std::string chipText =
                    interactive ? kindChipText(row.kind) : std::string();
                const float chipW =
                    chipText.empty()
                        ? 0.0f
                        : core::theme::scaled(20.0f) +
                              textWidth(chipText, chipFont);
                const char* tagText =
                    row.archived ? "已归档" : (!row.enabled ? "已停用" : nullptr);
                const float tagW =
                    tagText != nullptr
                        ? core::theme::scaled(16.0f) +
                              textWidth(tagText, chipFont)
                        : 0.0f;
                float nameX = padX;
                if (row.isSystem) {
                    ui.text(id + ".lock")
                        .position(padX, padY)
                        .size(core::theme::scaled(16.0f),
                              core::theme::scaled(kNameRowH))
                        .icon(eui::utf8(kLockGlyph))
                        .color(t.colors.textTertiary)
                        .build();
                    nameX += core::theme::scaled(24.0f);
                }
                const float nameW = std::max(
                    core::theme::scaled(60.0f),
                    innerW - (nameX - padX) - chipW - tagW -
                        core::theme::scaled(tagText != nullptr ? 16.0f : 8.0f));
                ui.text(id + ".name")
                    .position(nameX, padY)
                    .size(nameW, core::theme::scaled(kNameRowH))
                    .text(row.name)
                    .fontSize(t.sizes.fontSizeBody)
                    .fontWeight(500)
                    .color(t.colors.textPrimary)
                    .build();
                if (tagText != nullptr) {
                    const float tagX = nameX + nameW + core::theme::scaled(8.0f);
                    const float tagH = core::theme::scaled(18.0f);
                    ui.rect(id + ".tag")
                        .position(tagX, padY + (core::theme::scaled(kNameRowH) - tagH) * 0.5f)
                        .size(tagW, tagH)
                        .color(t.colors.hover)
                        .radius(tagH * 0.5f)
                        .build();
                    ui.text(id + ".tag.label")
                        .position(tagX, padY + (core::theme::scaled(kNameRowH) - tagH) * 0.5f)
                        .size(tagW, tagH)
                        .text(tagText)
                        .fontSize(chipFont)
                        .horizontalAlign(eui::HorizontalAlign::Center)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(t.colors.textTertiary)
                        .build();
                }
                if (!chipText.empty()) {
                    const float chipX = cardW - padX - chipW;
                    ui.rect(id + ".chip")
                        .position(chipX, padY)
                        .size(chipW, chipH)
                        .color(t.colors.hover)
                        .radius(chipH * 0.5f)
                        .build();
                    ui.text(id + ".chip.label")
                        .position(chipX, padY)
                        .size(chipW, chipH)
                        .text(chipText)
                        .fontSize(chipFont)
                        .fontWeight(500)
                        .horizontalAlign(eui::HorizontalAlign::Center)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(t.colors.textSecondary)
                        .build();
                }

                // ---- 摘要（12/400 textSecondary，最多 2 行省略）----
                const int lines = estimateLines(row.summary, innerW,
                                                t.sizes.fontSizeSmall);
                const float sumY =
                    padY + core::theme::scaled(kNameRowH + kSummaryTopGap);
                const float sumH =
                    core::theme::scaled(kSummaryLineH * lines);
                ui.text(id + ".summary")
                    .position(padX, sumY)
                    .size(innerW, sumH)
                    .text(row.summary)
                    .fontSize(t.sizes.fontSizeSmall)
                    .color(t.colors.textSecondary)
                    .build();

                // ---- 数据区（分割线下「下次块 + 上次块」两块横排，占满卡宽）----
                const float divY = sumY + sumH + core::theme::scaled(kDataTopGap);
                const float dataY = divY + 1.0f + core::theme::scaled(kDataBottomGap);
                const float guideH = core::theme::scaled(kGuideRowH);
                const float valueH = core::theme::scaled(kValueRowH);
                const float valueY = dataY + guideH + core::theme::scaled(kGuideValueGap);
                ui.rect(id + ".divider")
                    .position(padX, divY)
                    .size(innerW, 1.0f)
                    .color(t.colors.borderDefault)
                    .opacity(dataOpacity)
                    .build();
                // 左块「下次」：15/600 textPrimary 为卡内第一数据锚；无值「—」12 灰
                const bool hasNext =
                    row.nextFireAt.has_value() && row.nextFireAt.value() > 0;
                ui.text(id + ".next.guide")
                    .position(padX, dataY)
                    .size(core::theme::scaled(80.0f), guideH)
                    .text("下次")
                    .fontSize(t.sizes.fontSizeSmall - 1.0f)
                    .color(t.colors.textTertiary)
                    .opacity(dataOpacity)
                    .build();
                ui.text(id + ".next")
                    .position(padX, valueY)
                    .size(std::max(0.0f, innerW * 0.55f - core::theme::scaled(8.0f)), valueH)
                    .text(nextFireText(row.nextFireAt, nowSec))
                    .fontSize(hasNext ? t.sizes.fontSizeBody + 1.0f
                                      : t.sizes.fontSizeSmall)
                    .fontWeight(hasNext ? 600 : 400)
                    .lineHeight(valueH)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(hasNext ? t.colors.textPrimary : t.colors.textTertiary)
                    .opacity(dataOpacity)
                    .build();
                if (row.isSystem) {
                    // 系统卡右块「来源」：模块名 12 + 4 + 「只读」11（超长省略）
                    ui.text(id + ".source.guide")
                        .position(rightX, dataY)
                        .size(core::theme::scaled(80.0f), guideH)
                        .text("来源")
                        .fontSize(t.sizes.fontSizeSmall - 1.0f)
                        .color(t.colors.textTertiary)
                        .build();
                    ui.text(id + ".source")
                        .position(rightX, valueY)
                        .size(std::max(0.0f, rightW - core::theme::scaled(40.0f)), valueH)
                        .text(row.source)
                        .fontSize(t.sizes.fontSizeSmall)
                        .lineHeight(valueH)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(t.colors.textSecondary)
                        .build();
                    ui.text(id + ".readonly")
                        .position(rightX + rightW - core::theme::scaled(36.0f), valueY)
                        .size(core::theme::scaled(36.0f), valueH)
                        .text("只读")
                        .fontSize(t.sizes.fontSizeSmall - 1.0f)
                        .lineHeight(valueH)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(t.colors.textTertiary)
                        .build();
                } else {
                    // 右块「上次」：七态 pill + 时间戳；无记录灰字占位
                    ui.text(id + ".last.guide")
                        .position(rightX, dataY)
                        .size(core::theme::scaled(80.0f), guideH)
                        .text("上次")
                        .fontSize(t.sizes.fontSizeSmall - 1.0f)
                        .color(t.colors.textTertiary)
                        .opacity(dataOpacity)
                        .build();
                    if (row.lastRunAt.has_value() && !row.lastRunStatus.empty()) {
                        const std::string statusZh = runStatusZh(row.lastRunStatus);
                        const float pillW = core::theme::scaled(20.0f) +
                                            textWidth(statusZh, t.sizes.fontSizeSmall);
                        eui::Color pillBg;
                        eui::Color pillFg;
                        runStatusPillColors(row.lastRunStatus, t, pillBg, pillFg);
                        ui.rect(id + ".last.pill")
                            .position(rightX, valueY)
                            .size(pillW, valueH)
                            .color(pillBg)
                            .radius(valueH * 0.5f)
                            .opacity(dataOpacity)
                            .build();
                        ui.text(id + ".last.status")
                            .position(rightX, valueY)
                            .size(pillW, valueH)
                            .text(statusZh)
                            .fontSize(t.sizes.fontSizeSmall)
                            .fontWeight(500)
                            .horizontalAlign(eui::HorizontalAlign::Center)
                            .verticalAlign(eui::VerticalAlign::Center)
                            .color(pillFg)
                            .opacity(dataOpacity)
                            .build();
                        const std::time_t at =
                            static_cast<std::time_t>(row.lastRunAt.value());
                        ui.text(id + ".last.time")
                            .position(rightX + pillW + core::theme::scaled(6.0f), valueY)
                            .size(std::max(0.0f, rightW - pillW - core::theme::scaled(6.0f)), valueH)
                            .text(monthDayText(at) + " " + clockText(at))
                            .fontSize(t.sizes.fontSizeSmall - 1.0f)
                            .lineHeight(valueH)
                            .verticalAlign(eui::VerticalAlign::Center)
                            .color(t.colors.textTertiary)
                            .opacity(dataOpacity)
                            .build();
                    } else {
                        ui.text(id + ".last.none")
                            .position(rightX, valueY)
                            .size(rightW, valueH)
                            .text("无记录")
                            .fontSize(t.sizes.fontSizeSmall)
                            .lineHeight(valueH)
                            .verticalAlign(eui::VerticalAlign::Center)
                            .color(t.colors.textTertiary)
                            .opacity(dataOpacity)
                            .build();
                    }
                }

                // hover 反馈层（§6：混黑 0.05，120ms 过渡；目标值每帧随命中面换）
                ui.rect(id + ".tint")
                    .position(0.0f, 0.0f)
                    .size(cardW, cardH)
                    .color(eui::Color{0.0f, 0.0f, 0.0f, 0.05f})
                    .radius(t.sizes.radiusLarge)
                    .opacity(hovered ? 1.0f : 0.0f)
                    .transition(core::Transition::make(0.12f, core::Ease::OutCubic))
                    .animate(core::AnimProperty::Opacity)
                    .build();

                // 命中面（池140：卡面零控件，命中面即整卡；bug 150 口径）——
                // EUI 兄弟命中逆序遍历、后建者先命中，命中面必须建在卡 stack
                // 内**最后**。用户/归档卡 = 点击开编辑弹窗 + cursor Hand；系统卡
                // 只读 = 无 onClick、cursor Arrow、仅 hover 浏览反馈。
                auto hit = ui.rect(id + ".hit")
                               .position(0.0f, 0.0f)
                               .size(cardW, cardH)
                               .states(kTransparent, kTransparent, kTransparent)
                               .cursor(interactive ? core::CursorShape::Hand
                                                   : core::CursorShape::Arrow)
                               .onHover([this, rowId = id](bool h) {
                                   if (h) hoveredCardIds_.insert(rowId);
                                   else hoveredCardIds_.erase(rowId);
                               });
                if (interactive) {
                    hit.onClick([this, ruleId = row.ruleId] {
                        openEditRule(ruleId);
                    });
                }
                hit.build();
            })
            .build();
    };

    // 段内瀑布流：卡高预排 → masonryLayout 最短列入列 → 逐卡绘制（ui 透传同上⚠）。
    // yFlowOffset = 本段卡在滚动 content 里的绝对 y 基准：卡是 ignoreLayout 绝对
    // 定位，slot 从 (0,0) 起算——归档段若不叠加前方流式高度（用户段 spacer + 折叠
    // 头），卡片会叠到用户卡正上方并抢先命中（点用户卡开归档卡编辑器，bug 150）。
    // 返回本段总高（含自身 spacer），供后续段计算偏移。
    auto drawMasonry = [&](eui::Ui& ui, const std::vector<ListRow>& rows,
                           const std::string& idPrefix, float w,
                           float yFlowOffset) -> float {
        std::vector<float> heights;
        heights.reserve(rows.size());
        const float summaryAvailW =
            std::max(0.0f, w - core::theme::scaled(kCardPad * 2.0f));
        for (const auto& row : rows) {
            heights.push_back(cardHeightForLines(
                estimateLines(row.summary, summaryAvailW, t.sizes.fontSizeSmall)));
        }
        const auto slots = masonryLayout(heights, w, core::theme::scaled(kCardGap));
        // 流式占位：卡在滚动 content 的 column 里绝对定位（ignoreLayout，池133 同款
        // 坑——column 对未 ignoreLayout 子元素会静默忽略 position()），整块高度必须由
        // spacer 贡献，否则滚动内容高塌 0、后续流式兄弟（归档段）叠到卡上。
        float totalH = 0.0f;
        for (const auto& s : slots) totalH = std::max(totalH, s.y + s.h);
        ui.stack(idPrefix + "spacer").size(w, totalH).build();
        for (std::size_t i = 0; i < rows.size(); ++i) {
            drawCard(ui, rows[i], idPrefix + std::to_string(i),
                     {slots[i].x, slots[i].y + yFlowOffset, slots[i].w, slots[i].h});
        }
        return totalH;
    };

    ui.stack("schedule.list.root")
        .size(screen.width, screen.height)
        .content([&] {
            // 页头（池139：全站 kHeaderH=40、标题顶格自页 pad 20 起算，无写死顶距）
            ui.text("schedule.list.title")
                .position(pad, headerY)
                .size(std::max(0.0f, contentW - newBtnW - historyW -
                                        core::theme::scaled(32.0f)), headerH)
                .text("定时")
                .fontSize(t.sizes.fontSizeTitle)
                .lineHeight(headerH)
                .fontWeight(600)
                .color(t.colors.textPrimary)
                .build();
            ui.text("schedule.list.history")
                .position(pageW - pad - newBtnW - core::theme::scaled(16.0f) - historyW,
                          headerY)
                .size(historyW, headerH)
                .text("运行历史")
                .fontSize(t.sizes.fontSizeBody)
                .lineHeight(headerH)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(t.colors.primaryText)
                .onClick([this] { openHistory(); })
                .build();
            // 新建（池137：全页唯一新建入口；空态纯文字提示后恒 Primary）
            ui_kit::button(ui, "schedule.list.new", "新建",
                           ui_kit::ButtonLevel::Primary, pageW - pad - newBtnW,
                           headerY, newBtnW, ui_kit::kButtonHeight, t,
                           {[this] { openNewRule(); }});

            // TAB 行（全站 wtab 块面式现行口径；计数紧跟选中 tab 之后）
            const char* kTabLabels[2] = {"系统定时", "用户定时"};
            float tabX = pad;
            for (int i = 0; i < 2; ++i) {
                const bool active = i == activeTab_;
                const float tabW =
                    textWidth(kTabLabels[i], ui_kit::kWtabFontSize) + 24.0f;
                ui_kit::wtab(ui, "schedule.list.tab." + std::to_string(i),
                             kTabLabels[i], active, tabX, tabY, tabW, tabRowH, t,
                             [this, i] { setTab(i); });
                tabX += tabW + core::theme::scaled(6.0f);
                if (active) {
                    const int n = i == 0
                                      ? static_cast<int>(assembly_.systemRows.size())
                                      : static_cast<int>(assembly_.userRows.size());
                    const std::string countText = "共 " + std::to_string(n) + " 个";
                    const float countW =
                        textWidth(countText, t.sizes.fontSizeSmall) +
                        core::theme::scaled(8.0f);
                    ui.text("schedule.list.tabcount")
                        .position(tabX, tabY)
                        .size(countW, tabRowH)
                        .text(countText)
                        .fontSize(t.sizes.fontSizeSmall)
                        .lineHeight(tabRowH)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(t.colors.textTertiary)
                        .build();
                    tabX += countW + core::theme::scaled(6.0f);
                }
            }

            // toast（仅 error_ 非空时插入，滚动区顶缩）
            if (!error_.empty()) {
                ui.text("schedule.list.toast")
                    .position(pad, tabY + tabRowH + core::theme::scaled(8.0f))
                    .size(contentW, toastH)
                    .text(error_)
                    .fontSize(t.sizes.fontSizeSmall)
                    .lineHeight(toastH)
                    .color(t.colors.errorText)
                    .build();
            }

            // 唯一滚动区（§1.2：外壳/页头/TAB 不滚，瀑布流自滚）
            components::scrollView(ui, "schedule.list.scroll")
                .position(pad, scrollY)
                .size(contentW, scrollH)
                .theme(comp)
                .content([&](eui::Ui& ui, float innerW, float innerH) {
                    (void)innerH;
                    const float w = std::max(0.0f, innerW);
                    const bool userTab = activeTab_ == 1;
                    ui.column("schedule.list.content")
                        .width(w)
                        .height(core::SizeValue::wrapContent())
                        // TAB 切换 120ms 淡入：切帧目标 0，次帧回 1 由过渡插值
                        .opacity(tabFadePending_ ? 0.0f : 1.0f)
                        .transition(core::Transition::make(0.12f, core::Ease::OutCubic))
                        .animate(core::AnimProperty::Opacity)
                        .content([&] {
                            const std::vector<ListRow>& rows =
                                userTab ? assembly_.userRows : assembly_.systemRows;
                            float userMasonryH = 0.0f;
                            if (rows.empty()) {
                                // 空态（池137）：贴左灰字纯提示，缩进 16 与卡对齐；
                                // 无卡无任何按钮，唯一新建入口在页头
                                ui.text(userTab ? "schedule.list.user.empty"
                                                : "schedule.list.sys.empty")
                                    .position(core::theme::scaled(16.0f),
                                              core::theme::scaled(8.0f))
                                    .size(std::max(0.0f, w - core::theme::scaled(32.0f)),
                                          core::theme::scaled(20.0f))
                                    .text(userTab ? "暂无用户定时" : "暂无系统定时")
                                    .fontSize(t.sizes.fontSizeSmall)
                                    .color(t.colors.textTertiary)
                                    .build();
                            } else {
                                userMasonryH = drawMasonry(ui, rows,
                                                           userTab ? "schedule.list.user."
                                                                   : "schedule.list.sys.",
                                                           w, 0.0f);
                            }
                            if (userTab) {
                                // 归档折叠段（§1.1：仅用户 TAB 滚动区尾部，默认折叠）
                                ui.row("schedule.list.archive.header")
                                    .size(w, core::theme::scaled(32.0f))
                                    .alignItems(eui::Align::CENTER)
                                    .gap(t.sizes.spacingSmall)
                                    .onClick([this] {
                                        archiveExpanded_ = !archiveExpanded_;
                                    })
                                    .content([&] {
                                        ui.text("schedule.list.archive.chevron")
                                            .size(core::theme::scaled(16.0f),
                                                  core::theme::scaled(22.0f))
                                            .icon(eui::utf8(archiveExpanded_
                                                                ? kChevronDown
                                                                : kChevronRight))
                                            .color(t.colors.textTertiary)
                                            .build();
                                        ui.text("schedule.list.archive.label")
                                            .size(core::theme::scaled(240.0f),
                                                  core::theme::scaled(22.0f))
                                            .text("已归档 (" +
                                                  std::to_string(
                                                      assembly_.archivedRows.size()) +
                                                  ")")
                                            .fontSize(t.sizes.fontSizeBody)
                                            .color(t.colors.textSecondary)
                                            .build();
                                    })
                                    .build();
                                if (archiveExpanded_ &&
                                    !assembly_.archivedRows.empty()) {
                                    // 卡基线 = 用户段流高 + 折叠头高（32），见
                                    // drawMasonry 的 yFlowOffset 注释
                                    drawMasonry(ui, assembly_.archivedRows,
                                                "schedule.list.archive.", w,
                                                userMasonryH +
                                                    core::theme::scaled(32.0f));
                                }
                            }
                        })
                        .build();
                })
                .build();

            // TAB 切换一次性标记消费：滚动回顶（帧末取件）+ 淡入翻回 1
            if (pendingScrollReset_) {
                pendingScrollReset_ = false;
                ui.setScrollOffset("schedule.list.scroll", 0.0f);
            }
            if (tabFadePending_) {
                tabFadePending_ = false;
            }
        })
        .build();

    // 运行历史抽屉（关闭态也常驻构建，EUI 收起过渡依赖节点存在；zIndex 990 盖过列表）
    ui::drawHistoryDrawer(ui, screen, t, historyState_, historyHost_);

    // 删除确认小弹窗（DialogBuilder 契约：open(false) 时节点常驻、仅禁交互）
    {
        const auto themeTokens = core::theme::componentTokensWeChat(t);
        const float w = 460.0f;
        const float h = 230.0f;
        const float x0 = ui_kit::kDialogInsetX;
        const float innerW = w - ui_kit::kDialogBodyInsetX;
        components::dialog(ui, "schedule.list.deleteConfirm")
            .open(deleteConfirmOpen())
            .screen(screen.width, screen.height)
            .size(w, h)
            .theme(themeTokens)
            .zIndex(1200)
            .onOpenChange([this](bool v) { if (!v) cancelPendingDelete(); })
            .content([&] {
                ui_kit::drawDialogTitle(ui, "schedule.list.deleteConfirm.title",
                                        w, "删除定时规则", ui_kit::kDialogTitleFontSize,
                                        t.colors.textPrimary,
                                        {.y = 16.0f, .height = 28.0f, .lineHeight = 28.0f});
                ui_kit::drawDialogText(
                    ui, "schedule.list.deleteConfirm.body", w,
                    "确定删除「" + pendingDeleteName_ +
                        "」吗？默认仅软删（配置与运行历史保留）。",
                    t.sizes.fontSizeSmall, t.colors.textSecondary,
                    {.y = 56.0f, .height = 20.0f, .lineHeight = 20.0f});
                // 不用 components::checkbox：其头文件在 -Werror 下有 unused
                // variable（EUI-NEO 上游问题），「彻底删除」改行内开关同语义
                ui.text("schedule.list.deleteConfirm.purge.label")
                    .position(x0, 92.0f)
                    .size(innerW - 60.0f, 24.0f)
                    .text("彻底删除（物理删除规则，运行历史保留）")
                    .fontSize(t.sizes.fontSizeSmall)
                    .color(t.colors.textSecondary)
                    .build();
                components::toggleSwitch(ui, "schedule.list.deleteConfirm.purge")
                    .position(w - ui_kit::kDialogInsetX - 52.0f, 90.0f)
                    .size(44.0f, 24.0f)
                    .checked(pendingDeletePurge_)
                    .theme(themeTokens)
                    .onChange([this](bool v) { pendingDeletePurge_ = v; })
                    .build();
                components::button(ui, "schedule.list.deleteConfirm.cancel")
                    .position(x0, h - 60.0f)
                    .size(120.0f, 36.0f)
                    .text("取消")
                    .colors(t.colors.background, t.colors.hover,
                            t.colors.primarySoftBg)
                    .textColor(t.colors.textPrimary)
                    .radius(t.sizes.radius)
                    .border(1.0f, t.colors.borderDefault)
                    .onClick([this] { cancelPendingDelete(); })
                    .build();
                components::button(ui, "schedule.list.deleteConfirm.ok")
                    .position(w - 120.0f - ui_kit::kDialogInsetX, h - 60.0f)
                    .size(120.0f, 36.0f)
                    .text("删除")
                    .colors(t.colors.primary, t.colors.primaryHover,
                            t.colors.primaryPressed)
                    .textColor(t.colors.onPrimary)
                    .radius(t.sizes.radius)
                    .onClick([this] { confirmPendingDelete(); })
                    .build();
            })
            .build();
    }

    // 规则编辑弹窗（关闭态也常驻构建，EUI 内建收起过渡依赖节点存在）
    {
        core::schedule::FireContext ctx;
        ctx.nowUnix = static_cast<int64_t>(core::time::nowUnix());
        core::time::localTime(static_cast<std::time_t>(ctx.nowUnix), &ctx.localNow);
        ui::drawEditDialog(ui, screen, t, editState_, editHost_, ctx);
    }
}

} // namespace module::schedule
