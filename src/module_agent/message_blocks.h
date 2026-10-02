// src/module_agent/message_blocks.h
// agent_messages.contentJson 的块级解码 + 渲染辅助（run 日志改造，2026-10-02）。
//
// 背景：M1 的 run 日志把 contentJson 原文截 80 字节当一行预览——tool_use /
// tool_result 块完全不可见、长输出看不全（用户反馈①②）。本文件是 contentJson
// 的**唯一解码点**（trigger_config.h 同款「单一编解码」口径）：渲染层、预览、
// 高度估算全走这里，不再各自裸 parse。
//
// contentJson 实际形态（agent_loop.cpp 落库口径，见 keep/keepAssistant）：
//   - user 首条      = [{"type":"text","text":"<goal>"}]
//   - assistant 响应 = LLM content blocks 原文（text / tool_use 混合数组）
//   - tool 回喂      = [{"type":"tool_result",...,"content":<str|blocks>}, ...]
//   - 兼容：纯文本行（旧数据）/ JSON 字符串字面量（网关兼容形态）→ Plain/Text
#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "module_agent/agent_store.h"

namespace module::agent {

// 解码后的单个 content 块。tool_use 的 input 以 JSON 原文存进 text（渲染层
// 自己做单行摘要+截断）；tool_result 的 content 两种形态（字符串 / text blocks
// 数组）都摊平成纯文本。
struct MessageBlock {
    enum class Type {
        Text,       // {"type":"text","text":...}
        ToolUse,    // {"type":"tool_use","name":...,"input":...}
        ToolResult, // {"type":"tool_result","content":...,"is_error":...}
        Plain,      // 非 JSON 原文 / 未知块类型原文（容错回落，不丢内容）
    };
    Type type = Type::Plain;
    std::string text;        // Text/Plain=文本；ToolUse=input JSON 原文；ToolResult=摊平输出
    std::string toolName;    // 仅 ToolUse
    bool isError = false;    // 仅 ToolResult
};

// 解码：contentJson → 块列表。容错不报错（审计日志是只读展示面）：
//   坏 JSON / 非 JSON / 合法 JSON 但非数组 → 单 Plain 块保留原文；
//   JSON 字符串字面量 → 单 Text 块取字符串值；空串 → 空列表。
std::vector<MessageBlock> decodeMessageBlocks(const std::string& contentJson);

// 解码后全部块的纯文本拼接（预览 / 复制按钮用）；tool_use 块贡献
// 「<name>(<input 摘要>)」一行。
std::string messagePlainText(const std::vector<MessageBlock>& blocks);

// UTF-8 边界安全截断：超长时在 maxBytes 内回退到字符边界再截
//（沿用原 messagePreview 的回退逻辑），不加省略号——由调用方决定。
std::string utf8Truncate(const std::string& s, size_t maxBytes);

// 预览文本：decode → 纯文本 → maxBytes 截断 + 「…」。 collapsed 行用。
std::string messagePreviewFromJson(const std::string& contentJson,
                                   size_t maxBytes);

// 工具参数摘要：input JSON 压成单行（换行/连续空白折叠）+ UTF-8 截断。
std::string toolArgsSummary(const std::string& inputJson, size_t maxBytes);

// ---- 高度估算（scroll 区域外高的依据；宁高勿低——估算偏大只会多留白，
//      偏小会让下一行文字压上来）----

// 展开的单个文本块高：按「字符宽单元」估行数（CJK=1 单元、ASCII≈0.55 单元，
// 每单元宽 fontSize*0.55），行数 = 单元数 / 每行单元容量（各段独立，\n 分段），
// 至少 1 行，末尾 +1 行保险。lines*lineHeight。
float estimateTextBlockHeight(const std::string& text, float contentW,
                              float fontSize, float lineHeight);

// 单条消息高（消息头 20 + 间距 4 + 内容）：
//   collapsed = 头 + 预览 1 行；expanded = 头 + Σ 各块高 + 块间距。
float estimateMessageHeight(const AgentMessageRow& msg, bool expanded,
                            float contentW, float lineHeight);

// 消息区滚动容器的建议外高：clamp(Σ 消息高 + 间距, minH, maxH)。
// 真实内容高由 scrollView 自测（contentKey 变更重测），本估算只给外高。
float estimateMessagesAreaHeight(const std::vector<AgentMessageRow>& msgs,
                                 const std::set<int64_t>& expandedSeqs,
                                 float contentW, float lineHeight,
                                 float minH, float maxH);

// 消息头（role 徽章行）固定高与块间距——估算与 compose 必须同源
constexpr float kMsgHeaderH = 20.0f;
constexpr float kMsgGap = 4.0f;

// 消息角色徽章文案：user=用户 / assistant=助手 / 含 tool_result 块的消息
//（落库 role 是 "user"，按内容判）= 工具；其余原样。
std::string messageRoleBadge(const AgentMessageRow& msg);

} // namespace module::agent
