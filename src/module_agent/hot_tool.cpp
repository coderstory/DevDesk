// hot.list 实现：参数校验 → 缓存/限频闸 → 四源串行降级 → 成功/降级/stale/失败 JSON。
// 所有失败都是「Result ok + ToolResult::isError=true」，Result::fail 只留给内部序列化
// 不变量破坏（与 app.notify / todo.* 同口径）。
#include "module_agent/hot_tool.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/time.h"
#include "platform/http_client.h"
#include "platform/json.h"

namespace module::agent {

namespace {

namespace pj = platform::json;

using core::Result;
using core::ToolArgs;
using core::ToolResult;

constexpr std::size_t kTopN = 10;
constexpr std::size_t kMaxBodyBytes = 1024 * 1024;  // 1MB
constexpr int64_t kFreshCacheSeconds = 300;         // 5 分钟
constexpr int64_t kMinRequestIntervalSeconds = 60;  // 最小请求间隔
constexpr int64_t kStaleCacheSeconds = 1800;        // 30 分钟内可降级用旧缓存
constexpr int64_t kTotalBudgetSeconds = 20;         // 一次调用跨源总预算
constexpr int64_t kPerSourceTimeoutMs = 8000;       // 单源上限

constexpr const char* kWeiboUserAgent =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36";
constexpr const char* kWeiboReferer = "https://weibo.com/";
constexpr const char* kWeiboAccept = "application/json, text/plain, */*";

// 链序 = 优先级。微博两个接口共用 Referer/UA/Accept；百度/头条不带特殊头。
struct SourceSpec {
    HotSource source;
    const char* id;
    const char* label;
    const char* url;
    bool weiboHeaders;
};

constexpr SourceSpec kChain[] = {
    {HotSource::Weibo, "weibo", "微博热搜",
     "https://weibo.com/ajax/side/hotSearch", true},
    {HotSource::WeiboBand, "weibo_band", "微博热榜",
     "https://weibo.com/ajax/statuses/hot_band", true},
    {HotSource::Baidu, "baidu", "百度热搜",
     "https://top.baidu.com/api/board?platform=pc&tab=realtime", false},
    {HotSource::Toutiao, "toutiao", "今日头条",
     "https://www.toutiao.com/hot-event/hot-board/?origin=toutiao_pc", false},
};

// Attempt 只活在 execute 的栈上，不进出头文件。
struct Attempt {
    std::string source;
    std::string label;
    std::string error;
};

struct ParsedFetch {
    std::vector<HotItem> items;  // 已截取 top10
};

const SourceSpec& specFor(HotSource source) {
    switch (source) {
        case HotSource::Weibo: return kChain[0];
        case HotSource::WeiboBand: return kChain[1];
        case HotSource::Baidu: return kChain[2];
        case HotSource::Toutiao: return kChain[3];
    }
    return kChain[0];
}

HotFetchRequest makeRequest(const SourceSpec& spec, int timeoutMs) {
    HotFetchRequest request;
    request.url = spec.url;
    request.timeoutMs = timeoutMs;
    if (spec.weiboHeaders) {
        request.headers["User-Agent"] = kWeiboUserAgent;
        request.headers["Referer"] = kWeiboReferer;
        request.headers["Accept"] = kWeiboAccept;
        // 刻意不设 Accept-Encoding：客户端不解压，服务端压缩会让 body 变成二进制。
    }
    return request;
}

// epoch 秒 → 本地「YYYY-MM-DD HH:MM:SS」。跨平台时区转换走 core::time::localTime。
std::string formatLocalTime(int64_t epochSeconds) {
    std::tm local{};
    core::time::localTime(static_cast<std::time_t>(epochSeconds), &local);
    char buffer[32] = {};
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local) == 0) {
        return {};
    }
    return buffer;
}

Result<std::string> quoteJsonString(const std::string& text) {
    return pj::stringify(pj::makeString(text));
}

Result<pj::JsonValue> parseArrayText(const std::string& text) {
    auto parsed = pj::parse(text);
    if (parsed.isFail() || !parsed.value().isArray()) {
        return Result<pj::JsonValue>::fail(
            core::ErrorCode::Internal,
            "热榜结果数组序列化失败: " +
                (parsed.isFail() ? parsed.errorMessage() : std::string("根节点不是数组")));
    }
    return parsed;
}

