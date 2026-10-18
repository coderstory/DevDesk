// tests/test_modules/test_hot_tool.cpp
// hot.list 工具契约（fake fetcher + fake clock，绝不联网）：
//   1) 微博成功：请求 URL/Referer/UA/8s 超时、top10、rank、hot null、本地时间格式；
//   2) 源链降级：微博 403 → hot_band / 百度 / 头条，degraded + attempted + fallbackFrom；
//   3) 全部失败：isError + attempts 4 + retryable，不伪装空 items 成功；
//   4) fresh cache 300s、最小请求间隔 60s（retryAfterSec）、refresh 绕过 fresh 但仍限频；
//   5) stale cache 1800s 内降级返回（stale/warning），超龄报错；
//   6) partial（<10 条）、空列表切源、非 200/网络错误/超 1MB/坏 JSON 均切源；
//   7) schema、参数校验与并发调用（同实例不崩、后到者命中 fresh cache）。
#include "doctest.h"

#include "core/result.h"
#include "core/time.h"
#include "module_agent/hot_tool.h"
#include "platform/json.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace pj = platform::json;

using core::ToolArgs;
using core::ToolResult;
using module::agent::HotFetchRequest;
using module::agent::HotFetchResponse;
using module::agent::HotFetcher;
using module::agent::HotListTool;
using module::agent::NowFn;

constexpr char kWeiboUrl[] = "https://weibo.com/ajax/side/hotSearch";
constexpr char kWeiboBandUrl[] = "https://weibo.com/ajax/statuses/hot_band";
constexpr char kBaiduUrl[] =
    "https://top.baidu.com/api/board?platform=pc&tab=realtime";
constexpr char kToutiaoUrl[] =
    "https://www.toutiao.com/hot-event/hot-board/?origin=toutiao_pc";

constexpr char kWeiboUserAgent[] =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36";

struct FakeClock {
    int64_t now = 1700000000;

    NowFn fn() { return [this] { return now; }; }
};

struct FakeNet {
    std::map<std::string, HotFetchResponse> responses;
    std::vector<HotFetchRequest> requests;
    int calls = 0;

    HotFetcher fetcher() {
        return [this](const HotFetchRequest& request) {
            ++calls;
            requests.push_back(request);
            const auto found = responses.find(request.url);
            if (found == responses.end()) {
                return HotFetchResponse{0, {}, "no fixture for " + request.url};
            }
            return found->second;
        };
    }
};

bool parseOut(const ToolResult& result, pj::JsonValue& value) {
    auto parsed = pj::parse(result.contentJson);
    if (parsed.isFail() || !parsed.value().isObject()) return false;
    value = parsed.value();
    return true;
}

std::string expectedLocalTimestamp(int64_t epoch) {
    std::tm local{};
    core::time::localTime(static_cast<std::time_t>(epoch), &local);
    char buffer[32] = {};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local);
    return buffer;
}

// 微博两类响应共用的 item 数组；第 10 条故意缺 num，用于钉住 hot=null。
std::string weiboItems(int count) {
    std::string items;
    for (int i = 1; i <= count; ++i) {
        if (!items.empty()) items += ',';
        items += "{\"word\":\"微博第" + std::to_string(i) + "条\"";
        if (i != 10) {
            items += ",\"num\":" + std::to_string(3000000 - i * 1000);
        }
        items += ",\"realpos\":" + std::to_string(i) + "}";
    }
    return items;
}

std::string weiboBody(int count) {
    return "{\"ok\":1,\"data\":{\"realtime\":[" + weiboItems(count) + "]}}";
}

std::string weiboBandBody(int count) {
    return "{\"ok\":1,\"data\":{\"band_list\":[" + weiboItems(count) + "]}}";
}

