// src/core/llm_endpoint.h
// LLM 请求的抽象端点：agent loop 依赖此抽象，LlmClient（module_agent 内）
// 实现为「经本地代理」形态。测试可注入 mock LlmEndpoint 验证 agent loop
// 逻辑，不发真实网络请求。
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/result.h"

namespace core {

// Anthropic messages API 的一条消息（role + content blocks JSON 原文）
struct LlmMessage {
    std::string role;           // "user" / "assistant"
    std::string contentJson;    // content blocks 的 JSON 数组原文
};

struct LlmResponse {
    std::string stopReason;     // "end_turn" / "tool_use" / "max_tokens" ...
    std::string contentJson;    // 响应 content blocks 原文（含 tool_use blocks）
    int64_t inputTokens = -1;   // -1 = 未采集
    int64_t outputTokens = -1;
};

class LlmEndpoint {
public:
    virtual ~LlmEndpoint() = default;
    // 发一次 /v1/messages 请求。messages 为完整对话历史（agent loop 累积），
    // toolsJson 为 Anthropic tools 定义数组。systemPrompt 可空。
    // model 参数由调用方（LlmClient）经 IClaudeProxyService::resolveModelSlot
    // 解析后传入（见下方 H1）——本接口只发，不解析槽位。
    virtual Result<LlmResponse> send(
        const std::vector<LlmMessage>& messages,
        const std::string& toolsJson,
        const std::string& systemPrompt,
        const std::string& model) = 0;
};

} // namespace core