// JSON 数组没有公共 push 接口：用 stringify 逐项转义后拼文本，再 parse 回 JsonValue。
Result<std::string> itemsArrayText(const std::vector<HotItem>& items) {
    std::string text = "[";
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0) text += ',';
        auto title = quoteJsonString(items[i].title);
        if (title.isFail()) return title;
        auto url = quoteJsonString(items[i].url);
        if (url.isFail()) return url;

        text += "{\"rank\":";
        text += std::to_string(items[i].rank);
        text += ",\"title\":";
        text += title.value();
        text += ",\"hot\":";
        if (items[i].hot.has_value()) {
            text += std::to_string(*items[i].hot);
        } else {
            text += "null";  // hot 缺失固定输出 null，测试钉住该形状
        }
        text += ",\"url\":";
        text += url.value();
        text += '}';
    }
    text += ']';
    return Result<std::string>::ok(std::move(text));
}

Result<std::string> attemptsArrayText(const std::vector<Attempt>& attempts) {
    std::string text = "[";
    for (std::size_t i = 0; i < attempts.size(); ++i) {
        if (i > 0) text += ',';
        auto source = quoteJsonString(attempts[i].source);
        if (source.isFail()) return source;
        auto error = quoteJsonString(attempts[i].error);
        if (error.isFail()) return error;
        text += "{\"source\":";
        text += source.value();
        text += ",\"error\":";
        text += error.value();
        text += '}';
    }
    text += ']';
    return Result<std::string>::ok(std::move(text));
}

Result<ToolResult> finishResult(pj::JsonObject object, bool isError,
                                const char* what) {
    auto text = pj::stringify(pj::JsonValue(std::move(object)));
    if (text.isFail()) {
        return Result<ToolResult>::fail(
            core::ErrorCode::Internal,
            std::string(what) + "序列化失败: " + text.errorMessage());
    }
    return Result<ToolResult>::ok(ToolResult{isError, text.value()});
}

Result<ToolResult> successResult(const SourceSpec& spec, int64_t fetchedAtEpoch,
                                 const std::vector<HotItem>& items, bool degraded,
                                 bool cached,
                                 const std::vector<std::string>& attempted,
                                 bool stale, int64_t ageSeconds) {
    auto itemsText = itemsArrayText(items);
    if (itemsText.isFail()) {
        return Result<ToolResult>::fail(itemsText.errorCode(),
                                        itemsText.errorMessage());
    }
    auto itemsValue = parseArrayText(itemsText.value());
    if (itemsValue.isFail()) {
        return Result<ToolResult>::fail(itemsValue.errorCode(),
                                        itemsValue.errorMessage());
    }

    pj::JsonObject out;
    out.set("source", pj::makeString(spec.id));
    out.set("sourceLabel", pj::makeString(spec.label));
    out.set("fetchedAt", pj::makeString(formatLocalTime(fetchedAtEpoch)));
    out.set("fetchedAtEpoch", pj::makeInt(fetchedAtEpoch));
    out.set("items", itemsValue.value());
    out.set("partial", pj::makeBool(items.size() < kTopN));
    out.set("degraded", pj::makeBool(degraded));
    out.set("attempted", pj::makeStringArray(attempted));
    out.set("cached", pj::makeBool(cached));
    if (degraded) {
        out.set("fallbackFrom", pj::makeString(kChain[0].id));
    }
    if (stale) {
        int64_t minutes = ageSeconds / 60;
        if (minutes < 1) minutes = 1;
        out.set("stale", pj::makeBool(true));
        out.set("warning", pj::makeString("网络失败，返回 " +
                                          std::to_string(minutes) +
                                          " 分钟前缓存"));
    }
    return finishResult(std::move(out), false, "热榜结果");
}