std::string baiduBody(int count) {
    std::string items;
    for (int i = 1; i <= count; ++i) {
        if (!items.empty()) items += ',';
        items += "{\"word\":\"百度第" + std::to_string(i) +
                 "条\",\"hotScore\":\"" + std::to_string(500000 - i) +
                 "\",\"url\":\"https://www.baidu.com/s?wd=" +
                 std::to_string(i) + "\",\"index\":" +
                 std::to_string(i - 1) + "}";
    }
    // 前面塞一张非热榜卡片，验证工具依赖解析层的 component 选卡而非下标。
    return "{\"success\":true,\"data\":{\"cards\":["
           "{\"component\":\"topContent\",\"content\":[]},"
           "{\"component\":\"hotList\",\"content\":[" +
           items + "]}]}}";
}

std::string toutiaoBody(int count) {
    std::string items;
    for (int i = 1; i <= count; ++i) {
        if (!items.empty()) items += ',';
        items += "{\"Title\":\"头条第" + std::to_string(i) +
                 "条\",\"HotValue\":\"" + std::to_string(500000 - i) +
                 "\",\"Url\":\"https://www.toutiao.com/trending/" +
                 std::to_string(i) + "/\"}";
    }
    return "{\"status\":\"success\",\"data\":[" + items + "]}";
}

constexpr char kEmptyWeibo[] = R"({"ok":1,"data":{"realtime":[]}})";
constexpr char kEmptyBand[] = R"({"data":{"band_list":[]}})";
constexpr char kEmptyBaidu[] =
    R"({"success":true,"data":{"cards":[{"component":"hotList","content":[]}]}})";
constexpr char kEmptyToutiao[] = R"({"status":"success","data":[]})";

void setAllResponses(FakeNet& net, const HotFetchResponse& response) {
    net.responses[kWeiboUrl] = response;
    net.responses[kWeiboBandUrl] = response;
    net.responses[kBaiduUrl] = response;
    net.responses[kToutiaoUrl] = response;
}

} // namespace

REGISTER_TEST(hot_tool_weibo_success,
              "hot.list: 微博成功返回 top10、rank、hot null、本地时间与请求头", {
    FakeClock clock;
    FakeNet net;
    net.responses[kWeiboUrl] = HotFetchResponse{200, weiboBody(12), ""};

    HotListTool tool(net.fetcher(), clock.fn());
    auto result = tool.execute(ToolArgs{});
    DOCTEST_REQUIRE(result.isOk());
    DOCTEST_CHECK_FALSE(result.value().isError);
    DOCTEST_CHECK_EQ(net.calls, 1);

    // 请求形状：固定 URL、微博三头、8s 超时、无 Accept-Encoding。
    DOCTEST_REQUIRE_EQ(net.requests.size(), static_cast<std::size_t>(1));
    const HotFetchRequest& request = net.requests[0];
    DOCTEST_CHECK_EQ(request.url, std::string(kWeiboUrl));
    DOCTEST_CHECK_EQ(request.timeoutMs, 8000);
    DOCTEST_CHECK_EQ(request.headers.size(), static_cast<std::size_t>(3));
    const auto ua = request.headers.find("User-Agent");
    DOCTEST_REQUIRE(ua != request.headers.end());
    DOCTEST_CHECK_EQ(ua->second, std::string(kWeiboUserAgent));
    const auto referer = request.headers.find("Referer");
    DOCTEST_REQUIRE(referer != request.headers.end());
    DOCTEST_CHECK_EQ(referer->second, std::string("https://weibo.com/"));
    const auto accept = request.headers.find("Accept");
    DOCTEST_REQUIRE(accept != request.headers.end());
    DOCTEST_CHECK_EQ(accept->second,
                     std::string("application/json, text/plain, */*"));
    DOCTEST_CHECK_EQ(request.headers.count("Accept-Encoding"),
                     static_cast<std::size_t>(0));

    pj::JsonValue value;
    DOCTEST_REQUIRE(parseOut(result.value(), value));
    const pj::JsonObject out = value.asObject();
    DOCTEST_CHECK_EQ(out.get("source").asString(), std::string("weibo"));
    DOCTEST_CHECK_EQ(out.get("sourceLabel").asString(), std::string("微博热搜"));
    DOCTEST_CHECK_EQ(out.get("fetchedAtEpoch").asInt(), clock.now);
    DOCTEST_CHECK_EQ(out.get("fetchedAt").asString(),
                     expectedLocalTimestamp(clock.now));
    DOCTEST_CHECK_FALSE(out.get("cached").asBool());
    DOCTEST_CHECK_FALSE(out.get("partial").asBool());
    DOCTEST_CHECK_FALSE(out.get("degraded").asBool());
    DOCTEST_CHECK_FALSE(out.has("stale"));

    const pj::JsonValue attempted = out.get("attempted");
    DOCTEST_REQUIRE(attempted.isArray());
    DOCTEST_CHECK_EQ(attempted.size(), 1);
    DOCTEST_CHECK_EQ(attempted[0].asString(), std::string("weibo"));

    const pj::JsonValue items = out.get("items");
    DOCTEST_REQUIRE(items.isArray());
    DOCTEST_CHECK_EQ(items.size(), 10);  // 12 条有效项截取 top10
    for (int i = 0; i < 10; ++i) {
        const pj::JsonObject item = items[i].asObject();
        DOCTEST_CHECK_EQ(item.get("rank").asInt(), static_cast<int64_t>(i + 1));
        DOCTEST_CHECK_EQ(item.get("title").asString(),
                         std::string("微博第") + std::to_string(i + 1) + "条");
        DOCTEST_CHECK(item.get("url").asString().rfind("https://s.weibo.com/", 0) ==
                      0);
    }
    // 第 1 条有 num；第 10 条缺 num → 键存在且值为 null（固定形状，不省略键）。
    DOCTEST_CHECK_EQ(items[0].asObject().get("hot").asInt(),
                     static_cast<int64_t>(2999000));
    DOCTEST_CHECK(items[9].asObject().has("hot"));
    DOCTEST_CHECK(items[9].asObject().get("hot").isNull());
    return true;
})

