// agent_messages.contentJson 的块级解码 + 渲染辅助，是 contentJson 的唯一解码点
//（trigger_config.h 同款「单一编解码」口径）：渲染层、预览、高度估算全走这里。
// 形态：user 首条 = [{"type":"text",...}]；assistant = LLM content blocks 原文；
// tool 回喂 = [{"type":"tool_result",...}]；坏 JSON / 字符串字面量 → Plain/Text 兼容
#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "module_agent/agent_store.h"

namespace module::agent {

// tool_use input 以 JSON 原文存 text（渲染层自做摘要+截断）；tool_result content 两形态都摊平为纯文本
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

// 解码容错不报错：坏 JSON / 非数组 → 单 Plain 原文；JSON 字符串字面量 → 单 Text；空串 → 空列表
std::vector<MessageBlock> decodeMessageBlocks(const std::string& contentJson);

// 全部块纯文本拼接（预览/复制用）；tool_use 贡献「<name>(<input 摘要>)」一行
std::string messagePlainText(const std::vector<MessageBlock>& blocks);

// run 最终回复全文（详情弹窗「复制回复」chip 的数据源）：取最后一条 role=assistant 的
// 消息——不是最后一条消息（run 可能以 tool 结果收尾）——解析 contentJson，按序拼接全部
// type=text 块的 text（中间夹 tool_use 也保持顺序、tool_use 不混入）；无 assistant 或
// 全无 text 块 → 空串
std::string finalAssistantText(const std::vector<AgentMessageRow>& msgs);

// UTF-8 边界安全截断：超长时回退到字符边界再截，不加省略号（调用方决定）
std::string utf8Truncate(const std::string& s, size_t maxBytes);

// 预览文本：decode → 纯文本 → maxBytes 截断 + 「…」
std::string messagePreviewFromJson(const std::string& contentJson,
                                   size_t maxBytes);

// 工具参数摘要：input JSON 压单行 + UTF-8 截断
std::string toolArgsSummary(const std::string& inputJson, size_t maxBytes);

// 展开的单文本块高，真实测量：调 TextPrimitive::measureTextSize，字体不可用（测量全零）时退 1 行
float estimateTextBlockHeight(const std::string& text, float contentW,
                              float fontSize, float lineHeight);

// 单条消息高：collapsed = 头(24) + 预览 1 行；expanded = 头 + Σ 各块高 + 块间距。
// fontSize/lineHeight 必传（13/20 口径）——不再写死 12/18，fontScale 派生值由调用方给
float estimateMessageHeight(const AgentMessageRow& msg, bool expanded,
                            float contentW, float fontSize, float lineHeight);

// 消息区建议外高：clamp(Σ消息高 + 条间 gap, minH, maxH)；真实内容高由 scrollView 自测
float estimateMessagesAreaHeight(const std::vector<AgentMessageRow>& msgs,
                                 const std::set<int64_t>& expandedSeqs,
                                 float contentW, float fontSize,
                                 float lineHeight, float minH, float maxH);

// 消息头行高（§10 头高 24）。tool_use chip 行不复用它——那是普通文本行（lineHeight）
constexpr float kMsgHeaderH = 24.0f;
constexpr float kMsgGap = 4.0f;
// 消息间行距，与 compose 里 msgscroll 的 gap 同源（列内 4、条间 6）
constexpr float kMsgRowGap = 6.0f;

// 角色徽章文案：user=用户 / assistant=助手 / 含 tool_result 块（落库 role 是 "user"）= 工具
std::string messageRoleBadge(const AgentMessageRow& msg);

} // namespace module::agent
