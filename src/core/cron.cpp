// cron 5 字段解析 + 匹配：纯函数，无 IO 无时钟。支持 * / 单值 / a-b / a-b/c /
// */c / a/c / 逗号列表；拒绝字段数≠5、空字段、越界、逆序区间、步长 0、非数字
#include "cron.h"

#include <charconv>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "time.h"

namespace core::cron {
namespace {

// 严格整数解析：空串 / 尾随字符 / 溢出 / 非数字一律拒。前导 '-' 由越界检查兜住
bool parseNumber(const std::string& text, int& out) {
    if (text.empty()) return false;
    int value = 0;
    const auto res = std::from_chars(text.data(), text.data() + text.size(), value);
    if (res.ec != std::errc() || res.ptr != text.data() + text.size()) return false;
    out = value;
    return true;
}

// 按空格切分（连续空格合一，首尾 trim）；制表符等其他空白留字段里由 parseNumber 拒
std::vector<std::string> splitFields(const std::string& expr) {
    size_t begin = 0, end = expr.size();
    while (begin < end && expr[begin] == ' ') ++begin;
    while (end > begin && expr[end - 1] == ' ') --end;
    std::vector<std::string> fields;
    std::string cur;
    for (size_t i = begin; i < end; ++i) {
        if (expr[i] == ' ') {
            if (!cur.empty()) {
                fields.push_back(cur);
                cur.clear();
            }
        } else {
            cur.push_back(expr[i]);
        }
    }
    if (!cur.empty()) fields.push_back(cur);
    return fields;
}

// 逗号列表的一段：`*` / 单值 / `a-b`，可带 `/c` 步长；`a/c` = a 起、步长 c、到 hi 止
bool parseItem(const std::string& item, int lo, int hi, bool* out) {
    if (item.empty()) return false;
    std::string base = item;
    int step = 1;
    const size_t slash = item.find('/');
    if (slash != std::string::npos) {
        base = item.substr(0, slash);
        if (!parseNumber(item.substr(slash + 1), step) || step < 1) return false;
        if (base.empty()) return false;
    }
    int start = lo;
    int end = hi;                       // `*` 与带步长单值的上界都是 hi
    if (base != "*") {
        const size_t dash = base.find('-');
        if (dash != std::string::npos) {
            if (!parseNumber(base.substr(0, dash), start)) return false;
            if (!parseNumber(base.substr(dash + 1), end)) return false;
            if (start > end) return false;              // 逆序区间
        } else {
            if (!parseNumber(base, start)) return false;
            if (slash == std::string::npos) end = start;  // 无步长单值 = 该值本身
        }
    }
    if (start < lo || end > hi) return false;           // 越界
    // 步长上界 = 字段跨度：拒绝巨大 step，堵死 v += step 有符号溢出 → 数组越界写的 UB
    if (step > hi - lo + 1) return false;
    for (int v = start; v <= end;) {
        out[v] = true;
        if (step > end - v) break;   // 再步进必越过 end（且 end-v ≥ 0，无溢出）
        v += step;
    }
    return true;
}

// 单字段解析：`*`（无步长）= 全 true 且 non-restricted；其余形态 restricted。
bool parseField(const std::string& text, int lo, int hi, bool* out, bool& restricted) {
    if (text == "*") {
        for (int v = lo; v <= hi; ++v) out[v] = true;
        restricted = false;
        return true;
    }
    restricted = true;
    size_t begin = 0;
    while (true) {
        const size_t comma = text.find(',', begin);
        const std::string item = comma == std::string::npos
                                     ? text.substr(begin)
                                     : text.substr(begin, comma - begin);
        if (!parseItem(item, lo, hi, out)) return false;   // 空段（前导/尾随/双逗号）同拒
        if (comma == std::string::npos) break;
        begin = comma + 1;
    }
    return true;
}

// 五个字段的取值域（`分 时 日 月 周`）
constexpr int kLo[5] = {0, 0, 1, 1, 0};
constexpr int kHi[5] = {59, 23, 31, 12, 6};
constexpr const char* kNames[5] = {"分", "时", "日", "月", "周"};

} // namespace

core::Result<CronExpr> parseCron(const std::string& expr) {
    const std::vector<std::string> fields = splitFields(expr);
    if (fields.size() != 5)
        return core::Result<CronExpr>::fail(
            core::ErrorCode::ValidationError,
            "cron 表达式必须是 5 字段（分 时 日 月 周，支持 * , - /），实得 " +
                std::to_string(fields.size()) + " 字段：" + expr);

    CronExpr c;
    bool ignored = false;               // 分/时/月字段的 restricted 标志无匹配语义
    bool* flags[5] = {&ignored, &ignored, &c.domRestricted, &ignored, &c.dowRestricted};
    bool* outs[5] = {c.minutes.data(), c.hours.data(), c.doms.data(),
                     c.months.data(), c.dows.data()};
    for (int i = 0; i < 5; ++i) {
        if (!parseField(fields[i], kLo[i], kHi[i], outs[i], *flags[i]))
            return core::Result<CronExpr>::fail(
                core::ErrorCode::ValidationError,
                std::string("cron ") + kNames[i] + " 字段非法「" + fields[i] + "」：" +
                    expr);
    }
    return core::Result<CronExpr>::ok(std::move(c));
}

bool cronMatches(const CronExpr& c, const std::tm& local) {
    if (local.tm_min < 0 || local.tm_min > 59) return false;
    if (local.tm_hour < 0 || local.tm_hour > 23) return false;
    if (local.tm_mon < 0 || local.tm_mon > 11) return false;
    if (!c.minutes[local.tm_min]) return false;
    if (!c.hours[local.tm_hour]) return false;
    if (!c.months[local.tm_mon + 1]) return false;

    const int mday = local.tm_mday;
    const int wday = local.tm_wday;    // 0=周日（由 core::time::localTime 填充）
    const bool domHit = mday >= 1 && mday <= 31 && c.doms[mday];
    const bool dowHit = wday >= 0 && wday <= 6 && c.dows[wday];
    // 日/周双受限 → vixie「或」（命中任一）；仅一方受限 → 该方必须命中；皆 `*` → 恒真
    if (c.domRestricted && c.dowRestricted) return domHit || dowHit;
    if (c.domRestricted) return domHit;
    if (c.dowRestricted) return dowHit;
    return true;
}

core::Result<std::tm> nextCronMatch(const CronExpr& c, std::time_t fromSec) {
    // 候选 = fromSec 之后的每个分钟边界（floor((fromSec+60)/60) = fromSec/60 + 1，
    // 恰为 fromSec 所在分钟的下一分钟）；上限 366*1440 步覆盖一年（含闰）所有分钟
    constexpr int64_t kMaxSteps = 366LL * 1440;
    const int64_t firstMinute = static_cast<int64_t>(fromSec) / 60 + 1;
    for (int64_t step = 0; step < kMaxSteps; ++step) {
        const std::time_t t = static_cast<std::time_t>((firstMinute + step) * 60);
        std::tm local{};
        core::time::localTime(t, &local);   // tm_wday 同步填充（cronMatches 依赖）
        if (cronMatches(c, local)) return core::Result<std::tm>::ok(local);
    }
    return core::Result<std::tm>::fail(core::ErrorCode::ValidationError,
                                       "一年内无触发时间");
}

core::Result<std::tm> previousCronMatch(const CronExpr& c, std::time_t fromSec) {
    // 与 nextCronMatch 的边界差：**包含** fromSec 所在分钟——run 在 09:00:30 启动，
    // cron `0 9 * * *` 的计划时刻应算 09:00（「计划 vs 实际」对照口径，spec §8.4）。
    // 向前逐分钟回扫，上限同 366*1440 分钟；到顶（或越过 1970 纪元下界）fail。
    // 纯函数：不读真时钟，fromSec 由调用方注入；时区转换只走 core::time::localTime。
    constexpr int64_t kMaxSteps = 366LL * 1440;
    const int64_t fromMinute = static_cast<int64_t>(fromSec) / 60;
    for (int64_t step = 0; step < kMaxSteps; ++step) {
        const int64_t minute = fromMinute - step;
        if (minute < 0) break;   // 纪元前：不可能有合法匹配
        const std::time_t t = static_cast<std::time_t>(minute * 60);
        std::tm local{};
        core::time::localTime(t, &local);   // tm_wday 同步填充（cronMatches 依赖）
        if (cronMatches(c, local)) return core::Result<std::tm>::ok(local);
    }
    return core::Result<std::tm>::fail(core::ErrorCode::ValidationError,
                                       "一年内无匹配时间");
}

} // namespace core::cron