REGISTER_TEST(hot_tool_falls_back_to_weibo_band,
              "hot.list: 微博 403 → hot_band 成功，degraded/attempted/fallbackFrom", {
    FakeClock clock;
    FakeNet net;
    net.responses[kWeiboUrl] = HotFetchResponse{403, "", ""};
    net.responses[kWeiboBandUrl] = HotFetchResponse{200, weiboBandBody(10), ""};

    HotListTool tool(net.fetcher(), clock.fn());
    auto result = tool.execute(ToolArgs{});
    DOCTEST_REQUIRE(result.isOk());
    DOCTEST_CHECK_FALSE(result.value().isError);
    DOCTEST_CHECK_EQ(net.calls, 2);
    DOCTEST_REQUIRE_EQ(net.requests.size(), static_cast<std::size_t>(2));
    DOCTEST_CHECK_EQ(net.requests[1].url, std::string(kWeiboBandUrl));
    DOCTEST_CHECK_EQ(net.requests[1].timeoutMs, 8000);
    const auto referer = net.requests[1].headers.find("Referer");
    DOCTEST_REQUIRE(referer != net.requests[1].headers.end());
    DOCTEST_CHECK_EQ(referer->second, std::string("https://weibo.com/"));

    pj::JsonValue value;
    DOCTEST_REQUIRE(parseOut(result.value(), value));
    const pj::JsonObject out = value.asObject();
    DOCTEST_CHECK_EQ(out.get("source").asString(), std::string("weibo_band"));
    DOCTEST_CHECK_EQ(out.get("sourceLabel").asString(), std::string("微博热榜"));
    DOCTEST_CHECK(out.get("degraded").asBool());
    DOCTEST_CHECK_EQ(out.get("fallbackFrom").asString(), std::string("weibo"));
    DOCTEST_CHECK_FALSE(out.get("partial").asBool());

    const pj::JsonValue attempted = out.get("attempted");
    DOCTEST_REQUIRE(attempted.isArray());
    DOCTEST_CHECK_EQ(attempted.size(), 2);
    DOCTEST_CHECK_EQ(attempted[0].asString(), std::string("weibo"));
    DOCTEST_CHECK_EQ(attempted[1].asString(), std::string("weibo_band"));
    return true;
})

