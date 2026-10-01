// src/module_agent/cron.cpp
// cron 5 字段解析 + 匹配（agent M2 Task 3，spec §5.1 R4）：纯函数，无 IO 无时钟。
// 语法域：`*` / 单值 / `a-b` / `a-b/c` / `*/c` / `a/c` / 逗号列表（逐段同构解析）。
// 拒绝域：字段数≠5、空字段、越界、逆序区间（a>b）、步长 0、非数字（含 L/W/#/英文名）。
#include "module_agent/cron.h"

#include <charconv>
#include <string>
#include <utility>
#include <vector>

namespace module::agent {
namespace {

// 严格整数解析：空串 / 尾随字符 / 溢出 / 非数字（含 L W # 与英文名）一律拒。
// 前导 '-' 由 from_chars 接受、随后在越界检查（lo ≥ 0）被拦，无需特判。
bool parseNumber(const std::string& text, int& out) {
    if (text.empty()) return false;
    int value = 0;
    const auto res = std::from_chars(text.data(), text.data() + text.size(), value);
    if (res.ec != std::errc() || res.ptr != text.data() + text.size()) return false;
    out = value;
    return true;
}

// 按空格切分（连续空格视为一个分隔；首尾空白 trim 掉）。
// 制表符等其它空白不做分隔——它们留在字段里，随后被 parseNumber 拒掉。
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

// 逗号列表的一段：`*` / 单值 / `a-b`，三种形态各自可带 `/c` 步长。
// `a/c` 按「a 起、步长 c、到上界 hi 止」解释（a=lo 时与 `*/c` 等价）。
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
    // 步长上界 = 字段跨度（hi-lo+1）：步长大于跨度只可能在起点命中一次，等价写法
    // 是单值——拒绝它同时堵死「巨大 step 让步进 `v += step` 有符号溢出 → 数组越界写」
    // 这条 UB（评审实测：`5/2147483647 * * * *` / `0 0 */2147483647 * *` SEGV）。
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
    // 日/周双受限 → vixie「或」（命中任一即命中）；仅一方受限 → 该方必须命中；
    // 两方皆 `*` → 恒真
    if (c.domRestricted && c.dowRestricted) return domHit || dowHit;
    if (c.domRestricted) return domHit;
    if (c.dowRestricted) return dowHit;
    return true;
}

} // namespace module::agent
