// 热榜响应解析实现（契约见 hot_parse.h）。
#include "module_agent/hot_parse.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <system_error>
#include <utility>

#include "core/str_util.h"
#include "platform/json.h"

namespace module::agent {

namespace {

namespace pj = platform::json;

using HotResult = core::Result<std::vector<HotItem>>;

HotResult failHot(core::ErrorCode code, std::string message) {
    return HotResult::fail(code, std::move(message));
}

// 解析根对象：四个源共用「合法 JSON + 根是对象」的前置条件，错误带源名与根路径。
core::Result<pj::JsonValue> parseRoot(std::string_view json, const char* source) {
    auto parsed = pj::parse(std::string(json));
    if (parsed.isFail()) {
        return core::Result<pj::JsonValue>::fail(
            core::ErrorCode::JsonParseError,
            std::string(source) + "响应不是合法 JSON（根节点）：" +
                parsed.errorMessage());
    }
    if (!parsed.value().isObject()) {
        return core::Result<pj::JsonValue>::fail(
            core::ErrorCode::ValidationError,
            std::string(source) + "响应根节点不是 JSON 对象（根节点）");
    }
    return parsed;
}

// JSON number / 数字字符串 → 非负 int64。
//
// number：Int 分支读 asInt()；Double 分支要求有限、非负、是整数且在 int64 上界内
//（2^63，避免 static_cast 越界 UB）。字符串：仅 allowString=true 时用 from_chars，
// 必须整串消费（"4e3" / "1.5" / 前后非空白字符都判失败）。
// 微博 T3a 契约明确「num 为字符串 → nullopt」，故微博调用传 allowString=false；
// 百度 hotScore / 头条 HotValue 传 true。
bool jsonToInt64(const pj::JsonValue& value, bool allowString, int64_t& out) {
    if (value.isNumber()) {
        const double d = value.asDouble();
        if (d != 0.0) {
            // Int 分支 asDouble() 恒 0，统一在此按 double 处理；
            // Double 0.0 落到下面的 asInt()（也是 0）。
            if (!std::isfinite(d) || d < 0.0 || d >= 9223372036854775808.0) {
                return false;
            }
            if (std::trunc(d) != d) return false;
            out = static_cast<int64_t>(d);
            return true;
        }
        const int64_t i = value.asInt();
        if (i < 0) return false;
        out = i;
        return true;
    }
    if (!allowString || !value.isString()) return false;

    const std::string text = core::str::trimAscii(value.asString());
    if (text.empty()) return false;
    int64_t parsed = 0;
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc() || result.ptr != end || parsed < 0) return false;
    out = parsed;
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

bool isHttpUrl(const std::string& url) {
    return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
}

// 链接优先 primary（url），为空 / 非 http(s) 时退回 fallback（appUrl）；
// 两者都不可用时返回空串（不把 javascript: 之类交给 UI 打开）。
std::string pickHttpUrl(const pj::JsonObject& item, const char* primary,
                        const char* fallback) {
    const pj::JsonValue primaryValue = item.get(primary);
    if (primaryValue.isString() && isHttpUrl(primaryValue.asString())) {
        return primaryValue.asString();
    }
    const pj::JsonValue fallbackValue = item.get(fallback);
    if (fallbackValue.isString() && isHttpUrl(fallbackValue.asString())) {
        return fallbackValue.asString();
    }
    return {};
}

struct RankedItem {
    int64_t key = 0;
    HotItem item;
};

// 微博 side/hotSearch 与 statuses/hot_band 的单条规则完全相同：
// 过滤 is_ad=1 / 缺 realpos、标题 trim、word_scheme 优先、num 编码为搜索链接。
bool weiboEntryToRanked(const pj::JsonValue& entry, RankedItem& out) {
    if (!entry.isObject()) return false;   // 单条坏项跳过，不让整榜失败
    const pj::JsonObject item = entry.asObject();

    int64_t isAd = 0;
    if (jsonToInt64(item.get("is_ad"), /*allowString=*/false, isAd) && isAd == 1) {
        return false;
    }

    int64_t realpos = 0;
    if (!jsonToInt64(item.get("realpos"), /*allowString=*/false, realpos) ||
        realpos < 1) {
        return false;   // 广告以外的无排名项也跳过
    }
    if (realpos > std::numeric_limits<int>::max()) return false;

    const pj::JsonValue word = item.get("word");
    if (!word.isString()) return false;
    std::string title = core::str::trimAscii(word.asString());
    if (title.empty()) return false;

    // 链接优先用话题形式 word_scheme；它为空/非字符串时退回 word。
    std::string query = title;
    const pj::JsonValue scheme = item.get("word_scheme");
    if (scheme.isString()) {
        std::string candidate = core::str::trimAscii(scheme.asString());
        if (!candidate.empty()) query = std::move(candidate);
    }

    HotItem parsed;
    parsed.title = std::move(title);
    int64_t hot = 0;
    if (jsonToInt64(item.get("num"), /*allowString=*/false, hot)) {
        parsed.hot = hot;
    }
    parsed.url = "https://s.weibo.com/weibo?q=" + percentEncode(query);

    out.key = realpos;
    out.item = std::move(parsed);
    return true;
}

// 按 key 升序稳定重排并赋 1-based 连续 rank。
std::vector<HotItem> flattenRanked(std::vector<RankedItem> ranked) {
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const RankedItem& a, const RankedItem& b) {
                         return a.key < b.key;
                     });