Result<ToolResult> failureResult(const std::string& message,
                                 const std::vector<Attempt>& attempts,
                                 std::optional<int64_t> retryAfterSec) {
    auto attemptsText = attemptsArrayText(attempts);
    if (attemptsText.isFail()) {
        return Result<ToolResult>::fail(attemptsText.errorCode(),
                                        attemptsText.errorMessage());
    }
    auto attemptsValue = parseArrayText(attemptsText.value());
    if (attemptsValue.isFail()) {
        return Result<ToolResult>::fail(attemptsValue.errorCode(),
                                        attemptsValue.errorMessage());
    }

    pj::JsonObject out;
    out.set("error", pj::makeString(message));
    out.set("attempts", attemptsValue.value());
    out.set("retryable", pj::makeBool(true));
    if (retryAfterSec.has_value()) {
        out.set("retryAfterSec", pj::makeInt(*retryAfterSec));
    }
    return finishResult(std::move(out), true, "热榜错误结果");
}

// 参数：空串视为 {}（与 todo/agent 其它工具同口径）；其余必须解析成 object。
Result<pj::JsonObject> parseArgs(const ToolArgs& args) {
    if (args.json.empty()) {
        return Result<pj::JsonObject>::ok(pj::JsonObject());
    }
    auto parsed = pj::parse(args.json);
    if (parsed.isFail() || !parsed.value().isObject()) {
        return Result<pj::JsonObject>::fail(core::ErrorCode::ValidationError,
                                            "参数不是合法 JSON 对象");
    }
    return Result<pj::JsonObject>::ok(parsed.value().asObject());
}

// 单源响应分类：网络错误 / 非 200 / 超 1MB / 结构错误 / 空列表都算失败，
// 让调用方记录 attempts 并切下一源；5xx 不做同源重试（v1）。
bool decodeResponse(const SourceSpec& spec, const HotFetchResponse& response,
                    ParsedFetch& out, std::string& error) {
    if (!response.networkError.empty()) {
        error = "网络错误：" + response.networkError;
        return false;
    }
    if (response.statusCode != 200) {
        error = "HTTP " + std::to_string(response.statusCode);
        return false;
    }
    if (response.body.size() > kMaxBodyBytes) {
        error = "响应过大（" + std::to_string(response.body.size()) +
                " 字节，上限 1MB）";
        return false;
    }

    auto parsed = parseHotList(spec.source, response.body);
    if (parsed.isFail()) {
        error = "解析失败：" + parsed.errorMessage();
        return false;
    }
    if (parsed.value().empty()) {
        error = "空列表";
        return false;
    }

    out.items = takeTopN(std::move(parsed.value()), kTopN);
    return true;
}

} // namespace

HotFetchResponse defaultHotFetcher(const HotFetchRequest& request) {
    platform::http::Request httpRequest;
    httpRequest.method = "GET";
    httpRequest.url = request.url;
    httpRequest.headers = request.headers;
    httpRequest.timeoutMs = static_cast<long>(request.timeoutMs);

    auto sent = platform::http::send(httpRequest);
    HotFetchResponse response;
    if (sent.isFail()) {
        response.networkError = sent.errorMessage();
        return response;
    }
    const platform::http::Response& httpResponse = sent.value();
    response.statusCode = httpResponse.statusCode;
    response.body = httpResponse.body;
    if (httpResponse.networkError) {
        response.networkError = httpResponse.errorMessage.empty()
                                    ? "网络请求失败"
                                    : httpResponse.errorMessage;
    }
    return response;
}

int64_t defaultNow() {
    return static_cast<int64_t>(core::time::nowUnix());
}

HotListTool::HotListTool(HotFetcher fetcher, NowFn now)
    : fetcher_(std::move(fetcher)), now_(std::move(now)) {}

std::string HotListTool::name() const { return "hot.list"; }

std::string HotListTool::schemaJson() const {
    return R"({"name":"hot.list","description":"手动查询今日热榜 top10（微博/百度/头条自动降级），返回标题、热度、链接、来源与抓取时间。","input_schema":{"type":"object","properties":{"refresh":{"type":"boolean","description":"默认 false；true 时忽略 5 分钟 fresh 缓存，但仍受 60 秒最小请求间隔限制"}},"additionalProperties":false}})";
}