REGISTER_TEST(hot_tool_falls_back_to_baidu,
              "hot.list: 两微博源失败 → 百度成功，degraded + 无特殊请求头", {
    FakeClock clock;
    FakeNet net;
    net.responses[kWeiboUrl] = HotFetchResponse{500, "", ""};
    net.responses[kWeiboBandUrl] = HotFetchResponse{0, {}, "timeout"};
    net.responses[kBaiduUrl] = HotFetchResponse{200, baiduBody(10), ""};

    HotListTool tool(net.fetcher(), clock.fn());
    auto result = tool.execute(ToolArgs{});
    DOCTEST_REQUIRE(result.isOk());
    DOCTEST_CHECK_FALSE(result.value().isError);
    DOCTEST_CHECK_EQ(net.calls, 3);
    DOCTEST_REQUIRE_EQ(net.requests.size(), static_cast<std::size_t>(3));
    DOCTEST_CHECK_EQ(net.requests[2].url, std::string(kBaiduUrl));
    DOCTEST_CHECK(net.requests[2].headers.empty());

    pj::JsonValue value;
    DOCTEST_REQUIRE(parseOut(result.value(), value));
    const pj::JsonObject out = value.asObject();
    DOCTEST_CHECK_EQ(out.get("source").asString(), std::string("baidu"));
    DOCTEST_CHECK_EQ(out.get("sourceLabel").asString(), std::string("百度热搜"));
    DOCTEST_CHECK(out.get("degraded").asBool());
    DOCTEST_CHECK_EQ(out.get("attempted").size(), 3);
    DOCTEST_CHECK_EQ(out.get("items").size(), 10);
    return true;
})

REGISTER_TEST(hot_tool_falls_back_to_toutiao,
              "hot.list: 前三源失败 → 今日头条成功，attempted 4 源", {
    FakeClock clock;
    FakeNet net;
    net.responses[kWeiboUrl] = HotFetchResponse{403, "", ""};
    net.responses[kWeiboBandUrl] = HotFetchResponse{0, {}, "connection reset"};
    net.responses[kBaiduUrl] = HotFetchResponse{500, "", ""};
    net.responses[kToutiaoUrl] = HotFetchResponse{200, toutiaoBody(10), ""};

    HotListTool tool(net.fetcher(), clock.fn());
    auto result = tool.execute(ToolArgs{});
    DOCTEST_REQUIRE(result.isOk());
    DOCTEST_CHECK_FALSE(result.value().isError);
    DOCTEST_CHECK_EQ(net.calls, 4);
    DOCTEST_CHECK(net.requests[3].headers.empty());

    pj::JsonValue value;
    DOCTEST_REQUIRE(parseOut(result.value(), value));
    const pj::JsonObject out = value.asObject();
    DOCTEST_CHECK_EQ(out.get("source").asString(), std::string("toutiao"));
    DOCTEST_CHECK_EQ(out.get("sourceLabel").asString(), std::string("今日头条"));
    DOCTEST_CHECK(out.get("degraded").asBool());
    DOCTEST_CHECK_EQ(out.get("attempted").size(), 4);
    DOCTEST_CHECK_EQ(out.get("attempted")[3].asString(), std::string("toutiao"));
    DOCTEST_CHECK_EQ(out.get("items").size(), 10);
    return true;
})

