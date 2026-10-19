// DirectLlmClient 实现：取 profile → 解析槽位 → 组 Anthropic 请求体 → 注入鉴权头
// → 发送 → 检查状态码 → 解析响应。不抛异常全程 Result。
// messages 数组逐元素 stringify 拼接后 parse 自检，拼装出错降级成 send 失败，不 abort。
#include "module_agent/direct_llm_client.h"

#include <cctype>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "model_slots.h"
#include "platform/json.h"

namespace module::agent {
namespace {

constexpr int64_t kLlmMaxTokens = 4096;
// 上游直连总超时：与代理转发同量级（proxy_router 的 up.timeoutMs），大上下文长生成不早断
constexpr long kLlmTimeoutMs = 120000;
constexpr const char* kAnthropicVersion = "2023-06-01";
// 代理对外 UA（上游方言契约，见 module_claude_proxy 的 upstream_dialect.h）：
// opencode.ai 前置 Cloudflare 按 UA 指纹拦请求，统一用 Claude Code 签名
constexpr const char* kClaudeCliUserAgent = "claude-cli/2.1.0 (external, cli)";
// 配置类错误统一前缀：agent 页据此渲染「去 Claude 页」引导（needsClaudePageHint 钉死本前缀）
constexpr const char* kConfigErrorPrefix = "LLM 配置不可用：";

// 槽位键全集（真源 = src/model_slots.h 的 X 宏表，新增槽位只改那里）
#define DEVDESK_MODEL_SLOT_JSON_KEY(name, jsonKey, label, formLabel, envKey, nameKey, oneM) \
    jsonKey,
constexpr const char* kSlotKeys[] = {
    DEVDESK_MODEL_SLOT_LIST(DEVDESK_MODEL_SLOT_JSON_KEY)};
#undef DEVDESK_MODEL_SLOT_JSON_KEY
constexpr size_t kSlotCount = sizeof(kSlotKeys) / sizeof(kSlotKeys[0]);

// 把已 stringify 的元素文本拼成 JSON 数组原文并 parse 自检，绝不把坏 body 发给上游
core::Result<platform::json::JsonValue> parseJoinedArray(
    const std::vector<std::string>& elementTexts, const char* what) {
    std::string text = "[";
    for (size_t i = 0; i < elementTexts.size(); ++i) {
        if (i > 0) text += ",";
        text += elementTexts[i];
    }
    text += "]";
    auto v = platform::json::parse(text);
    if (v.isFail() || !v.value().isArray()) {
        return core::Result<platform::json::JsonValue>::fail(
            core::ErrorCode::JsonParseError,
            std::string("组装 ") + what + " 数组失败（内部构造错误）："
                + (v.isFail() ? v.errorMessage() : "结果不是数组"));
    }
    return core::Result<platform::json::JsonValue>::ok(std::move(v.value()));
}

// usage 整数字段；缺/非数字 → -1（未采集）。asInt 只认 Int 形，网关回小数会取成 0（对 <=0 不累计）
int64_t usageInt(const platform::json::JsonObject& usage, const char* key) {
    platform::json::JsonValue v = usage.get(key);
    return v.isNumber() ? v.asInt() : -1;
}

std::string stripTrailingSlash(const std::string& s) {
    if (!s.empty() && s.back() == '/') return s.substr(0, s.size() - 1);
    return s;
}

std::string toLowerAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// 槽位键名 → 实际 model id。语义与代理侧（module_claude_proxy 的 claude_service）
// 曾用的 resolveModelSlot 同口径："def"/"default" 等价；default 槽在 models_config
// 缺该槽时回落 profile.model；其余槽位未配置 / 未知槽位键 / models_config 非法 → nullopt
std::optional<std::string> resolveSlotModel(const core::LlmDirectProfile& profile,
                                            const std::string& slot) {
    const std::string key = (slot == "def") ? "default" : slot;
    bool known = false;
    for (size_t i = 0; i < kSlotCount; ++i) {
        if (key == kSlotKeys[i]) {
            known = true;
            break;
        }
    }
    if (!known) return std::nullopt;

    std::string model;
    if (!profile.modelsConfigJson.empty()) {
        auto parsed = platform::json::parse(profile.modelsConfigJson);
        if (parsed.isOk() && parsed.value().isObject()) {
            platform::json::JsonValue slotVal = parsed.value().asObject().get(key);
            if (slotVal.isObject()) {
                platform::json::JsonValue modelVal = slotVal.asObject().get("model");
                if (modelVal.isString()) model = modelVal.asString();
            }
        }
    }
    if (!model.empty()) return model;
    if (key == "default" && !profile.defaultModel.empty()) {
        return profile.defaultModel;
    }
    return std::nullopt;
}

// URL host（小写，去 scheme / userinfo / port / path）；解析不出返回空串
std::string hostOf(const std::string& url) {
    size_t begin = 0;
    const size_t scheme = url.find("://");
    if (scheme != std::string::npos) begin = scheme + 3;
    size_t end = url.find_first_of("/?#", begin);
    if (end == std::string::npos) end = url.size();

    std::string authority = url.substr(begin, end - begin);
    const size_t at = authority.find('@');
    if (at != std::string::npos) authority.erase(0, at + 1);
    const size_t colon = authority.find(':');
    if (colon != std::string::npos) authority.erase(colon);
    return toLowerAscii(authority);
}

// opencode.ai 本身或其子域（zen.opencode.ai）；notopencode.ai 不算
bool isOpencodeHost(const std::string& host) {
    const std::string suffix = "opencode.ai";
    if (host == suffix) return true;
    if (host.size() <= suffix.size() + 1) return false;
    return host.compare(host.size() - suffix.size() - 1, suffix.size() + 1,
                        "." + suffix) == 0;
}

// 上游方言：当前只有 opencode zen 一家有差异（要 x-opencode-session + 不认 [1M] 模型标记）。
// 方言判定的唯一真相源是 module_claude_proxy 的 upstream_dialect.h（模块隔离下不可
// include，此处按同一规则最小复刻）；新增方言时两处都要改，漏改表现为「该网关下
// agent 直连失败」。
bool isOpencodeZen(const core::LlmDirectProfile& profile) {
    if (profile.dialectOverride == "opencode_zen") return true;
    if (profile.dialectOverride == "anthropic_default") return false;
    return isOpencodeHost(hostOf(profile.baseUrl));
}

// 模型名剥尾部 [1M]（大小写不敏感、容忍标记前后空白）；未命中 → 原样返回
std::string stripOneMSuffix(const std::string& model) {
    constexpr const char* kMarker = "[1M]";
    constexpr size_t kMarkerLen = 4;
    const auto trimEnd = [](const std::string& s) {
        size_t e = s.size();
        while (e > 0 && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
        return s.substr(0, e);
    };
    const std::string trimmed = trimEnd(model);
    if (trimmed.size() < kMarkerLen) return model;
    const size_t pos = trimmed.size() - kMarkerLen;
    for (size_t k = 0; k < kMarkerLen; ++k) {
        if (std::toupper(static_cast<unsigned char>(trimmed[pos + k])) !=
            static_cast<unsigned char>(kMarker[k])) {
            return model;
        }
    }
    return trimEnd(trimmed.substr(0, pos));
}

// opencode 网关要求 x-opencode-session（缺失回 400 MissingSessionID）；
// 进程内稳定一份，保证上游路由 / 缓存亲和
const std::string& upstreamSessionId() {
    static const std::string id = [] {
        std::random_device rd;
        std::uint32_t words[4];
        for (auto& w : words) w = rd();
        std::ostringstream oss;
        for (const auto w : words) {
            oss << std::hex << std::setw(8) << std::setfill('0') << w;
        }
        return oss.str();
    }();
    return id;
}

core::Result<core::LlmResponse> configFail(const std::string& detail) {
    return core::Result<core::LlmResponse>::fail(
        core::ErrorCode::ValidationError, std::string(kConfigErrorPrefix) + detail);
}

} // namespace

DirectLlmClient::DirectLlmClient(core::LlmProfileSource& profiles, HttpFn http)
    : profiles_(profiles), http_(std::move(http)) {}

core::Result<core::LlmResponse> DirectLlmClient::send(
    const std::vector<core::LlmMessage>& messages,
    const std::string& toolsJson,
    const std::string& systemPrompt,
    const std::string& model) {
    // 1. 配置来源：激活 profile（代理是否在跑与本步无关）
    auto profR = profiles_.currentProfile();
    if (profR.isFail()) {
        return configFail(profR.errorMessage());
    }
    const core::LlmDirectProfile& profile = profR.value();
    if (profile.baseUrl.empty()) {
        return configFail("激活 profile 未配置 Base URL，请在 Claude 页补全");
    }
    if (profile.apiKeyPlain.empty()) {
        return configFail("激活 profile 未配置 API Key，请在 Claude 页补全");
    }

    // 2. 槽位 → 实际 model id（model 形参是槽位键名，AgentLoop 原样传 modelSlot）
    auto id = resolveSlotModel(profile, model);
    if (!id.has_value()) {
        return configFail("无法解析模型槽位「" + model +
                          "」（该槽位未配置模型，请在 Claude 页补全）");
    }
    std::string modelId = id.value();
    // [1M] 是客户端侧 1M 上下文标记，网关一律不认此后缀——漏剥即 model null（星锐/百炼实测）
    modelId = stripOneMSuffix(modelId);
    const bool opencode = isOpencodeZen(profile);

    // 3. 组请求体：每条 contentJson parse 成数组再套 role，逐条 stringify 拼数组
    std::vector<std::string> messageTexts;
    messageTexts.reserve(messages.size());
    for (const core::LlmMessage& m : messages) {
        auto content = platform::json::parse(m.contentJson);
        if (content.isFail() || !content.value().isArray()) {
            return core::Result<core::LlmResponse>::fail(
                core::ErrorCode::JsonParseError,
                "消息 content 不是合法 JSON 数组（role=" + m.role + "）");
        }
        platform::json::JsonObject obj;
        obj.set("role", platform::json::makeString(m.role));
        obj.set("content", std::move(content.value()));
        auto text = platform::json::stringify(
            platform::json::JsonValue(std::move(obj)));
        if (text.isFail()) {
            return core::Result<core::LlmResponse>::fail(
                core::ErrorCode::JsonParseError,
                "序列化 messages 失败：" + text.errorMessage());
        }
        messageTexts.push_back(std::move(text.value()));
    }
    auto messagesArr = parseJoinedArray(messageTexts, "messages");
    if (messagesArr.isFail()) {
        return core::Result<core::LlmResponse>::fail(messagesArr.errorCode(),
                                                     messagesArr.errorMessage());
    }

    platform::json::JsonObject body;
    body.set("model", platform::json::makeString(modelId));
    body.set("max_tokens", platform::json::makeInt(kLlmMaxTokens));
    body.set("messages", std::move(messagesArr.value()));
    // tools 为空数组/空串时不带 tools 键；非空才 parse，坏定义当场拦下
    if (!toolsJson.empty() && toolsJson != "[]") {
        auto tools = platform::json::parse(toolsJson);
        if (tools.isFail() || !tools.value().isArray()) {
            return core::Result<core::LlmResponse>::fail(
                core::ErrorCode::JsonParseError, "解析 tools 定义失败");
        }
        body.set("tools", std::move(tools.value()));
    }
    if (!systemPrompt.empty()) {
        body.set("system", platform::json::makeString(systemPrompt));
    }
    auto bodyText = platform::json::stringify(
        platform::json::JsonValue(std::move(body)));
    if (bodyText.isFail()) {
        return core::Result<core::LlmResponse>::fail(
            core::ErrorCode::JsonParseError,
            "序列化请求体失败：" + bodyText.errorMessage());
    }

    // 4. 请求：直发上游（不经 127.0.0.1 代理），鉴权头由本处按 authField 注入
    platform::http::Request req;
    req.method = "POST";
    req.url = stripTrailingSlash(profile.baseUrl) + "/v1/messages";
    req.headers["content-type"] = "application/json";
    req.headers["anthropic-version"] = kAnthropicVersion;
    req.headers["User-Agent"] = kClaudeCliUserAgent;
    const std::string authField = toLowerAscii(profile.authField);
    if (authField != "bearer") {
        req.headers["x-api-key"] = profile.apiKeyPlain;
    }
    if (authField != "api_key") {
        req.headers["Authorization"] = "Bearer " + profile.apiKeyPlain;
    }
    if (opencode) {
        req.headers["x-opencode-session"] = upstreamSessionId();
    }
    req.body = bodyText.value();
    req.timeoutMs = kLlmTimeoutMs;
    req.verifyTls = true;

    // 5. 发送：测试经 HttpFn 注入，生产走 platform::http::send
    auto resp = http_ ? http_(req) : platform::http::send(req);
    if (resp.isFail()) {
        return core::Result<core::LlmResponse>::fail(resp.errorCode(),
                                                     resp.errorMessage());
    }

    // 6. 非 200 → HttpError，带状态码 + body 前 200 字节（按字节截，不保证 UTF-8 边界）
    const platform::http::Response& res = resp.value();
    if (res.statusCode != 200) {
        return core::Result<core::LlmResponse>::fail(
            core::ErrorCode::HttpError,
            "上游 HTTP " + std::to_string(res.statusCode) + ": " +
                res.body.substr(0, 200));
    }

    auto parsed = platform::json::parse(res.body);
    if (parsed.isFail() || !parsed.value().isObject()) {
        return core::Result<core::LlmResponse>::fail(
            core::ErrorCode::JsonParseError,
            std::string("解析响应 JSON 失败：")
                + (parsed.isFail() ? parsed.errorMessage() : "顶层不是对象"));
    }
    platform::json::JsonObject root = parsed.value().asObject();

    core::LlmResponse out;
    platform::json::JsonValue stop = root.get("stop_reason");
    out.stopReason = stop.isString() ? stop.asString() : std::string();

    platform::json::JsonValue content = root.get("content");
    if (!content.isArray()) {
        // 200 却无 content blocks 数组 = 上游自相矛盾；不硬凑 "null"/"[]"，原样报错
        return core::Result<core::LlmResponse>::fail(
            core::ErrorCode::JsonParseError,
            "响应缺 content blocks 数组（stop_reason=" + out.stopReason + "）");
    }
    auto contentText = platform::json::stringify(content);
    if (contentText.isFail()) {
        return core::Result<core::LlmResponse>::fail(
            core::ErrorCode::JsonParseError,
            "序列化响应 content 失败：" + contentText.errorMessage());
    }
    out.contentJson = std::move(contentText.value());

    platform::json::JsonValue usage = root.get("usage");
    if (usage.isObject()) {
        platform::json::JsonObject usageObj = usage.asObject();
        out.inputTokens = usageInt(usageObj, "input_tokens");
        out.outputTokens = usageInt(usageObj, "output_tokens");
    }
    return core::Result<core::LlmResponse>::ok(std::move(out));
}

} // namespace module::agent
