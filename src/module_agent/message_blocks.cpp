// contentJson 块级解码与渲染辅助实现（契约见 message_blocks.h）
#include "module_agent/message_blocks.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <core/render/text.h>

#include "platform/json.h"

namespace module::agent {

namespace {

namespace pj = platform::json;

std::string strField(const pj::JsonObject& obj, const char* key) {
    pj::JsonValue v = obj.get(key);
    return v.isString() ? v.asString() : std::string();
}

// 未知/异常块：stringify 保留原文，失败再退空串
std::string rawOf(const pj::JsonValue& blockValue) {
    auto text = pj::stringify(blockValue);
    return text.isOk() ? text.value() : std::string();
}

// tool_result.content 摊平：字符串 → 原值；blocks 数组 → 拼各 text 块；其他 → stringify
std::string flattenToolResultContent(const pj::JsonValue& content) {
    if (content.isString()) return content.asString();
    if (content.isArray()) {
        std::string out;
        for (int i = 0; i < content.size(); ++i) {
            pj::JsonValue item = content[i];
            std::string piece;
            if (item.isObject() &&
                strField(item.asObject(), "type") == "text") {
                piece = strField(item.asObject(), "text");
            } else {
                // 非 text 块 / 非对象项：stringify 原文保留，不丢内容
                piece = rawOf(item);
            }
            if (!out.empty()) out += "\n";
            out += piece;
        }
        return out;
    }
    if (content.isNull()) return std::string();
    auto text = pj::stringify(content);
    return text.isOk() ? text.value() : std::string();
}

// 单行化：换行/制表 → 空格。预览与工具 chip 是 wrap(false) 单行元素，\n 会画出框外
std::string flattenToSingleLine(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool lastSpace = false;
    for (const char c : s) {
        if (c == '\n' || c == '\r' || c == '\t') {
            if (!lastSpace) out += ' ';
            lastSpace = true;
            continue;
        }
        lastSpace = (c == ' ');
        out += c;
    }
    return out;
}

} // namespace

std::vector<MessageBlock> decodeMessageBlocks(const std::string& contentJson) {
    std::vector<MessageBlock> blocks;
    if (contentJson.empty()) return blocks;

    auto parsed = pj::parse(contentJson);
    if (parsed.isFail()) {
        // 坏 JSON（含旧版纯文本行）：单 Plain 块原文回落
        blocks.push_back(MessageBlock{MessageBlock::Type::Plain, contentJson,
                                      std::string(), false});
        return blocks;
    }
    const pj::JsonValue& root = parsed.value();
    if (root.isString()) {
        // JSON 字符串字面量（网关兼容形态）→ Text 取值
        blocks.push_back(MessageBlock{MessageBlock::Type::Text,
                                      root.asString(), std::string(), false});
        return blocks;
    }
    if (!root.isArray()) {
        // 合法 JSON 但非数组（对象/数字）：原文回落
        blocks.push_back(MessageBlock{MessageBlock::Type::Plain,
                                      rawOf(root), std::string(), false});
        return blocks;
    }

    for (int i = 0; i < root.size(); ++i) {
        pj::JsonValue item = root[i];
        if (!item.isObject()) {
            blocks.push_back(MessageBlock{MessageBlock::Type::Plain,
                                          rawOf(item), std::string(), false});
            continue;
        }
        pj::JsonObject obj = item.asObject();
        const std::string type = strField(obj, "type");
        if (type == "text") {
            pj::JsonValue text = obj.get("text");
            blocks.push_back(MessageBlock{
                MessageBlock::Type::Text,
                text.isString() ? text.asString() : rawOf(text), std::string(),
                false});
        } else if (type == "tool_use") {
            MessageBlock b;
            b.type = MessageBlock::Type::ToolUse;
            b.toolName = strField(obj, "name");
            pj::JsonValue input = obj.get("input");
            b.text = input.isNull() ? "{}" : rawOf(input);
            blocks.push_back(std::move(b));
        } else if (type == "tool_result") {
            MessageBlock b;
            b.type = MessageBlock::Type::ToolResult;
            pj::JsonValue content = obj.get("content");
            b.text = flattenToolResultContent(content);
            pj::JsonValue err = obj.get("is_error");
            b.isError = err.isBool() && err.asBool();
            blocks.push_back(std::move(b));
        } else {
            // 未知块类型（thinking 等）：Plain 原文保留，不丢内容
            blocks.push_back(MessageBlock{MessageBlock::Type::Plain,
                                          rawOf(item), std::string(), false});
        }
    }
    return blocks;
}

std::string messagePlainText(const std::vector<MessageBlock>& blocks) {
    std::string out;
    for (const auto& b : blocks) {
        if (!out.empty()) out += "\n";
        if (b.type == MessageBlock::Type::ToolUse) {
            out += b.toolName + "(" + toolArgsSummary(b.text, 80) + ")";
        } else {
            out += b.text;
        }
    }
    return out;
}

std::string finalAssistantText(const std::vector<AgentMessageRow>& msgs) {
    const AgentMessageRow* lastAssistant = nullptr;
    for (const auto& m : msgs) {
        if (m.role == "assistant") lastAssistant = &m;
    }
    if (lastAssistant == nullptr) return std::string();
    const auto blocks = decodeMessageBlocks(lastAssistant->contentJson);
    std::string out;
    for (const auto& b : blocks) {
        if (b.type != MessageBlock::Type::Text) continue;
        if (!out.empty()) out += "\n";
        out += b.text;
    }
    return out;
}

std::string utf8Truncate(const std::string& s, size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    size_t cut = maxBytes;
    while (cut > 0 &&
           (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return s.substr(0, cut);
}

std::string messagePreviewFromJson(const std::string& contentJson,
                                   size_t maxBytes) {
    // 先单行化再截断：预览是 wrap(false) 单行元素，\n 会画出元素框外
    const std::string plain = flattenToSingleLine(
        messagePlainText(decodeMessageBlocks(contentJson)));
    std::string preview = utf8Truncate(plain, maxBytes);
    if (preview.size() < plain.size()) {
        // 省略号补在已边界回退的文本后
        preview += "…";
    }
    return preview;
}

std::string toolArgsSummary(const std::string& inputJson, size_t maxBytes) {
    const std::string line = flattenToSingleLine(inputJson);
    std::string cut = utf8Truncate(line, maxBytes);
    if (cut.size() < line.size()) cut += "…";
    return cut;
}

float estimateTextBlockHeight(const std::string& text, float contentW,
                              float fontSize, float lineHeight) {
    const float lh = lineHeight > 0.0f ? lineHeight : fontSize * 1.2f;
    if (text.empty() || contentW <= 0.0f || fontSize <= 0.0f) {
        return lh;   // 至少一行
    }
    // 与渲染同一台引擎测量：measureTextSize 按 style.maxWidth 做相同换行判定，返回 {行宽, 行数×lineHeight}
    core::TextStyle style;
    style.text = text;
    style.fontSize = fontSize;
    style.maxWidth = contentW;
    style.wrap = true;
    style.lineHeight = lh;
    const core::Vec2 size = core::TextPrimitive::measureTextSize(style);
    // 字体不可用（测量全零）时退「按 1 行」
    const float lines = size.y > 0.0f ? std::round(size.y / lh) : 1.0f;
    return std::max(1.0f, lines) * lh;
}

float estimateMessageHeight(const AgentMessageRow& msg, bool expanded,
                            float contentW, float lineHeight) {
    float h = kMsgHeaderH + kMsgGap;
    if (!expanded) {
        return h + lineHeight;   // 预览单行
    }
    const auto blocks = decodeMessageBlocks(msg.contentJson);
    if (blocks.empty()) {
        return h + lineHeight;   // 空消息：占位一行
    }
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (i > 0) h += kMsgGap;
        const auto& b = blocks[i];
        switch (b.type) {
            case MessageBlock::Type::Text:
            case MessageBlock::Type::Plain:
                h += estimateTextBlockHeight(b.text, contentW, 12.0f,
                                             lineHeight);
                break;
            case MessageBlock::Type::ToolUse:
                h += kMsgHeaderH;   // 「🔧 name + 摘要」单行
                break;
            case MessageBlock::Type::ToolResult:
                h += lineHeight;    // 「工具输出」标签行
                h += kMsgGap;       // 标签行与输出文本之间的列间距
                h += estimateTextBlockHeight(b.text, contentW, 12.0f,
                                             lineHeight);
                break;
        }
    }
    return h;
}

float estimateMessagesAreaHeight(const std::vector<AgentMessageRow>& msgs,
                                 const std::set<int64_t>& expandedSeqs,
                                 float contentW, float lineHeight,
                                 float minH, float maxH) {
    float total = 0.0f;
    for (size_t i = 0; i < msgs.size(); ++i) {
        if (i > 0) total += kMsgRowGap;   // 条间行距 = scrollView 的 gap（同源）
        const bool expanded = expandedSeqs.count(msgs[i].seq) > 0;
        total += estimateMessageHeight(msgs[i], expanded, contentW, lineHeight);
    }
    if (msgs.empty()) total = minH;
    return std::clamp(total, minH, maxH);
}

std::string messageRoleBadge(const AgentMessageRow& msg) {
    const auto blocks = decodeMessageBlocks(msg.contentJson);
    for (const auto& b : blocks) {
        if (b.type == MessageBlock::Type::ToolResult) return "工具";
    }
    if (msg.role == "user") return "用户";
    if (msg.role == "assistant") return "助手";
    if (msg.role == "tool") return "工具";
    return msg.role.empty() ? "—" : msg.role;
}

} // namespace module::agent