REGISTER_TEST(hot_tool_all_sources_fail_without_cache,
              "hot.list: 全部失败无缓存 → isError、attempts 4、各源原因与 retryable", {
    FakeClock clock;
    FakeNet net;
    net.responses[kWeiboUrl] = HotFetchResponse{403, "", ""};
    net.responses[kWeiboBandUrl] = HotFetchResponse{0, {}, "timeout"};
    net.responses[kBaiduUrl] =
        HotFetchResponse{200, std::string(1024 * 1024 + 1, 'x'), ""};
    net.responses[kToutiaoUrl] = HotFetchResponse{200, "not-json", ""};

    HotListTool tool(net.fetcher(), clock.fn());
    auto result = tool.execute(ToolArgs{});
    DOCTEST_REQUIRE(result.isOk());
    DOCTEST_CHECK(result.value().isError);
    DOCTEST_CHECK_EQ(net.calls, 4);

    pj::JsonValue value;
    DOCTEST_REQUIRE(parseOut(result.value(), value));
    const pj::JsonObject out = value.asObject();
    const std::string error = out.get("error").asString();
    DOCTEST_CHECK(error.rfind("热榜获取失败：", 0) == 0);
    DOCTEST_CHECK(error.find("微博热搜 HTTP 403") != std::string::npos);
    DOCTEST_CHECK(error.find("网络错误") != std::string::npos);
    DOCTEST_CHECK(error.find("响应过大") != std::string::npos);

    DOCTEST_CHECK_FALSE(out.has("items"));  // 不把失败伪装成空 items 成功
    DOCTEST_CHECK(out.get("retryable").asBool());

    const pj::JsonValue attempts = out.get("attempts");
    DOCTEST_REQUIRE(attempts.isArray());
    DOCTEST_CHECK_EQ(attempts.size(), 4);
    DOCTEST_CHECK_EQ(attempts[0].asObject().get("source").asString(),
                     std::string("weibo"));
    DOCTEST_CHECK_EQ(attempts[0].asObject().get("error").asString(),
                     std::string("HTTP 403"));
    DOCTEST_CHECK_EQ(attempts[1].asObject().get("source").asString(),
                     std::string("weibo_band"));
    DOCTEST_CHECK(attempts[1].asObject().get("error").asString().find("timeout") !=
                  std::string::npos);
    DOCTEST_CHECK_EQ(attempts[2].asObject().get("source").asString(),
                     std::string("baidu"));
    DOCTEST_CHECK(attempts[2].asObject().get("error").asString().find("响应过大") !=
                  std::string::npos);
    DOCTEST_CHECK_EQ(attempts[3].asObject().get("source").asString(),
                     std::string("toutiao"));
    DOCTEST_CHECK(attempts[3].asObject().get("error").asString().find("解析失败") !=
                  std::string::npos);
    return true;
})

REGISTER_TEST(hot_tool_fresh_cache_skips_network,
              "hot.list: 5 分钟 fresh cache 直接返回，fetcher 不增、cached=true", {
    FakeClock clock;
    FakeNet net;
    net.responses[kWeiboUrl] = HotFetchResponse{200, weiboBody(10), ""};

    HotListTool tool(net.fetcher(), clock.fn());
    auto first = tool.execute(ToolArgs{});
    DOCTEST_REQUIRE(first.isOk());
    DOCTEST_CHECK_FALSE(first.value().isError);
    DOCTEST_CHECK_EQ(net.calls, 1);
    const int64_t firstEpoch = clock.now;

    clock.now += 120;
    auto second = tool.execute(ToolArgs{});
    DOCTEST_REQUIRE(second.isOk());
    DOCTEST_CHECK_FALSE(second.value().isError);
    DOCTEST_CHECK_EQ(net.calls, 1);  // 不联网

    pj::JsonValue value;
    DOCTEST_REQUIRE(parseOut(second.value(), value));
    const pj::JsonObject out = value.asObject();
    DOCTEST_CHECK_EQ(out.get("source").asString(), std::string("weibo"));
    DOCTEST_CHECK(out.get("cached").asBool());
    DOCTEST_CHECK_EQ(out.get("fetchedAtEpoch").asInt(), firstEpoch);
    DOCTEST_CHECK_EQ(out.get("attempted").size(), 0);
    DOCTEST_CHECK_EQ(out.get("items").size(), 10);
    return true;
})

