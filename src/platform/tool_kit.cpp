#include "platform/tool_kit.h"

#include <utility>

#include "core/logging.h"
#include "platform/utf8.h"

namespace pj = platform::json;

namespace platform::tool_kit {

core::Result<core::ToolResult> toolError(const std::string& message) {
    pj::JsonObject err;
    err.set("error", pj::makeString(message));
    auto text = pj::stringify(pj::JsonValue(std::move(err)));
    if (text.isFail()) {
        // 自有字面量 + 一个字符串字段的序列化失败 = 编程不变量破坏
        return core::Result<core::ToolResult>::fail(
            core::ErrorCode::Internal,
            "工具错误消息序列化失败: " + text.errorMessage());
    }
    return core::Result<core::ToolResult>::ok(
        core::ToolResult{true, text.value()});
}

core::Result<platform::json::JsonObject> parseArgs(const core::ToolArgs& args) {
    if (args.json.empty()) {
        return core::Result<platform::json::JsonObject>::ok(
            platform::json::JsonObject());
    }
    auto v = platform::json::parse(args.json);
    if (v.isFail() || !v.value().isObject()) {
        return core::Result<platform::json::JsonObject>::fail(
            core::ErrorCode::ValidationError, "参数不是合法 JSON 对象");
    }
    return core::Result<platform::json::JsonObject>::ok(v.value().asObject());
}

std::string strField(const platform::json::JsonObject& obj, const char* key) {
    platform::json::JsonValue v = obj.get(key);
    return v.isString() ? v.asString() : std::string();
}

std::optional<int64_t> intField(const platform::json::JsonObject& obj,
                                const char* key) {
    platform::json::JsonValue v = obj.get(key);
    if (!v.isNumber()) return std::nullopt;
    return std::optional<int64_t>(v.asInt());
}

core::Result<std::string> arrayOfJsonTexts(
    const std::vector<std::string>& items) {
    std::string out = "[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) out += ",";
        out += items[i];
    }
    out += "]";
    if (platform::json::parse(out).isFail()) {
        return core::Result<std::string>::fail(core::ErrorCode::Internal,
                                               "结果数组组装校验失败");
    }
    return core::Result<std::string>::ok(out);
}

std::string mcpToolName(const std::string& agentToolName) {
    std::string out = agentToolName;
    for (char& c : out) {
        if (c == '.' || c == '-') c = '_';
    }
    return out;
}

McpToolAdapter::McpToolAdapter(std::shared_ptr<core::ITool> tool,
                               std::string mcpName)
    : tool_(std::move(tool)), mcpName_(std::move(mcpName)) {
    // schemaJson 形状（core/tool.h 契约）= {"name","description","input_schema"}：
    // 拆出后两项直接用；拆失败（空/非法）退化到空参数 schema，不让单个坏工具
    // 拖垮整页 tools/list。
    auto parsed = platform::json::parse(tool_->schemaJson());
    if (parsed.isOk() && parsed.value().isObject()) {
        const platform::json::JsonObject schema = parsed.value().asObject();
        const platform::json::JsonValue desc = schema.get("description");
        if (desc.isString()) description_ = desc.asString();
        const platform::json::JsonValue in = schema.get("input_schema");
        if (in.isObject()) {
            auto text = platform::json::stringify(in);
            if (text.isOk()) inputSchema_ = text.value();
        }
    }
    if (inputSchema_.empty()) {
        inputSchema_ = R"({"type":"object","properties":{}})";
    }
}

core::Result<core::mcp::ToolResult> McpToolAdapter::execute(
    const std::string& argumentsJson) {
    auto r = tool_->execute(core::ToolArgs{argumentsJson});
    if (r.isFail()) {
        return core::Result<core::mcp::ToolResult>::fail(r.errorCode(),
                                                         r.errorMessage());
    }
    // contentJson 原样当文本透传（结构由宿主侧解析）；先 UTF-8 归一，防
    // 非法字节把 MCP 响应 JSON 写成坏串（utf8.h 头注：工具边界的既有做法）。
    return core::Result<core::mcp::ToolResult>::ok(core::mcp::ToolResult{
        r.value().isError,
        platform::utf8::sanitize(r.value().contentJson)});
}

void registerToolsAsMcp(
    core::mcp::McpToolRegistry& registry,
    const std::vector<std::shared_ptr<core::ITool>>& tools) {
    for (const auto& tool : tools) {
        if (tool == nullptr) continue;
        const std::string name = mcpToolName(tool->name());
        auto r = registry.registerTool(
            std::make_shared<McpToolAdapter>(tool, name));
        if (r.isFail()) {
            LOG_ERROR("MCP 对外注册失败（不阻断启动）: " + name + " — " +
                      r.errorMessage());
        }
    }
}

void unregisterToolsAsMcp(
    core::mcp::McpToolRegistry& registry,
    const std::vector<std::shared_ptr<core::ITool>>& tools) {
    for (const auto& tool : tools) {
        if (tool == nullptr) continue;
        registry.unregisterTool(mcpToolName(tool->name()));
    }
}

}  // namespace platform::tool_kit
