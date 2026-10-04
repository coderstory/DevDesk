// LlmEndpoint 生产实现：经本地代理 HTTP 端口发 /v1/messages。链路 = 预检代理 →
// 解析槽位 → 组请求体 → 发送 → 状态码检查 → 解析响应。HTTP 发送经 HttpFn 可注入
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "core/claude_proxy_service.h"
#include "core/llm_endpoint.h"
#include "core/result.h"
#include "platform/http_client.h"

namespace module::agent {

class LlmClient final : public core::LlmEndpoint {
public:
    // HTTP 发送缝（nullptr = 生产路径 platform::http::send）
    using HttpFn = std::function<core::Result<platform::http::Response>(
        const platform::http::Request&)>;
    explicit LlmClient(core::IClaudeProxyService& proxy, HttpFn http = nullptr);

    // model 形参语义 = 槽位键名（AgentLoop 原样传 task.modelSlot），本函数内 resolveModelSlot 取实际 id
    core::Result<core::LlmResponse> send(
        const std::vector<core::LlmMessage>& messages,
        const std::string& toolsJson,
        const std::string& systemPrompt,
        const std::string& model) override;

private:
    core::IClaudeProxyService& proxy_;
    HttpFn http_;
};

} // namespace module::agent
