// src/module_agent/llm_client.cpp
// LlmClient 实现（agent M1 Task 7）。逻辑序 = spec §1 步骤 3：
//   1. 预检代理运行状态   2. 槽位 → model id   3. 组请求体   4. 发送（可注入缝）
//   5. 非 200 → HttpError   6. 解析响应 → LlmResponse
// 不抛异常，全程 Result。platform::json 没有对象数组 push 口径（json.h:107-109，
// makeArray 是空桩），messages 数组按 AgentLoop joinBlockArray 同款路数组装：
// 逐元素 stringify → 逗号拼接 → parse 一次自检（与 agent_loop.cpp:32-57 同源，
// 区别只在解析失败这里降级成 send 失败——文本含外部来源，坏请求当场拦下、
// 不发上游、不 abort）。
#include "module_agent/llm_client.h"

#include <string>
#include <utility>
#include <vector>

#include "platform/json.h"

namespace module::agent {
namespace {

// 单次请求 max_tokens（spec §1 留白 → 计划定值 4096；M1 不做每任务配置）
constexpr int64_t kLlmMaxTokens = 4096;

// 把已各自 stringify 的元素文本拼成 JSON 数组原文并 parse 自检。
// 每个元素是 stringify(合法 JsonValue) 的产物（构造保证），拼接本身只加
// 逗号与方括号；parse 这一道是「拼装层出错当场拦下」的自检，失败返回
// JsonParseError，绝不把坏 body 发给上游。
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

// 响应 usage 里的整数字段；缺 / 非数字 → -1（LlmResponse「未采集」口径）。
// 注：yyjson 读侧 Int 与 Double 同映 T_Number、asInt 只认 Int 形——Anthropic
// 的 token 计数恒为整数，网关若回小数会取成 0（agent_loop 对 <=0 不累计，
// 只丢统计不丢数据，可接受）。
int64_t usageInt(const platform::json::JsonObject& usage, const char* key) {
    platform::json::JsonValue v = usage.get(key);
    return v.isNumber() ? v.asInt() : -1;
}

} // namespace

LlmClient::LlmClient(core::IClaudeProxyService& proxy, HttpFn http)
    : proxy_(proxy), http_(std::move(http)) {}

core::Result<core::LlmResponse> LlmClient::send(
    const std::vector<core::LlmMessage>& messages,
    const std::string& toolsJson,
    const std::string& systemPrompt,
    const std::string& model) {
    // 1. 预检：代理没起就没有可打的本地端口（UI 先启代理是前置条件；
    //    这里再拦一道，错误信息给用户可操作的指引）
    if (!proxy_.isProxyRunning()) {
        return core::Result<core::LlmResponse>::fail(
            core::ErrorCode::ValidationError,
            "代理未启，请先在 Claude 页启动代理");
    }
    // 2. 槽位 → 实际 model id（H1 定案：解析在 LlmClient 内做，model 形参
    //    是槽位键名；AgentLoop 原样传 task.modelSlot）
    auto id = proxy_.resolveModelSlot(model);
    if (!id.has_value()) {
        return core::Result<core::LlmResponse>::fail(
            core::ErrorCode::ValidationError,
            "无法解析模型槽位「" + model + "」（无激活 profile 或槽位未配置）");
    }

    // 3. 组请求体。messages：每条 contentJson 先 parse 成数组再套上 role，
    //    逐条 stringify 拼成数组文本（无对象数组 push 口径，见文件头注）
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
    body.set("model", platform::json::makeString(id.value()));
    body.set("max_tokens", platform::json::makeInt(kLlmMaxTokens));
    body.set("messages", std::move(messagesArr.value()));
    // tools 定义为空数组 / 空串时不带 tools 键（上游对空数组同样接受，
    // 但省一个键省一次解析；非空才 parse——坏定义当场拦下）
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

    platform::http::Request req;
    req.method = "POST";
    req.url = "http://127.0.0.1:" + std::to_string(proxy_.proxyPort()) +
              "/v1/messages";
    req.headers["content-type"] = "application/json";
    // anthropic-version / 鉴权头由本地代理补注入，LlmClient 不碰
    req.body = bodyText.value();
    req.timeoutMs = 60000;

    // 4. 发送：测试经 HttpFn 注入假发送，生产走 platform::http::send；
    //    失败原样透传（错误码与消息都是调用方可读的）
    auto resp = http_ ? http_(req) : platform::http::send(req);
    if (resp.isFail()) {
        return core::Result<core::LlmResponse>::fail(resp.errorCode(),
                                                     resp.errorMessage());
    }

    // 5. 非 200 → HttpError，带状态码 + body 前 200 字节（够定位 4xx 报错；
    //    诊断文本按字节截，不保证 UTF-8 边界，可接受）
    const platform::http::Response& res = resp.value();
    if (res.statusCode != 200) {
        return core::Result<core::LlmResponse>::fail(
            core::ErrorCode::HttpError,
            "上游 HTTP " + std::to_string(res.statusCode) + ": " +
                res.body.substr(0, 200));
    }

    // 6. 解析响应 → LlmResponse
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
        // 200 却没有 content blocks 数组 = 上游自相矛盾；不造 "null"/"[]"
        // 硬凑，把矛盾原样报给 run 落库
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