    std::vector<HotItem> result;
    result.reserve(ranked.size());
    int rank = 1;
    for (auto& entry : ranked) {
        entry.item.rank = rank++;
        result.push_back(std::move(entry.item));
    }
    return result;
}

} // namespace

core::Result<std::vector<HotItem>> parseWeiboHot(std::string_view json) {
    auto root = parseRoot(json, "微博热榜");
    if (root.isFail()) return failHot(root.errorCode(), root.errorMessage());
    const pj::JsonObject top = root.value().asObject();

    int64_t ok = 0;
    if (!jsonToInt64(top.get("ok"), /*allowString=*/false, ok) || ok != 1) {
        return failHot(core::ErrorCode::ValidationError,
                       "微博热榜响应 ok 字段缺失或不为 1（ok）");
    }

    const pj::JsonValue data = top.get("data");
    if (!data.isObject()) {
        return failHot(core::ErrorCode::ValidationError,
                       "微博热榜响应 data 字段缺失或不是 JSON 对象（data）");
    }
    const pj::JsonValue realtime = data.asObject().get("realtime");
    if (!realtime.isArray()) {
        return failHot(core::ErrorCode::ValidationError,
                       "微博热榜响应 data.realtime 字段缺失或不是 JSON 数组"
                       "（data.realtime）");
    }

    // hotgov / hotgovs 是独立的置顶政务条目，刻意不读：它们不占 top10 名额。
    std::vector<RankedItem> ranked;
    const int count = realtime.size();
    ranked.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        RankedItem entry;
        if (weiboEntryToRanked(realtime[i], entry)) {
            ranked.push_back(std::move(entry));
        }
    }
    return HotResult::ok(flattenRanked(std::move(ranked)));
}

core::Result<std::vector<HotItem>> parseWeiboBand(std::string_view json) {
    auto root = parseRoot(json, "微博 hot_band 热榜");
    if (root.isFail()) return failHot(root.errorCode(), root.errorMessage());
    const pj::JsonObject top = root.value().asObject();

    const pj::JsonValue data = top.get("data");
    if (!data.isObject()) {
        return failHot(core::ErrorCode::ValidationError,
                       "微博 hot_band 热榜结构变化：data 缺失或不是 JSON 对象"
                       "（data）");
    }
    const pj::JsonValue bandList = data.asObject().get("band_list");
    if (!bandList.isArray()) {
        return failHot(core::ErrorCode::ValidationError,
                       "微博 hot_band 热榜结构变化：data.band_list 缺失或不是 JSON 数组"
                       "（data.band_list）");
    }

    // data.hotgov 是独立置顶政务条目，刻意不读。
    std::vector<RankedItem> ranked;
    const int count = bandList.size();
    ranked.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        RankedItem entry;
        if (weiboEntryToRanked(bandList[i], entry)) {
            ranked.push_back(std::move(entry));
        }
    }
    return HotResult::ok(flattenRanked(std::move(ranked)));
}

