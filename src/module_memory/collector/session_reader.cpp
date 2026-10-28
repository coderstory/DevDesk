// src/module_memory/collector/session_reader.cpp
#include "session_reader.h"

#include "platform/json.h"

namespace module::memory::collector {

namespace {

using platform::json::JsonValue;

// ② 注入块标记：正文携带即整条丢弃（system-reminder 形态由 Claude Code 包裹）
bool containsInjectionMarker(const std::string& text) {
    return text.find("<system-reminder") != std::string::npos ||
           text.find("system-reminder:") != std::string::npos;
}

// 消息体抽取：content 为字符串直接用；为数组时按块类型走①
//（text 保留 / thinking、tool_use 丢弃 / tool_result 仅错误时留摘要）。
// 返回 false = 该消息无正文（非 user/assistant 角色或全部块被过滤）。
bool extractMessageText(const JsonValue& message, bool* narrative,
                        std::string* textOut) {
    if (!message.isObject()) return false;
    const std::string role = message.asObject().get("role").asString();
    if (role != "user" && role != "assistant") return false;
    const JsonValue content = message.asObject().get("content");
    if (content.isString()) {
        *textOut = content.asString();
        *narrative = true;  // 字符串正文 = 真文本
        return true;
    }
    if (!content.isArray()) return false;
    std::string merged;
    for (int i = 0; i < content.size(); ++i) {
        const JsonValue block = content[i];
        if (!block.isObject()) continue;
        const std::string type = block.asObject().get("type").asString();
        if (type == "text") {
            merged += block.asObject().get("text").asString();
            merged += '\n';
            *narrative = true;  // text 块 = 真文本（工具摘要不算）
        } else if (type == "tool_result" && block.asObject().get("is_error").isBool() &&
                   block.asObject().get("is_error").asBool()) {
            // ① 工具结果只在错误/失败时保留摘要
            const JsonValue body = block.asObject().get("content");
            std::string summary =
                body.isString() ? body.asString() : std::string();
            if (summary.size() > kToolErrorSummaryBytes) {
                summary = truncateUtf8(summary, kToolErrorSummaryBytes);
            }
            merged += "[tool_error] " + summary + "\n";
        }
        // thinking / tool_use / 其余块：一律丢弃
    }
    if (merged.empty()) return false;
    *textOut = merged;
    return true;  // *narrative 只在取到 text 块/字符串时为真
}

}  // namespace

std::string truncateUtf8(const std::string& text, std::size_t maxBytes) {
    if (text.size() <= maxBytes) return text;
    std::size_t cut = maxBytes;
    // 回退到码点边界：续字节（10xxxxxx）不属于码点首字节
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return text.substr(0, cut);
}

core::Result<std::vector<SessionEvent>> parseSessionLine(
    const std::string& line, std::uint64_t seq) {
    std::vector<SessionEvent> out;
    const auto parsed = platform::json::parse(line);
    if (parsed.isFail()) {
        return core::Result<std::vector<SessionEvent>>::fail(
            core::ErrorCode::JsonParseError, "坏行（非 JSON）: 行 " +
                                                 std::to_string(seq));
    }
    const JsonValue root = parsed.value();
    if (!root.isObject()) {
        return core::Result<std::vector<SessionEvent>>::ok(std::move(out));
    }
    // ① 角色过滤只发生在 message 层；顶层 type 限定 user/assistant 两种行型
    const std::string type = root.asObject().get("type").asString();
    if (type != "user" && type != "assistant") {
        return core::Result<std::vector<SessionEvent>>::ok(std::move(out));
    }
    bool narrative = false;
    std::string text;
    if (!extractMessageText(root.asObject().get("message"), &narrative, &text)) {
        return core::Result<std::vector<SessionEvent>>::ok(std::move(out));
    }
    if (containsInjectionMarker(text)) {  // ②
        return core::Result<std::vector<SessionEvent>>::ok(std::move(out));
    }
    text = truncateUtf8(text, kMaxEventBytes);  // ③
    SessionEvent ev;
    ev.sessionId = root.asObject().get("sessionId").asString();
    ev.eventSeq = seq;
    ev.narrative = narrative;
    ev.text = std::move(text);
    out.push_back(std::move(ev));
    return core::Result<std::vector<SessionEvent>>::ok(std::move(out));
}

}  // namespace module::memory::collector
