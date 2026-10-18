// 微博热榜响应解析实现（契约见 hot_parse.h）。
#include "module_agent/hot_parse.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include "core/str_util.h"
#include "platform/json.h"

namespace module::agent {

namespace {

namespace pj = platform::json;

// JsonValue 把 Int / Double 都归 T_Number，但内部只填一处：Int 分支 asDouble()
// 为 0，Double 分支 asInt() 为 0（与 quota 的 jsonNumber/jsonInteger 同款回退）。
bool numberToInt64(const pj::JsonValue& value, int64_t& out) {
    if (!value.isNumber()) return false;
    const double d = value.asDouble();
    if (d != 0.0) {
        // 热度和 realpos 都是小整数；这里仍按 int64 边界拒绝离谱值，避免转换 UB。
        if (!std::isfinite(d) || d < -9.0e18 || d > 9.0e18) return false;
        out = static_cast<int64_t>(d);
        return true;
    }
    out = value.asInt();
    return true;
}

// 内部纯函数：保留 A-Z a-z 0-9 - . _ ~，其余按 UTF-8 字节编码为 %XX（大写）。
// `#` 走通用分支 → %23；中文按 UTF-8 字节逐个编码。
std::string percentEncode(const std::string& text) {
    static const char kHex[] = "0123456789ABCDEF";
    const auto isUnreserved = [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
               c == '~';
    };
    std::string out;
    out.reserve(text.size() * 3);
    for (const char ch : text) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (isUnreserved(byte)) {
            out.push_back(ch);
        } else {
            out.push_back('%');
            out.push_back(kHex[byte >> 4]);
            out.push_back(kHex[byte & 0x0F]);
        }
    }
    return out;
}

struct RankedItem {
    int64_t realpos = 0;
    HotItem item;
};

} // namespace

core::Result<std::vector<HotItem>> parseWeiboHot(std::string_view json) {
    auto parsed = pj::parse(std::string(json));
    if (parsed.isFail()) {
        return core::Result<std::vector<HotItem>>::fail(
            core::ErrorCode::JsonParseError,
            "微博热榜响应不是合法 JSON（根节点）：" + parsed.errorMessage());
    }
    if (!parsed.value().isObject()) {
        return core::Result<std::vector<HotItem>>::fail(
            core::ErrorCode::ValidationError,
            "微博热榜响应根节点不是 JSON 对象（根节点）");
    }

    const pj::JsonObject root = parsed.value().asObject();
    int64_t ok = 0;
    if (!numberToInt64(root.get("ok"), ok) || ok != 1) {
        return core::Result<std::vector<HotItem>>::fail(
            core::ErrorCode::ValidationError,
            "微博热榜响应 ok 字段缺失或不为 1（ok）");
    }

    const pj::JsonValue data = root.get("data");
    if (!data.isObject()) {
        return core::Result<std::vector<HotItem>>::fail(
            core::ErrorCode::ValidationError,
            "微博热榜响应 data 字段缺失或不是 JSON 对象（data）");
    }
    const pj::JsonValue realtime = data.asObject().get("realtime");
    if (!realtime.isArray()) {
        return core::Result<std::vector<HotItem>>::fail(
            core::ErrorCode::ValidationError,
            "微博热榜响应 data.realtime 字段缺失或不是 JSON 数组"
            "（data.realtime）");
    }

    // hotgov / hotgovs 是独立的置顶政务条目，刻意不读：它们不占 top10 名额。
    std::vector<RankedItem> ranked;
    const int count = realtime.size();
    ranked.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const pj::JsonValue entry = realtime[i];
        if (!entry.isObject()) continue;   // 单条坏项跳过，不让整榜失败
        const pj::JsonObject item = entry.asObject();

        int64_t isAd = 0;
        if (numberToInt64(item.get("is_ad"), isAd) && isAd == 1) continue;

        int64_t realpos = 0;
        if (!numberToInt64(item.get("realpos"), realpos) || realpos < 1) {
            continue;   // 广告以外的无排名项也跳过
        }
        if (realpos > std::numeric_limits<int>::max()) continue;

        const pj::JsonValue word = item.get("word");
        if (!word.isString()) continue;
        std::string title = core::str::trimAscii(word.asString());
        if (title.empty()) continue;

        // 链接优先用话题形式 word_scheme；它为空/非字符串时退回 word。
        std::string query = title;
        const pj::JsonValue scheme = item.get("word_scheme");
        if (scheme.isString()) {
            std::string candidate = core::str::trimAscii(scheme.asString());
            if (!candidate.empty()) query = std::move(candidate);
        }

        HotItem out;
        out.title = std::move(title);
        int64_t hot = 0;
        if (numberToInt64(item.get("num"), hot)) out.hot = hot;
        out.url = "https://s.weibo.com/weibo?q=" + percentEncode(query);
        ranked.push_back(RankedItem{realpos, std::move(out)});
    }

    // realpos 是 1-based 权威排名；rank 字段 0-based 不可靠，不读。
    // stable_sort 让同 realpos 的异常数据保持响应原序。
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const RankedItem& a, const RankedItem& b) {
                         return a.realpos < b.realpos;
                     });

    std::vector<HotItem> result;
    result.reserve(ranked.size());
    int rank = 1;
    for (auto& entry : ranked) {
        entry.item.rank = rank++;
        result.push_back(std::move(entry.item));
    }
    return core::Result<std::vector<HotItem>>::ok(std::move(result));
}

std::vector<HotItem> takeTopN(std::vector<HotItem> items, std::size_t n) {
    if (n == 0) return {};
    if (n < items.size()) items.resize(n);
    return items;
}

} // namespace module::agent