REGISTER_TEST(hot_tool_rate_limit_60_seconds,
              "hot.list: 60 秒最小请求间隔 → 有缓存 60s 内可返回，无缓存报错不联网", {
    FakeClock clock;
    FakeNet net;
    setAllResponses(net, HotFetchResponse{403, "", ""});

    HotListTool tool(net.fetcher(), clock.fn());
    auto first = tool.execute(ToolArgs{});
    DOCTEST_REQUIRE(first.isOk());
    DOCTEST_CHECK(first.value().isError);
    DOCTEST_CHECK_EQ(net.calls, 4);

    clock.now += 10;
    auto second = tool.execute(ToolArgs{});
    DOCTEST_REQUIRE(second.isOk());
    DOCTEST_CHECK(second.value().isError);
    DOCTEST_CHECK_EQ(net.calls, 4);  // 限频，不再尝试

    pj::JsonValue value;
    DOCTEST_REQUIRE(parseOut(second.value(), value));
    const pj::JsonObject out = value.asObject();
    DOCTEST_CHECK_EQ(out.get("error").asString(),
                     std::string("热榜请求过于频繁，请 60 秒后再试"));
    DOCTEST_CHECK_EQ(out.get("retryAfterSec").asInt(), static_cast<int64_t>(50));
    DOCTEST_CHECK_EQ(out.get("attempts").size(), 0);
    DOCTEST_CHECK(out.get("retryable").asBool());

    clock.now += 60;  // 距首次尝试 70s
    auto third = tool.execute(ToolArgs{});
    DOCTEST_REQUIRE(third.isOk());
    DOCTEST_CHECK(third.value().isError);
    DOCTEST_CHECK_EQ(net.calls, 8);  // 允许重新尝试整条源链
    return true;
})

REGISTER_TEST(hot_tool_stale_cache_within_30_minutes,
              "hot.list: 全源失败 → 30 分钟内 stale 缓存降级返回，超龄报错", {
    FakeClock clock;
    FakeNet net;
    net.responses[kWeiboUrl] = HotFetchResponse{200, weiboBody(10), ""};

    HotListTool tool(net.fetcher(), clock.fn());
    auto first = tool.execute(ToolArgs{});
    DOCTEST_REQUIRE(first.isOk());
    DOCTEST_CHECK_FALSE(first.value().isError);
    const int64_t cacheEpoch = clock.now;

    clock.now += 600;  // 10 分钟：fresh 已过，stale 仍在窗口内
    setAllResponses(net, HotFetchResponse{403, "", ""});

    auto second = tool.execute(ToolArgs{});
    DOCTEST_REQUIRE(second.isOk());
    DOCTEST_CHECK_FALSE(second.value().isError);
    DOCTEST_CHECK_EQ(net.calls, 5);  // 1 次成功 + 4 次失败尝试

    pj::JsonValue value;
    DOCTEST_REQUIRE(parseOut(second.value(), value));
    const pj::JsonObject out = value.asObject();
    DOCTEST_CHECK_EQ(out.get("source").asString(), std::string("weibo"));
    DOCTEST_CHECK_EQ(out.get("fetchedAtEpoch").asInt(), cacheEpoch);
    DOCTEST_CHECK(out.get("cached").asBool());
    DOCTEST_CHECK(out.get("stale").asBool());
    DOCTEST_CHECK_EQ(out.get("items").size(), 10);
    DOCTEST_CHECK_EQ(out.get("attempted").size(), 4);
    const std::string warning = out.get("warning").asString();
    DOCTEST_CHECK(warning.find("网络失败") != std::string::npos);
    DOCTEST_CHECK(warning.find("10 分钟前缓存") != std::string::npos);

    clock.now += 1400;  // 距抓取 2000s > 1800s
    auto third = tool.execute(ToolArgs{});
    DOCTEST_REQUIRE(third.isOk());
    DOCTEST_CHECK(third.value().isError);
    DOCTEST_CHECK_EQ(net.calls, 9);
    DOCTEST_REQUIRE(parseOut(third.value(), value));
    DOCTEST_CHECK_EQ(value.asObject().get("attempts").size(), 4);
    return true;
})

