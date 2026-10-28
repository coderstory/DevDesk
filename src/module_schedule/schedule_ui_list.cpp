#include "module_schedule/schedule_ui_list.h"

#include "core/component_theme.h"
#include "core/logging.h"
#include "core/theme.h"
#include "core/time.h"
#include "platform/json.h"

#include <components/scrollview.h>
#include <components/switch.h>
#include <components/theme.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace module::schedule {

namespace {

// 页内常量（1000×640 最小档设计口径，弹性布局随屏宽伸缩）
constexpr float kPagePad = 12.0f;      // 与 t.sizes.spacingPage 同值
constexpr float kRowHeight = 64.0f;
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

// spec_json 安全取字符串数组字段；缺失/非数组 → 空
std::vector<std::string> specStringArray(const platform::json::JsonObject& obj,
                                         const char* key) {
    std::vector<std::string> out;
    if (!obj.has(key)) return out;
    const auto v = obj.get(key);
    if (!v.isArray()) return out;
    for (int i = 0; i < v.size(); ++i) {
        const auto elem = v[i];
        if (elem.isString()) out.push_back(elem.asString());
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
            const int64_t nth = specInt(spec, "nth_weekday_nth", 0);
            const std::string wd =
                weekdaysSummary(specStringArray(spec, "nth_weekday"));
            if (nth > 0 && !wd.empty()) {
                return "每月第 " + std::to_string(nth) + " 个" + wd + at;
            }
            return "每月" + at;
        }
        if (unit == "year") {
            const int64_t month = specInt(spec, "month", 0);
            const int64_t day = specInt(spec, "month_day", 0);
            if (month > 0 && day > 0) {
                return "每年 " + std::to_string(month) + " 月 " +
                       std::to_string(day) + " 日" + at;
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
                               ScheduleProviderRegistry& providers)
    : store_(store), providers_(providers) {}

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

void ScheduleListUi::resetForTest() {
    archiveExpanded_ = false;
    error_.clear();
    assembly_ = ListAssembly{};
}

void ScheduleListUi::compose(eui::Ui& ui, const eui::Screen& screen) {
    const auto& t = core::theme::current();
    const auto comp = core::theme::componentTokensWeChat(t);
    const float pad = kPagePad;

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
            auto drawRowFrame = [&](const std::string& id, float w, float h) {
                ui.rect(id + ".bg")
                    .position(0.0f, 0.0f)
                    .size(w, h)
                    .color(t.colors.surfaceElevated)
                    .radius(t.sizes.radiusLarge)
                    .border(1.0f, t.colors.borderMuted)
                    .build();
            };

            const float nextW = core::theme::scaled(110.0f);
            const float lastW = core::theme::scaled(150.0f);
            const float tailW = nextW + lastW + 20.0f;
            const float leftW = contentW - tailW - 56.0f;
            const float nameW = std::max(60.0f, leftW - 24.0f);

            auto drawUserRow = [&](const ListRow& row, const std::string& id) {
                ui.stack(id)
                    .size(contentW, kRowHeight)
                    .content([&] {
                        drawRowFrame(id, contentW, kRowHeight);
                        ui.text(id + ".name")
                            .position(16.0f, 10.0f)
                            .size(nameW, core::theme::scaled(20.0f))
                            .text(row.name)
                            .fontSize(t.sizes.fontSizeBody)
                            .fontWeight(500)
                            .color(t.colors.textPrimary)
                            .build();
                        ui.text(id + ".summary")
                            .position(16.0f, 34.0f)
                            .size(nameW, core::theme::scaled(16.0f))
                            .text(row.summary)
                            .fontSize(t.sizes.fontSizeSmall)
                            .color(t.colors.textSecondary)
                            .build();
                        ui.text(id + ".next")
                            .position(contentW - tailW - 56.0f, 22.0f)
                            .size(nextW, core::theme::scaled(20.0f))
                            .text("下次 " + nextFireText(row.nextFireAt,
                                static_cast<int64_t>(core::time::nowUnix())))
                            .fontSize(t.sizes.fontSizeSmall)
                            .color(t.colors.textSecondary)
                            .build();
                        ui.text(id + ".last")
                            .position(contentW - lastW - 48.0f, 22.0f)
                            .size(lastW, core::theme::scaled(20.0f))
                            .text("上次 " + lastRunText(row,
                                static_cast<int64_t>(core::time::nowUnix())))
                            .fontSize(t.sizes.fontSizeSmall)
                            .color(t.colors.textSecondary)
                            .build();
                        components::toggleSwitch(ui, id + ".switch")
                            .position(contentW - 44.0f, 20.0f)
                            .size(44.0f, 24.0f)
                            .checked(row.enabled)
                            .theme(comp)
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
                ui.stack(id)
                    .size(contentW, kRowHeight)
                    .content([&] {
                        drawRowFrame(id, contentW, kRowHeight);
                        ui.text(id + ".lock")
                            .position(16.0f, 21.0f)
                            .size(core::theme::scaled(16.0f),
                                  core::theme::scaled(22.0f))
                            .icon(eui::utf8(kLockGlyph))
                            .color(t.colors.textTertiary)
                            .build();
                        ui.text(id + ".name")
                            .position(40.0f, 10.0f)
                            .size(nameW - 8.0f, core::theme::scaled(20.0f))
                            .text(row.name)
                            .fontSize(t.sizes.fontSizeBody)
                            .fontWeight(500)
                            .color(t.colors.textPrimary)
                            .build();
                        ui.text(id + ".summary")
                            .position(40.0f, 34.0f)
                            .size(nameW - 8.0f, core::theme::scaled(16.0f))
                            .text(row.summary)
                            .fontSize(t.sizes.fontSizeSmall)
                            .color(t.colors.textSecondary)
                            .build();
                        ui.text(id + ".source")
                            .position(contentW - tailW - 56.0f + nextW - 60.0f,
                                      10.0f)
                            .size(core::theme::scaled(60.0f),
                                  core::theme::scaled(18.0f))
                            .text(row.source)
                            .fontSize(t.sizes.fontSizeSmall)
                            .color(t.colors.textTertiary)
                            .build();
                        ui.text(id + ".next")
                            .position(contentW - tailW - 56.0f, 22.0f)
                            .size(nextW, core::theme::scaled(20.0f))
                            .text("下次 " + nextFireText(row.nextFireAt,
                                static_cast<int64_t>(core::time::nowUnix())))
                            .fontSize(t.sizes.fontSizeSmall)
                            .color(t.colors.textSecondary)
                            .build();
                        ui.text(id + ".readonly")
                            .position(contentW - lastW - 48.0f, 22.0f)
                            .size(lastW, core::theme::scaled(20.0f))
                            .text("只读 · 执行归来源模块")
                            .fontSize(t.sizes.fontSizeSmall)
                            .color(t.colors.textTertiary)
                            .build();
                    })
                    .build();
            };

            ui.column("schedule.list.root")
                .width(innerW)
                .height(core::SizeValue::wrapContent())
                .padding(pad, 0.0f, pad, t.sizes.spacingXLarge)
                .gap(t.sizes.spacingLarge)
                .content([&] {
                    // 页头：标题 + 运行历史抽屉入口（抽屉本体 S7b 接，本波只留按钮位）
                    ui.row("schedule.list.header")
                        .size(contentW, core::theme::scaled(32.0f))
                        .alignItems(eui::Align::CENTER)
                        .content([&] {
                            ui.text("schedule.list.title")
                                .size(core::theme::scaled(200.0f),
                                      core::theme::scaled(32.0f))
                                .text("定时")
                                .fontSize(t.sizes.fontSizeTitle)
                                .fontWeight(600)
                                .color(t.colors.textPrimary)
                                .build();
                            ui.text("schedule.list.history")
                                .size(core::theme::scaled(96.0f),
                                      core::theme::scaled(24.0f))
                                .text("运行历史")
                                .fontSize(t.sizes.fontSizeBody)
                                .color(t.colors.primaryText)
                                .onClick([] {
                                    // S7b：此处打开运行历史抽屉；本波入口位占位
                                    LOG_INFO("schedule 列表：运行历史入口（S7b 接入抽屉）");
                                })
                                .build();
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
                        ui.text("schedule.list.sys.empty")
                            .size(contentW, core::theme::scaled(20.0f))
                            .text("暂无系统定时")
                            .fontSize(t.sizes.fontSizeSmall)
                            .color(t.colors.textTertiary)
                            .build();
                    }
                    for (size_t i = 0; i < assembly_.systemRows.size(); ++i) {
                        drawSystemRow(assembly_.systemRows[i],
                                      "schedule.list.sys." + std::to_string(i));
                    }

                    // 用户定时段
                    sectionTitle("schedule.list.user.title", "用户定时",
                                 /*lock=*/false);
                    if (assembly_.userRows.empty()) {
                        ui.text("schedule.list.user.empty")
                            .size(contentW, core::theme::scaled(20.0f))
                            .text("暂无用户定时")
                            .fontSize(t.sizes.fontSizeSmall)
                            .color(t.colors.textTertiary)
                            .build();
                    }
                    for (size_t i = 0; i < assembly_.userRows.size(); ++i) {
                        drawUserRow(assembly_.userRows[i],
                                    "schedule.list.user." + std::to_string(i));
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
                        for (size_t i = 0; i < assembly_.archivedRows.size();
                             ++i) {
                            drawUserRow(
                                assembly_.archivedRows[i],
                                "schedule.list.archive." + std::to_string(i));
                        }
                    }
                });
        });
}

} // namespace module::schedule
