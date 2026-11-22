#include "platform/tool_kit.h"
#include <utility>

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

}  // namespace platform::tool_kit