Result<ToolResult> HotListTool::execute(const ToolArgs& args) {
    auto parsedArgs = parseArgs(args);
    if (parsedArgs.isFail()) {
        return failureResult(parsedArgs.errorMessage(), {}, std::nullopt);
    }
    const pj::JsonObject& input = parsedArgs.value();

    bool refresh = false;
    if (input.has("refresh")) {
        const pj::JsonValue value = input.get("refresh");
        if (!value.isBool()) {
            return failureResult("参数 refresh 必须是布尔值", {}, std::nullopt);
        }
        refresh = value.asBool();
    }

    // 单锁覆盖缓存检查、限频检查与整条源链：同实例并发调用串行化。
    std::lock_guard<std::mutex> lock(mutex_);
    const int64_t now = now_();

    // fresh cache 直接返回，不联网（refresh 显式跳过）。
    if (!refresh && cache_.has_value() &&
        now - cache_->fetchedAtEpoch <= kFreshCacheSeconds) {
        return successResult(specFor(cache_->source), cache_->fetchedAtEpoch,
                             cache_->items, cache_->degraded,
                             /*cached=*/true, {}, /*stale=*/false, 0);
    }

    // 最小请求间隔只闸「是否开始新一轮联网」；一次调用内部的多源尝试不重复计数。
    if (lastAttemptAt_.has_value() &&
        now - *lastAttemptAt_ < kMinRequestIntervalSeconds) {
        int64_t retryAfterSec =
            kMinRequestIntervalSeconds - (now - *lastAttemptAt_);
        if (retryAfterSec < 1) retryAfterSec = 1;
        return failureResult("热榜请求过于频繁，请 60 秒后再试", {},
                             retryAfterSec);
    }

    lastAttemptAt_ = now;
    const int64_t start = now;
    std::vector<Attempt> attempts;
    std::vector<std::string> attempted;

    for (const SourceSpec& spec : kChain) {
        const int64_t elapsed = now_() - start;
        const int64_t remainingSec = kTotalBudgetSeconds - elapsed;
        if (remainingSec <= 0) break;  // 总预算耗尽，停止切源

        const int64_t timeoutMs =
            std::min<int64_t>(kPerSourceTimeoutMs, remainingSec * 1000);
        attempted.push_back(spec.id);

        Attempt attempt;
        attempt.source = spec.id;
        attempt.label = spec.label;

        if (!fetcher_) {
            attempt.error = "网络错误：未配置抓取器";
            attempts.push_back(std::move(attempt));
            continue;
        }

        const HotFetchResponse response =
            fetcher_(makeRequest(spec, static_cast<int>(timeoutMs)));

        ParsedFetch fetched;
        std::string error;
        if (!decodeResponse(spec, response, fetched, error)) {
            attempt.error = std::move(error);
            attempts.push_back(std::move(attempt));
            continue;
        }

        // 首个成功即返回：不合并多源。成功时间单独取一次时钟，供缓存年龄计算。
        const int64_t fetchedAt = now_();
        const bool degraded = attempted.size() > 1;

        CacheEntry entry;
        entry.source = spec.source;
        entry.fetchedAtEpoch = fetchedAt;
        entry.items = std::move(fetched.items);
        entry.degraded = degraded;
        cache_ = std::move(entry);

        return successResult(spec, fetchedAt, cache_->items, degraded,
                             /*cached=*/false, attempted, /*stale=*/false, 0);
    }

    // 全部源失败：30 分钟内的 stale 缓存可降级返回，否则工具错误。
    if (cache_.has_value()) {
        int64_t ageSeconds = now_() - cache_->fetchedAtEpoch;
        if (ageSeconds < 0) ageSeconds = 0;
        if (ageSeconds <= kStaleCacheSeconds) {
            return successResult(specFor(cache_->source), cache_->fetchedAtEpoch,
                                 cache_->items, cache_->degraded,
                                 /*cached=*/true, attempted, /*stale=*/true,
                                 ageSeconds);
        }
    }

    std::string message = "热榜获取失败：";
    if (attempts.empty()) {
        message += "总预算耗尽（20 秒）";
    } else {
        for (std::size_t i = 0; i < attempts.size(); ++i) {
            if (i > 0) message += "，";
            message += attempts[i].label + " " + attempts[i].error;
        }
    }
    return failureResult(message, attempts, std::nullopt);
}

} // namespace module::agent
