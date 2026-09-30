// src/module_agent/llm_client.h
// LlmEndpoint 的生产实现（agent M1 Task 7）：经本地代理的 HTTP 端口发
// /v1/messages。链路 = spec §1 步骤 3：预检代理运行状态 → 解析模型槽位 →
// 组装 Anthropic 请求体 → 发送 → 状态码检查 → 解析响应。
// HTTP 发送经 HttpFn 可注入（测试传假发送，零真实网络）；生产传 nullptr，
// 内部走 platform::http::send。
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

    // send 的 model 形参语义 = **槽位键名**（AgentLoop 原样传 task.modelSlot，
    // 不在 loop 里解析）：本函数内 proxy_.resolveModelSlot(model) 取实际
    // model id 再进请求体（H1 定案）。
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
