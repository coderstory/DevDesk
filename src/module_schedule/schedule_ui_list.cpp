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
#include "ui_kit/empty_state.h"
#include "ui_kit/jelly.h"
#include "ui_kit/materials.h"
#include "ui_kit/metrics.h"

#include <components/button.h>
#include <components/dialog.h>
#include <components/scrollview.h>
#include <components/switch.h>
#include <components/theme.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <utility>

namespace module::schedule {

namespace {

// 页内常量（1000×640 最小档设计口径，弹性布局随屏宽伸缩）
constexpr float kPagePad = 12.0f;      // 与 t.sizes.spacingPage 同值
constexpr float kRowHeight = 64.0f;
// 相邻行卡间距（= 智能体列表 kTaskCardGap 同值，全站业务卡纵向节奏一致）
constexpr float kRowCardGap = 8.0f;
// 行卡在内容列内的左右内缩（卡宽 = 内容宽 − 2×inset，全站 L2 卡同口径）
constexpr float kRowCardInset = 10.0f;
// 行卡上下的呼吸内边距（卡高 = 行高 + 2×pad，设计 §3.1 卡化口径）
constexpr float kRowCardPad = 12.0f;
constexpr int kHistoryLimit = 50;      // 抽屉装载与展示的 runs 上限（assembleHistory 截断）
constexpr unsigned int kLockGlyph = 0xF023;      // fa-lock（系统段锁图标）
constexpr unsigned int kChevronDown = 0xF107;    // fa-angle-down
constexpr unsigned int kChevronRight = 0xF105;   // fa-angle-right

// 上次结果状态 → 中文（与 schedule_run.status 值域对齐，§4.5）
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

void ScheduleListUi::resetForTest() {    archiveExpanded_ = false;
    error_.clear();
    assembly_ = ListAssembly{};
    historyState_ = ui::HistoryDrawerState{};
    editState_ = ui::EditDialogState{};
    cancelPendingDelete();
}

void ScheduleListUi::compose(eui::Ui& ui, const eui::Screen& screen) {
    const auto& t = core::theme::current();
    const auto comp = core::theme::componentTokensWeChat(t);
            const float pad = kPagePad;
            const bool hasUserRows = !assembly_.userRows.empty();

    components::scrollView(ui, "schedule.list.scroll")
        .size(screen.width, screen.height)
        .theme(comp)
        .content([&](eui::Ui& ui, float innerW, float innerH) {
            (void)innerH;
            const float contentW = std::max(0.0f, innerW - pad * 2.0f);

            // 段标题（锁图标 + 标题文本；flow 行布局）
            auto sectionTitle = [&](const std::string& id,
                                    const std::string& text, bool lock) {
                ui.row(id)
                    .size(contentW, core::theme::scaled(22.0f))
                    .alignItems(eui::Align::CENTER)
                    .gap(t.sizes.spacingSmall)
                    .content([&] {
                        if (lock) {
                            ui.text(id + ".lock")
                                .size(core::theme::scaled(16.0f),
                                      core::theme::scaled(22.0f))
                                .icon(eui::utf8(kLockGlyph))
                                .color(t.colors.textTertiary)
                                .build();
                        }
                        ui.text(id + ".text")
                            .size(core::theme::scaled(120.0f),
                                  core::theme::scaled(22.0f))
                            .text(text)
                            .fontSize(t.sizes.fontSizeLarge)
                            .fontWeight(600)
                            .color(t.colors.textPrimary)
                            .build();
                    })
                    .build();
            };

            // 行内绝对定位（stack 行，几何手工排布，与 settings 卡同口径）：
            // 右缘三列固定宽（下次 110 / 上次 150 / 开关或锁 40），左列弹性。
            // 行卡几何（F-67-1）：L2 业务卡 = 内容列内缩 kRowCardInset 的全宽卡
            // （不限宽：定时行信息量大）；行内容整体右移同一偏移，相对布局不变。
            const float cardX = kRowCardInset;
            const float cardW = std::max(0.0f, contentW - kRowCardInset * 2.0f);
            const float rightEdge = cardX + cardW;  // 卡内右侧基准

            const float nextW = core::theme::scaled(110.0f);
            const float lastW = core::theme::scaled(150.0f);
            const float tailW = nextW + lastW + 20.0f;
            const float leftW = cardW - tailW - 56.0f;
            const float nameW = std::max(60.0f, leftW - 24.0f);
            // 卡高 = 行内容高 + 2×pad（设计 §3.1）；行内绝对定位的 y 一律以 padY 为原点。
            // B6/F-69-2：行高与内边距随字号派生——字变大而卡高不变会把二级行挤出卡外。
            const float rowH = core::theme::scaled(kRowHeight);
            const float rowPad = core::theme::scaled(kRowCardPad);
            const float rowCardH = rowH + rowPad * 2.0f;
            const float padY = rowPad;

            const eui::Color kTransparent{0.0f, 0.0f, 0.0f, 0.0f};

            // 行级文字操作（用户行专属，编辑/删除排在启停开关左侧；
            // 「上次」列整体左移给操作让位）。rowId 用于把控件上的 hover 并入
            // 行卡 hover 集合（指针移到文字上时卡不闪回常态，见 §10.0.1 约束 3）。
            auto drawRowAction = [&](const std::string& id, float x,
                                     const std::string& text, bool danger,
                                     const std::string& rowId,
                                     std::function<void()> onClick) {
                ui.text(id)
                    .position(x, padY + core::theme::scaled(22.0f))
                    .size(core::theme::scaled(34.0f), core::theme::scaled(20.0f))
                    .text(text)
                    .fontSize(t.sizes.fontSizeSmall)
                    .color(danger ? t.colors.errorText : t.colors.primaryText)
                    .onHover([this, rowId](bool h) {
                        if (h) hoveredRowControlIds_.insert(rowId);
                        else hoveredRowControlIds_.erase(rowId);
                    })
                    .onClick(std::move(onClick))
                    .build();
            };

            // 卡 hover = 命中面 ∪ 卡内控件（两集合，见头文件说明）
            auto rowHovered = [&](const std::string& id) {
                return hoveredRowIds_.count(id) > 0 ||
                       hoveredRowControlIds_.count(id) > 0;
            };

            auto drawUserRow = [&](const ListRow& row, const std::string& id) {
                const bool hovered = rowHovered(id);
                ui.stack(id)
                    .size(contentW, rowCardH)
                    .content([&] {
                        ui_kit::jellyCard(ui, id + ".bg", cardX, 0.0f, cardW,
                                          rowCardH, t, {}, hovered);
                        ui.text(id + ".name")
                            .position(cardX + 16.0f, padY + core::theme::scaled(10.0f))
                            .size(nameW, core::theme::scaled(20.0f))
                            .text(row.name)
                            .fontSize(t.sizes.fontSizeBody)
                            .fontWeight(500)
                            .color(t.colors.textPrimary)
                            .build();
                        ui.text(id + ".summary")
                            .position(cardX + 16.0f, padY + core::theme::scaled(34.0f))
                            .size(nameW, core::theme::scaled(16.0f))
                            .text(row.summary)
                            .fontSize(t.sizes.fontSizeSmall)
                            .color(t.colors.textSecondary)
                            .build();
                        ui.text(id + ".next")
                            .position(rightEdge - tailW - 56.0f, padY + core::theme::scaled(22.0f))
                            .size(nextW, core::theme::scaled(20.0f))
                            .text("下次 " + nextFireText(row.nextFireAt,
                                static_cast<int64_t>(core::time::nowUnix())))
                            .fontSize(t.sizes.fontSizeSmall)
                            .color(t.colors.textSecondary)
                            .build();
                        ui.text(id + ".last")
                            .position(rightEdge - lastW - 140.0f, padY + core::theme::scaled(22.0f))
                            .size(lastW, core::theme::scaled(20.0f))
                            .text("上次 " + lastRunText(row,
                                static_cast<int64_t>(core::time::nowUnix())))
                            .fontSize(t.sizes.fontSizeSmall)
                            .color(t.colors.textSecondary)
                            .build();
                        // 命中面（hover 源）：铺满卡、三态同色、不接点击（点行卡无动作）。
                        // 构建在静态内容之后、可点控件之前：EUI 兄弟命中逆序遍历，后建者先命中
                        // → 编辑/删除/开关仍可点。cursor 显式回 Arrow（states 默认 Hand）。
                        ui.rect(id + ".hit")
                            .position(cardX, 0.0f)
                            .size(cardW, rowCardH)
                            .states(kTransparent, kTransparent, kTransparent)
                            .cursor(core::CursorShape::Arrow)
                            .onHover([this, rowId = id](bool h) {
                                if (h) hoveredRowIds_.insert(rowId);
                                else hoveredRowIds_.erase(rowId);
                            })
                            .build();
                        drawRowAction(id + ".edit", rightEdge - 124.0f, "编辑",
                                      /*danger=*/false, id,
                                      [this, ruleId = row.ruleId] {
                                          openEditRule(ruleId);
                                      });
                        drawRowAction(id + ".delete", rightEdge - 86.0f, "删除",
                                      /*danger=*/true, id,
                                      [this, ruleId = row.ruleId] {
                                          requestDeleteRule(ruleId);
                                      });
                        // 开关 hover 并入行卡 hover 集合（同 drawRowAction 口径）：
                        // 指针停在开关上时卡不闪回常态（SwitchBuilder.onHover → .hit 命中面）。
                        components::toggleSwitch(ui, id + ".switch")
                            .position(rightEdge - 44.0f, padY + core::theme::scaled(20.0f))
                            .size(44.0f, 24.0f)
                            .checked(row.enabled)
                            .theme(comp)
                            .onHover([this, rowId = id](bool h) {
                                if (h) hoveredRowControlIds_.insert(rowId);
                                else hoveredRowControlIds_.erase(rowId);
                            })
                            .onChange([this, ruleId = row.ruleId](bool v) {
                                const auto r = setRuleEnabled(ruleId, v);
                                if (r.isFail()) {
                                    error_ = "开关切换失败：" + r.errorMessage();
                                    LOG_WARN("schedule 列表：开关切换失败 — " +
                                             r.errorMessage());
                                } else {
                                    reload(static_cast<int64_t>(
                                        core::time::nowUnix()));
                                }
                            })
                            .build();
                    })
                    .build();
            };

            auto drawSystemRow = [&](const ListRow& row, const std::string& id) {
                const bool hovered = rowHovered(id);
                ui.stack(id)
                    .size(contentW, rowCardH)
                    .content([&] {
                        ui_kit::jellyCard(ui, id + ".bg", cardX, 0.0f, cardW,
                                          rowCardH, t, {}, hovered);
                        ui.text(id + ".lock")
                            .position(cardX + 16.0f, padY + core::theme::scaled(21.0f))
                            .size(core::theme::scaled(16.0f),
                                  core::theme::scaled(22.0f))
                            .icon(eui::utf8(kLockGlyph))
                            .color(t.colors.textTertiary)
                            .build();
                        ui.text(id + ".name")
                            .position(cardX + 40.0f, padY + core::theme::scaled(10.0f))
                            .size(nameW - 8.0f, core::theme::scaled(20.0f))
                            .text(row.name)
                            .fontSize(t.sizes.fontSizeBody)
                            .fontWeight(500)
                            .color(t.colors.textPrimary)
                            .build();
                        ui.text(id + ".summary")
                            .position(cardX + 40.0f, padY + core::theme::scaled(34.0f))
                            .size(nameW - 8.0f, core::theme::scaled(16.0f))
                            .text(row.summary)
                            .fontSize(t.sizes.fontSizeSmall)
                            .color(t.colors.textSecondary)
                            .build();
                        ui.text(id + ".source")
                            .position(rightEdge - tailW - 56.0f + nextW - 60.0f,
                                      padY + core::theme::scaled(10.0f))
                            .size(core::theme::scaled(60.0f),
                                  core::theme::scaled(18.0f))
                            .text(row.source)
                            .fontSize(t.sizes.fontSizeSmall)
                            .color(t.colors.textTertiary)
                            .build();
                        ui.text(id + ".next")
                            .position(rightEdge - tailW - 56.0f, padY + core::theme::scaled(22.0f))
                            .size(nextW, core::theme::scaled(20.0f))
                            .text("下次 " + nextFireText(row.nextFireAt,
                                static_cast<int64_t>(core::time::nowUnix())))
                            .fontSize(t.sizes.fontSizeSmall)
                            .color(t.colors.textSecondary)
                            .build();
                        ui.text(id + ".readonly")
                            .position(rightEdge - lastW - 48.0f, padY + core::theme::scaled(22.0f))
                            .size(lastW, core::theme::scaled(20.0f))
                            .text("只读 · 执行归来源模块")
                            .fontSize(t.sizes.fontSizeSmall)
                            .color(t.colors.textTertiary)
                            .build();
                        // 命中面：系统行全卡无操作控件，整卡做 hover 源即可
                        ui.rect(id + ".hit")
                            .position(cardX, 0.0f)
                            .size(cardW, rowCardH)
                            .states(kTransparent, kTransparent, kTransparent)
                            .cursor(core::CursorShape::Arrow)
                            .onHover([this, rowId = id](bool h) {
                                if (h) hoveredRowIds_.insert(rowId);
                                else hoveredRowIds_.erase(rowId);
                            })
                            .build();
                    })
                    .build();
            };

            ui.column("schedule.list.root")
                .width(innerW)
                .height(core::SizeValue::wrapContent())
                .padding(pad, 40.0f, pad, t.sizes.spacingXLarge)
                .gap(t.sizes.spacingLarge)
                .content([&] {
                    // 页头：标题 + 弹性占位 + 运行历史抽屉入口 / 新建（历史靠右、新建最右）
                    ui.row("schedule.list.header")
                        .size(contentW, core::theme::scaled(32.0f))
                        .alignItems(eui::Align::CENTER)
                        .content([&] {
                            ui.text("schedule.list.title")
                                .width(core::SizeValue::wrapContent())
                                .height(core::theme::scaled(32.0f))
                                .text("定时")
                                .fontSize(t.sizes.fontSizeTitle)
                                .lineHeight(core::theme::scaled(32.0f))
                                .fontWeight(600)
                                .color(t.colors.textPrimary)
                                .build();
                            ui.stack("schedule.list.header.spacer")
                                .height(core::theme::scaled(24.0f))
                                .flexGrow(1.0f)
                                .build();
                            ui.text("schedule.list.history")
                                .size(core::theme::scaled(96.0f),
                                      core::theme::scaled(24.0f))
                                .text("运行历史")
                                .fontSize(t.sizes.fontSizeBody)
                                .color(t.colors.primaryText)
                                .onClick([this] { openHistory(); })
                                .build();
                            // 新建入口（C2）：空态时降级 Secondary——此时用户定时空态卡
                            // 中央已有引导钮，页头不再抢注意力；有数据保持 Primary。
                            // 判据与用户空态卡同源（userRows 为空），保证不会同屏两个绿钮。
                            ui_kit::button(
                                ui, "schedule.list.new", "新建",
                                hasUserRows ? ui_kit::ButtonLevel::Primary
                                            : ui_kit::ButtonLevel::Secondary,
                                0.0f, 0.0f, core::theme::scaled(88.0f), 40.0f,
                                t, {[this] { openNewRule(); }});
                        })
                        .build();

                    if (!error_.empty()) {
                        ui.text("schedule.list.toast")
                            .size(contentW, core::theme::scaled(20.0f))
                            .text(error_)
                            .fontSize(t.sizes.fontSizeSmall)
                            .color(t.colors.errorText)
                            .build();
                    }

                    // 系统定时段（锁图标只读）
                    sectionTitle("schedule.list.sys.title", "系统定时",
                                 /*lock=*/true);
                    if (assembly_.systemRows.empty()) {
                        ui.stack("schedule.list.sys.empty.wrap")
                            .width(contentW)
                            .height(core::theme::scaled(64.0f))
                            .content([&] {
                                // 空态也铺 L2 材质：段内一块空白不带底，与有数据时的
                                // 行卡不成序列（S1 判据：卡内/卡外采样有差）。
                                ui.rect("schedule.list.sys.empty.card")
                                    .position(kRowCardInset, 0.0f)
                                    .size(std::max(0.0f, contentW - kRowCardInset * 2.0f),
                                          core::theme::scaled(64.0f))
                                    .color(ui_kit::itemSurface(t))
                                    .radius(t.sizes.radiusLarge)
                                    .border(1.0f, t.colors.borderDefault)
                                    .build();
                                ui_kit::emptyState(
                                    ui, "schedule.list.sys.empty", "暂无系统定时",
                                    std::nullopt, kRowCardInset, 0.0f,
                                    std::max(0.0f, contentW - kRowCardInset * 2.0f),
                                    core::theme::scaled(64.0f), t);
                            })
                            .build();
                    }
                    if (!assembly_.systemRows.empty()) {
                        // 段内行卡同容器、间距 kRowCardGap（S3：相邻卡间距 = 全站卡间距）
                        ui.column("schedule.list.sys.rows")
                            .width(contentW)
                            .height(core::SizeValue::wrapContent())
                            .gap(kRowCardGap)
                            .content([&] {
                                for (size_t i = 0;
                                     i < assembly_.systemRows.size(); ++i) {
                                    drawSystemRow(
                                        assembly_.systemRows[i],
                                        "schedule.list.sys." + std::to_string(i));
                                }
                            })
                            .build();
                    }

                    // 用户定时段
                    sectionTitle("schedule.list.user.title", "用户定时",
                                 /*lock=*/false);
                    if (assembly_.userRows.empty()) {
                        ui.stack("schedule.list.user.empty.wrap")
                            .width(contentW)
                            .height(core::theme::scaled(96.0f))
                            .content([&] {
                                ui.rect("schedule.list.user.empty.card")
                                    .position(kRowCardInset, 0.0f)
                                    .size(std::max(0.0f, contentW - kRowCardInset * 2.0f),
                                          core::theme::scaled(96.0f))
                                    .color(ui_kit::itemSurface(t))
                                    .radius(t.sizes.radiusLarge)
                                    .border(1.0f, t.colors.borderDefault)
                                    .build();
                                ui_kit::emptyState(
                                    ui, "schedule.list.user.empty", "暂无用户定时",
                                    std::make_optional(std::make_pair(
                                        std::string("新建定时"),
                                        [this] { openNewRule(); })),
                                    kRowCardInset, 0.0f,
                                    std::max(0.0f, contentW - kRowCardInset * 2.0f),
                                    core::theme::scaled(96.0f), t);
                            })
                            .build();
                    }
                    if (!assembly_.userRows.empty()) {
                        ui.column("schedule.list.user.rows")
                            .width(contentW)
                            .height(core::SizeValue::wrapContent())
                            .gap(kRowCardGap)
                            .content([&] {
                                for (size_t i = 0;
                                     i < assembly_.userRows.size(); ++i) {
                                    drawUserRow(
                                        assembly_.userRows[i],
                                        "schedule.list.user." + std::to_string(i));
                                }
                            })
                            .build();
                    }

                    // 一次性已执行折叠区（§4.4：archived_at 非空默认折叠）
                    ui.row("schedule.list.archive.header")
                        .size(contentW, core::theme::scaled(22.0f))
                        .alignItems(eui::Align::CENTER)
                        .gap(t.sizes.spacingSmall)
                        .onClick([this] { archiveExpanded_ = !archiveExpanded_; })
                        .content([&] {
                            ui.text("schedule.list.archive.chevron")
                                .size(core::theme::scaled(16.0f),
                                      core::theme::scaled(22.0f))
                                .icon(eui::utf8(archiveExpanded_ ? kChevronDown
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
                    if (archiveExpanded_) {
                        ui.column("schedule.list.archive.rows")
                            .width(contentW)
                            .height(core::SizeValue::wrapContent())
                            .gap(kRowCardGap)
                            .content([&] {
                                for (size_t i = 0;
                                     i < assembly_.archivedRows.size(); ++i) {
                                    drawUserRow(
                                        assembly_.archivedRows[i],
                                        "schedule.list.archive." +
                                            std::to_string(i));
                                }
                            })
                            .build();
                    }
                })
                .build();
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