REGISTER_TEST(hot_tool_refresh_bypasses_cache_but_respects_rate_limit,
              "hot.list: refresh=true 绕过 fresh cache，但 60s 内仍报限频", {
    FakeClock clock;
    FakeNet net;
    net.responses[kWeiboUrl] = HotFetchResponse{200, weiboBody(10), ""};

    HotListTool tool(net.fetcher(), clock.fn());
    auto first = tool.execute(ToolArgs{});
    DOCTEST_REQUIRE(first.isOk());
    DOCTEST_CHECK_FALSE(first.value().isError);
    DOCTEST_CHECK_EQ(net.calls, 1);

    clock.now += 10;
    auto limited = tool.execute(ToolArgs{R"({"refresh":true})"});
    DOCTEST_REQUIRE(limited.isOk());
    DOCTEST_CHECK(limited.value().isError);
    DOCTEST_CHECK_EQ(net.calls, 1);

    clock.now += 110;  // 距首次尝试 120s：限频已过，但 cache 仍 fresh
    auto refreshed = tool.execute(ToolArgs{R"({"refresh":true})"});
    DOCTEST_REQUIRE(refreshed.isOk());
    DOCTEST_CHECK_FALSE(refreshed.value().isError);
    DOCTEST_CHECK_EQ(net.calls, 2);

    pj::JsonValue value;
    DOCTEST_REQUIRE(parseOut(refreshed.value(), value));
    const pj::JsonObject out = value.asObject();
    DOCTEST_CHECK_FALSE(out.get("cached").asBool());
    DOCTEST_CHECK_EQ(out.get("items").size(), 10);
    return true;
})

REGISTER_TEST(hot_tool_partial_when_fewer_than_ten,
              "hot.list: 成功源只有 5 条 → partial=true、items 5", {
    FakeClock clock;
    FakeNet net;
    net.responses[kWeiboUrl] = HotFetchResponse{200, weiboBody(5), ""};

    HotListTool tool(net.fetcher(), clock.fn());
    auto result = tool.execute(ToolArgs{});
    DOCTEST_REQUIRE(result.isOk());
    DOCTEST_CHECK_FALSE(result.value().isError);

    pj::JsonValue value;
    DOCTEST_REQUIRE(parseOut(result.value(), value));
    const pj::JsonObject out = value.asObject();
    DOCTEST_CHECK(out.get("partial").asBool());
    DOCTEST_CHECK_FALSE(out.get("degraded").asBool());
    DOCTEST_CHECK_EQ(out.get("items").size(), 5);
    DOCTEST_CHECK_EQ(out.get("items")[4].asObject().get("rank").asInt(),
                     static_cast<int64_t>(5));
    return true;
})

REGISTER_TEST(hot_tool_empty_list_falls_through_to_next_source,
              "hot.list: 源返回空列表 → 切下一源；全空 → 工具错误", {
    FakeClock clock;
    FakeNet net;
    net.responses[kWeiboUrl] = HotFetchResponse{200, kEmptyWeibo, ""};
    net.responses[kWeiboBandUrl] = HotFetchResponse{200, weiboBandBody(10), ""};

    HotListTool tool(net.fetcher(), clock.fn());
    auto result = tool.execute(ToolArgs{});
    DOCTEST_REQUIRE(result.isOk());
    DOCTEST_CHECK_FALSE(result.value().isError);
    DOCTEST_CHECK_EQ(net.calls, 2);

    pj::JsonValue value;
    DOCTEST_REQUIRE(parseOut(result.value(), value));
    pj::JsonObject out = value.asObject();
    DOCTEST_CHECK_EQ(out.get("source").asString(), std::string("weibo_band"));
    DOCTEST_CHECK_EQ(out.get("attempted").size(), 2);

    // 新实例：四源都返回结构正确但空列表 → 不能成功返回空榜。
    FakeClock emptyClock;
    FakeNet emptyNet;
    emptyNet.responses[kWeiboUrl] = HotFetchResponse{200, kEmptyWeibo, ""};
    emptyNet.responses[kWeiboBandUrl] = HotFetchResponse{200, kEmptyBand, ""};
    emptyNet.responses[kBaiduUrl] = HotFetchResponse{200, kEmptyBaidu, ""};
    emptyNet.responses[kToutiaoUrl] = HotFetchResponse{200, kEmptyToutiao, ""};

    HotListTool emptyTool(emptyNet.fetcher(), emptyClock.fn());
    auto failed = emptyTool.execute(ToolArgs{});
    DOCTEST_REQUIRE(failed.isOk());
    DOCTEST_CHECK(failed.value().isError);
    DOCTEST_CHECK_EQ(emptyNet.calls, 4);
    DOCTEST_REQUIRE(parseOut(failed.value(), value));
    out = value.asObject();
    DOCTEST_CHECK(out.get("error").asString().find("空列表") != std::string::npos);
    DOCTEST_CHECK_EQ(out.get("attempts").size(), 4);
    return true;
})

