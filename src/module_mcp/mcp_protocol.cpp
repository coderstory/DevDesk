#include "mcp_protocol.h"

#include <cctype>
#include <memory>
#include <string>
#include <utility>

#include "core/logging.h"
#include "platform/json.h"
#include "platform/utf8.h"

namespace module::mcp {

namespace {

using platform::json::JsonObject;
using platform::json::JsonValue;

std::string trimmed(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end &&
           std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return text.substr(begin, end - begin);
}

bool isSupportedVersion(const std::string& version) {
    const std::string value = trimmed(version);
    return value == kMcpProtocolVersion || value == kMcpProtocolVersionLegacy;
}

// 所有进入响应体的字符串值都先 sanitize 再走 JSON 工厂 + stringify：非法 UTF-8
// 会让后续 parse 自检失败（stringify 允许、parse 严格）。
std::string quoteString(const std::string& text) {
    auto serialized = platform::json::stringify(
        platform::json::makeString(platform::utf8::sanitize(text)));
    return serialized.isOk() ? serialized.value() : std::string("\"\"");
}

std::string errorResponseText(const JsonValue& id, int code,
                              const std::string& message) {
    JsonObject error;
    error.set("code", platform::json::makeInt(code));
    error.set("message",
              platform::json::makeString(platform::utf8::sanitize(message)));
    JsonObject root;
    root.set("jsonrpc", platform::json::makeString("2.0"));
    root.set("id", id);
    root.set("error", JsonValue(error));
    auto text = platform::json::stringify(root);
    if (text.isFail()) {
        // 工厂/序列化双重失败只可能是内存耗尽：给 ASCII 兜底，绝不拼未转义文本
        return R"({"jsonrpc":"2.0","id":null,"error":{"code":-32603,"message":"internal error"}})";
    }
    return text.value();
}

std::string successResponseText(const JsonValue& id, const JsonValue& result) {
    JsonObject root;
    root.set("jsonrpc", platform::json::makeString("2.0"));
    root.set("id", id);
    root.set("result", result);
    auto text = platform::json::stringify(root);
    if (text.isFail()) {
        return errorResponseText(id, -32603, "响应序列化失败");
    }
    return text.value();
}

// 片段拼接后用 parse 自检/规范化（同 agent_loop 的 parseArrayOrDie 口径）
core::Result<JsonValue> parseFragment(const std::string& fragment) {
    return platform::json::parse(fragment);
}

} // namespace

McpProtocol::McpProtocol(McpSessionStore& sessions,
                         core::mcp::McpToolRegistry& tools,
                         std::string serverVersion)
    : sessions_(sessions),
      tools_(tools),
      serverVersion_(std::move(serverVersion)) {}

HttpOutcome McpProtocol::handlePost(const std::string& body,
                                    const std::string& sessionId,
                                    const std::string& protocolVersionHeader,
                                    std::string& newSessionIdOut) {
    newSessionIdOut.clear();

    // 判序（spec §5.4，自上而下先命中先返回）：会话 → 版本头 → JSON 解析。
    if (!sessionId.empty() && !sessions_.has(sessionId)) {
        return HttpOutcome{404, "", false};
    }

    if (!protocolVersionHeader.empty() &&
        !isSupportedVersion(protocolVersionHeader)) {
        // 版本头只约束非 initialize 请求；解析失败 = 不可能是 initialize。
        bool bodyIsInitialize = false;
        auto probe = platform::json::parse(body);
        if (probe.isOk() && probe.value().isObject()) {
            const JsonValue method = probe.value().asObject().get("method");
            bodyIsInitialize = method.isString() &&
                               method.asString() == "initialize";
        }
        if (!bodyIsInitialize) return HttpOutcome{400, "", false};
    }

    auto parsed = platform::json::parse(body);
    if (parsed.isFail()) {
        return HttpOutcome{400, errorResponseText(JsonValue(), -32700,
                                                 "JSON 解析失败"),
                           false};
    }
    const JsonValue message = parsed.value();
    if (message.isArray()) {
        return HttpOutcome{400, errorResponseText(JsonValue(), -32600,
                                                 "不支持批处理（JSON 数组）"),
                           false};
    }
    if (!message.isObject()) {
        return HttpOutcome{400, errorResponseText(JsonValue(), -32600,
                                                 "不是合法的 JSON-RPC 请求"),
                           false};
    }
    const JsonObject request = message.asObject();
    const JsonValue methodValue = request.get("method");
    if (!methodValue.isString()) {
        return HttpOutcome{400, errorResponseText(JsonValue(), -32600,
                                                 "缺少 method"),
                           false};
    }

    // 会话门禁（收口批；规范 SHOULD + 官方 SDK 400 "Missing session ID"）：发了
    // 会话的服务,无 Mcp-Session-Id 的非 initialize 请求一律 400 空 body,通知
    // 不豁免——防会话记账被绕过。判序在结构校验（-32700/-32600/批处理）之后、
    // 通知 202 之前;initialize 豁免——会话由它创建。
    if (sessionId.empty() && methodValue.asString() != "initialize") {
        return HttpOutcome{400, "", false};
    }

    // 无 id 成员 = 通知：一律 202 无 body；initialize 通知不建会话
    if (!request.has("id")) {
        return HttpOutcome{202, "", true};
    }
    const JsonValue id = request.get("id");
    const std::string method = methodValue.asString();

    if (method == "initialize") {
        std::string negotiated = kMcpProtocolVersion;
        const JsonValue params = request.get("params");
        if (params.isObject()) {
            const JsonValue requested =
                params.asObject().get("protocolVersion");
            if (requested.isString() && isSupportedVersion(requested.asString())) {
                negotiated = trimmed(requested.asString());
            }
        }
        newSessionIdOut = sessions_.create();

        JsonObject toolsCapability;
        toolsCapability.set("listChanged", platform::json::makeBool(false));
        JsonObject capabilities;
        capabilities.set("tools", JsonValue(toolsCapability));
        JsonObject serverInfo;
        serverInfo.set("name", platform::json::makeString("devdesk"));
        serverInfo.set("version", platform::json::makeString(
                                      platform::utf8::sanitize(serverVersion_)));
        JsonObject result;
        result.set("protocolVersion", platform::json::makeString(negotiated));
        result.set("capabilities", JsonValue(capabilities));
        result.set("serverInfo", JsonValue(serverInfo));
        result.set("instructions", platform::json::makeString(
                                       "本服务运行在本机，提供网页抓取等工具。"));
        return HttpOutcome{200, successResponseText(id, JsonValue(result)),
                           false};
    }

    if (method == "ping") {
        return HttpOutcome{
            200, successResponseText(id, platform::json::makeObject()), false};
    }

    if (method == "tools/list") {
        std::string toolsJson = "[";
        bool first = true;
        for (const auto& tool : tools_.list()) {
            if (!tool) continue;
            // 单个工具 schema 非法只跳过它并告警，不拖垮整个列表
            auto schema = platform::json::parse(tool->inputSchemaJson());
            if (schema.isFail() || !schema.value().isObject()) {
                LOG_WARN("MCP tools/list 跳过 inputSchema 非法的工具: " +
                         tool->name());
                continue;
            }
            auto schemaText = platform::json::stringify(schema.value());
            if (schemaText.isFail()) {
                LOG_WARN("MCP tools/list 跳过无法序列化 schema 的工具: " +
                         tool->name());
                continue;
            }
            if (!first) toolsJson += ",";
            first = false;
            toolsJson += "{\"name\":" + quoteString(tool->name()) +
                         ",\"description\":" + quoteString(tool->description()) +
                         ",\"inputSchema\":" + schemaText.value() + "}";
        }
        toolsJson += "]";
        auto result = parseFragment("{\"tools\":" + toolsJson + "}");
        if (result.isFail() || !result.value().isObject()) {
            return HttpOutcome{200, errorResponseText(id, -32603,
                                                     "工具列表构造失败"),
                               false};
        }
        return HttpOutcome{200, successResponseText(id, result.value()), false};
    }

    if (method == "tools/call") {
        const JsonValue paramsValue = request.get("params");
        if (!paramsValue.isObject()) {
            return HttpOutcome{200, errorResponseText(id, -32602,
                                                     "params 必须是对象"),
                               false};
        }
        const JsonObject params = paramsValue.asObject();
        const JsonValue nameValue = params.get("name");
        if (!nameValue.isString() || nameValue.asString().empty()) {
            return HttpOutcome{200,
                               errorResponseText(id, -32602,
                                                 "params.name 必须是非空字符串"),
                               false};
        }
        const std::string toolName = nameValue.asString();

        std::string argumentsJson = "{}";
        const JsonValue arguments = params.get("arguments");
        if (arguments.isObject()) {
            auto serialized = platform::json::stringify(arguments);
            if (serialized.isFail()) {
                return HttpOutcome{200,
                                   errorResponseText(id, -32602,
                                                     "params.arguments 无法序列化"),
                                   false};
            }
            argumentsJson = serialized.value();
        } else if (!arguments.isNull()) {
            return HttpOutcome{200,
                               errorResponseText(id, -32602,
                                                 "params.arguments 必须是 JSON 对象"),
                               false};
        }

        auto found = tools_.find(toolName);
        if (found.isFail()) {
            return HttpOutcome{200,
                               errorResponseText(id, -32602,
                                                 "工具不存在: " + toolName),
                               false};
        }
        // keep-alive：execute 期间即使 registry 里被 unregister 也不悬垂
        const std::shared_ptr<core::mcp::IMcpTool> tool = found.value();
        auto executed = tool->execute(argumentsJson);
        if (executed.isFail()) {
            // 工具业务失败必须用 ToolResult{isError=true}；fail = 基础设施错误
            return HttpOutcome{200,
                               errorResponseText(id, -32603,
                                                 executed.errorMessage()),
                               false};
        }
        const core::mcp::ToolResult toolResult = executed.value();
        std::string content = "{\"content\":[{\"type\":\"text\",\"text\":" +
                              quoteString(toolResult.text) + "}]";
        if (toolResult.isError) content += ",\"isError\":true";
        content += "}";
        auto result = parseFragment(content);
        if (result.isFail() || !result.value().isObject()) {
            return HttpOutcome{200, errorResponseText(id, -32603,
                                                     "工具结果构造失败"),
                               false};
        }
        return HttpOutcome{200, successResponseText(id, result.value()), false};
    }

    return HttpOutcome{200,
                       errorResponseText(id, -32601, "未知方法: " + method),
                       false};
}

HttpOutcome McpProtocol::handleDelete(const std::string& sessionId) {
    if (sessionId.empty()) return HttpOutcome{200, "", false};
    if (!sessions_.has(sessionId)) return HttpOutcome{404, "", false};
    sessions_.remove(sessionId);
    return HttpOutcome{200, "", false};
}

} // namespace module::mcp