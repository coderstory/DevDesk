// 定时调度规则模型（core::schedule）：六 kind 规则的解析、校验、nextFire / missedFires。
// 纯函数层契约：
//   - 不读系统时钟：所有时刻（now / after / from / to）由调用方注入；本地时区换算只走
//     core::time::localTime / std::mktime(tm_isdst=-1)，不依赖进程状态。
//   - 不抛异常：一切错误走 Result；不碰 IO / DB / UI / platform / EUI-NEO。
//   - 时区纪律（与模块层引擎的约定）：日历/cron 规则存本地墙钟字段；推进日历日用
//     core::time::daysFromCivil 的日序号运算 + mktime 回算，禁止 +86400；春季跳变缺口
//     （墙钟不存在）在「该日最早存在时刻」触发；秋季回拨重复墙钟只返回第一次出现
//     （nextFire 严格大于 afterUnix，第二次出现天然跳过）；时钟回拨不重放由引擎比较
//     lastEvalAt 承担（本层无状态，只保证 nextFire 永不返回 ≤ afterUnix 的时刻）。
//   - v1 只支持系统本地时区：ctx.tzId 非空即 fail(NotImplemented)（宁可报错不可在错误
//     时区静默计算）；tzId 仅预留透传。
// 反重构警告：nextFire 的「严格大于 afterUnix」与「重复墙钟只取第一次」是时钟回拨
// 不重放、秋季不双触发两条验收的根基，改写解析/推进逻辑时不得破坏。
#pragma once
#include <array>
#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <vector>

#include "cron.h"
#include "result.h"

namespace core::schedule {

// 六种规则 kind：once/interval/calendar/cron 有时间线，event/resident 无下次时刻
enum class RuleKind {
    Once,      // 一次性：at（Unix 秒）或 at_local（本地墙钟），到点后由引擎归档
    Interval,  // 周期：anchor + ceil((after-anchor)/N)*N 锚点网格，停机只算一次到期（引擎职责）
    Calendar,  // 日历：unit day/week/month/year + 日选择器 + 墙钟 time
    Cron,      // cron 5 段「分 时 日 月 周」（复用 core::cron，不支持 L/W/# 与周奇偶）
    Event,     // 事件驱动：无下次时刻，事件到达由引擎判定冷却
    Resident,  // 值守：仅手动/内部入队
};

// 日历规则的日选择语义（本地墙钟日期匹配，不含时刻——时刻统一由 hour/minute 承担）
struct CalendarSpec {
    enum class Unit { Day, Week, Month, Year };
    Unit unit = Unit::Day;
    int interval = 1;                 // ≥1；day/week 的相位以 anchor_date 为第 0 天/第 0 周
    std::array<bool, 7> weekdays{};   // [0..6]，0=周日；仅 unit=week 使用
    bool workdaysOnly = false;        // 过滤周六日（法定节假日 v1 不处理）
    bool hasMonthDay = false;         // 与 nth_weekday / last_workday 三选一（month/year 必选一）
    int monthDay = 1;                 // 1..31；当月不存在该日则跳过（2 月 31 日 → 3 月 31 日）
    bool hasNthWeekday = false;
    int nth = 1;                      // 第 N 个周几，N ∈ [1,5]；当月不足 N 次则跳过
    int nthWeekday = 1;               // 0..6
    bool lastWorkday = false;         // 当月最后一个工作日（周一至周五）
    int hour = 0;                     // time "HH:MM"
    int minute = 0;
    int64_t anchorDayNum = 0;         // anchor_date 的 daysFromCivil 值（day/week 相位锚点）
};

// 解析/计算后的规则（由 parseRuleSpec 产出；引擎只读，不手工构造）
struct RuleSpec {
    RuleKind kind = RuleKind::Event;
    // once：at 与 at_local 解析时已校验互斥
    bool hasAt = false;
    int64_t atUnix = 0;               // 绝对 Unix 秒
    bool hasAtLocal = false;
    std::tm atLocal{};                // 本地墙钟（tm_year=1900 起、tm_mon=0 起、tm_sec=0）
    // interval
    int64_t everySec = 0;             // ≥1
    int64_t anchor = 0;               // ≥0，默认 0=epoch；网格相位锚点
    int64_t windowSec = 0;            // ≥0 且 < everySec；仅漂移容差，不参与 nextFire 计算
    // calendar
    CalendarSpec calendar;
    // cron
    cron::CronExpr cron;              // core::cron::CronExpr
    // event：事件名列表为引擎判定载荷；cooldownSec 由引擎执行冷却
    std::vector<std::string> events;
    int64_t cooldownSec = 0;
};

// 引擎每 tick 注入的上下文（本层不做换算信任：一切 unix → 墙钟换算以实参时刻为准，
// localNow 仅在 afterUnix == nowUnix 时作日历推进起点快照使用）
struct FireContext {
    int64_t nowUnix = 0;              // 当前 Unix 秒
    std::tm localNow{};               // nowUnix 的本地墙钟（引擎经 core::time::localTime 填）
    std::string tzId;                 // v1 恒空 = 系统本地；非空 → fail(NotImplemented)
    int64_t lastEvalAt = 0;           // 引擎上次评估时刻；nowUnix < lastEvalAt 时引擎只更新该值不重放
};

// 校验并解析 kind + spec_json → RuleSpec；字段缺失/越界/互斥/未知字段一律
// fail(ValidationError) 带明确中文文案。
Result<RuleSpec> parseRuleSpec(RuleKind kind, const std::string& specJson);

// 严格大于 afterUnix 的下一次触发时刻；无下次（一次性已过 / event / resident /
// 超出可表示范围 / calendar 400 年窗口无匹配 / cron 8 个 366 天窗口内无匹配）
// 返回 ok(nullopt)——六 kind 统一口径：无下次不是错误，fail 只留给参数非法。
Result<std::optional<int64_t>> nextFire(const RuleSpec& spec, int64_t afterUnix,
                                        const FireContext& ctx);

// [fromUnix, toUnix]（含两端）内的全部触发时刻，最多 cap 个（超出截断，引擎据此
// 合并「遗漏 N 次」摘要）；秋季回拨重复墙钟每（本地日期, 时分）只返回第一次。
// from > to 或 cap < 1 → fail(ValidationError)。
Result<std::vector<int64_t>> missedFires(const RuleSpec& spec, int64_t fromUnix,
                                         int64_t toUnix, int cap, const FireContext& ctx);

} // namespace core::schedule