core::Result<std::vector<HotItem>> parseBaiduHot(std::string_view json) {
    auto root = parseRoot(json, "百度热榜");
    if (root.isFail()) return failHot(root.errorCode(), root.errorMessage());
    const pj::JsonObject top = root.value().asObject();

    const pj::JsonValue success = top.get("success");
    if (!success.isBool() || !success.asBool()) {
        return failHot(core::ErrorCode::ValidationError,
                       "百度热榜结构变化：success 缺失或不为 true（success）");
    }

    const pj::JsonValue data = top.get("data");
    if (!data.isObject()) {
        return failHot(core::ErrorCode::ValidationError,
                       "百度热榜结构变化：data 缺失或不是 JSON 对象（data）");
    }
    const pj::JsonValue cards = data.asObject().get("cards");
    if (!cards.isArray()) {
        return failHot(core::ErrorCode::ValidationError,
                       "百度热榜结构变化：data.cards 缺失或不是数组（data.cards）");
    }

    // 不写死 cards[0]：按 component 找热榜卡片。data.topContent 是独立置顶专题，不读。
    pj::JsonValue content;
    bool found = false;
    for (int i = 0; i < cards.size(); ++i) {
        const pj::JsonValue card = cards[i];
        if (!card.isObject()) continue;
        const pj::JsonValue component = card.asObject().get("component");
        if (!component.isString() || component.asString() != "hotList") continue;
        content = card.asObject().get("content");
        found = true;
        break;
    }
    if (!found) {
        return failHot(core::ErrorCode::ValidationError,
                       "百度热榜结构变化：未找到 component 为 hotList 的卡片"
                       "（data.cards）");
    }
    if (!content.isArray()) {
        return failHot(core::ErrorCode::ValidationError,
                       "百度热榜结构变化：hotList.content 缺失或不是数组"
                       "（data.cards[].content）");
    }

    std::vector<RankedItem> ranked;
    const int count = content.size();
    ranked.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const pj::JsonValue entry = content[i];
        if (!entry.isObject()) continue;
        const pj::JsonObject item = entry.asObject();

        const pj::JsonValue word = item.get("word");
        if (!word.isString()) continue;
        std::string title = core::str::trimAscii(word.asString());
        if (title.empty()) continue;

        HotItem parsed;
        parsed.title = std::move(title);
        int64_t hot = 0;
        // hotScore 实测是字符串；也接受 JSON number。缺失/非数字/负数 → nullopt。
        if (jsonToInt64(item.get("hotScore"), /*allowString=*/true, hot)) {
            parsed.hot = hot;
        }
        parsed.url = pickHttpUrl(item, "url", "appUrl");

        // index 是 0-based 排序键；缺失时用数组下标，稳定排序保持相对顺序。
        int64_t index = 0;
        if (!jsonToInt64(item.get("index"), /*allowString=*/false, index)) {
            index = i;
        }
        ranked.push_back(RankedItem{index, std::move(parsed)});
    }
    return HotResult::ok(flattenRanked(std::move(ranked)));
}

core::Result<std::vector<HotItem>> parseToutiaoHot(std::string_view json) {
    auto root = parseRoot(json, "今日头条热榜");
    if (root.isFail()) return failHot(root.errorCode(), root.errorMessage());
    const pj::JsonObject top = root.value().asObject();

    const pj::JsonValue status = top.get("status");
    if (!status.isString() || status.asString() != "success") {
        return failHot(core::ErrorCode::ValidationError,
                       "今日头条热榜结构变化：status 缺失或不为 success（status）");
    }

    const pj::JsonValue data = top.get("data");
    if (!data.isArray()) {
        return failHot(core::ErrorCode::ValidationError,
                       "今日头条热榜结构变化：data 缺失或不是数组（data）");
    }

    // fixed_top_data 是独立置顶数据（没有 HotValue），刻意不读。
    std::vector<HotItem> result;
    const int count = data.size();
    result.reserve(static_cast<std::size_t>(count));
    int rank = 1;
    for (int i = 0; i < count; ++i) {
        const pj::JsonValue entry = data[i];
        if (!entry.isObject()) continue;
        const pj::JsonObject item = entry.asObject();

        const pj::JsonValue titleValue = item.get("Title");
        if (!titleValue.isString()) continue;
        std::string title = core::str::trimAscii(titleValue.asString());
        if (title.empty()) continue;

        HotItem parsed;
        parsed.title = std::move(title);
        int64_t hot = 0;
        // HotValue 实测是字符串；也接受 JSON number。缺失/非数字/负数 → nullopt。
        if (jsonToInt64(item.get("HotValue"), /*allowString=*/true, hot)) {
            parsed.hot = hot;
        }
        const pj::JsonValue url = item.get("Url");
        if (url.isString() && isHttpUrl(url.asString())) {
            parsed.url = url.asString();   // 原样保留长 URL，不按 & 截断
        }
        parsed.rank = rank++;
        result.push_back(std::move(parsed));
    }
    return HotResult::ok(std::move(result));
}

core::Result<std::vector<HotItem>> parseHotList(HotSource source,
                                                std::string_view json) {
    switch (source) {
        case HotSource::Weibo: return parseWeiboHot(json);
        case HotSource::WeiboBand: return parseWeiboBand(json);
        case HotSource::Baidu: return parseBaiduHot(json);
        case HotSource::Toutiao: return parseToutiaoHot(json);
    }
    return failHot(core::ErrorCode::ValidationError, "未知热榜源（source）");
}

std::vector<HotItem> takeTopN(std::vector<HotItem> items, std::size_t n) {
    if (n == 0) return {};
    if (n < items.size()) items.resize(n);
    return items;
}

} // namespace module::agent