REGISTER_TEST(hot_tool_rejects_bad_refresh_argument,
              "hot.list: refresh 非布尔 → isError 且不联网", {
    FakeClock clock;
    FakeNet net;
    net.responses[kWeiboUrl] = HotFetchResponse{200, weiboBody(10), ""};

    HotListTool tool(net.fetcher(), clock.fn());
    auto result = tool.execute(ToolArgs{R"({"refresh":"yes"})"});
    DOCTEST_REQUIRE(result.isOk());
    DOCTEST_CHECK(result.value().isError);
    DOCTEST_CHECK_EQ(net.calls, 0);

    pj::JsonValue value;
    DOCTEST_REQUIRE(parseOut(result.value(), value));
    DOCTEST_CHECK(value.asObject().get("error").asString().find("refresh") !=
                  std::string::npos);
    return true;
})

REGISTER_TEST(hot_tool_schema_and_name,
              "hot.list: schema 名称/描述/refresh/additionalProperties 契约", {
    HotListTool tool;  // 默认依赖不会被调用
    DOCTEST_CHECK_EQ(tool.name(), std::string("hot.list"));

    auto parsed = pj::parse(tool.schemaJson());
    DOCTEST_REQUIRE(parsed.isOk());
    const pj::JsonObject schema = parsed.value().asObject();
    DOCTEST_CHECK_EQ(schema.get("name").asString(), std::string("hot.list"));
    DOCTEST_CHECK(schema.get("description").asString().find("热榜") !=
                  std::string::npos);

    const pj::JsonObject input = schema.get("input_schema").asObject();
    DOCTEST_CHECK_EQ(input.get("type").asString(), std::string("object"));
    DOCTEST_CHECK_FALSE(input.get("additionalProperties").asBool());
    DOCTEST_CHECK_FALSE(input.has("required"));
    const pj::JsonObject properties = input.get("properties").asObject();
    DOCTEST_REQUIRE(properties.has("refresh"));
    DOCTEST_CHECK_EQ(properties.get("refresh").asObject().get("type").asString(),
                     std::string("boolean"));
    return true;
})

REGISTER_TEST(hot_tool_concurrent_calls_are_serialized,
              "hot.list: 同实例并发调用不崩，后到者命中 fresh cache", {
    FakeClock clock;
    std::atomic<int> calls{0};
    std::atomic<int> successes{0};
    std::atomic<int> failures{0};

    HotFetcher fetcher = [&calls](const HotFetchRequest&) {
        ++calls;
        return HotFetchResponse{200, weiboBody(10), ""};
    };
    HotListTool tool(fetcher, clock.fn());

    const auto run = [&tool, &successes, &failures] {
        auto result = tool.execute(ToolArgs{});
        if (result.isOk() && !result.value().isError) {
            ++successes;
        } else {
            ++failures;
        }
    };

    std::thread first(run);
    std::thread second(run);
    first.join();
    second.join();

    DOCTEST_CHECK_EQ(successes.load(), 2);
    DOCTEST_CHECK_EQ(failures.load(), 0);
    DOCTEST_CHECK_EQ(calls.load(), 1);  // 另一线程在锁后看到 fresh cache
    return true;
})
