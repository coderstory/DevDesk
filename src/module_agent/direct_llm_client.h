// LlmEndpoint 生产实现：不经本地代理，按当前生效 profile 直发 POST {baseUrl}/v1/messages。
// 链路 = 取 profile → 解析槽位 → 组请求体 → 注入鉴权头 → 发送 → 状态码检查 → 解析响应。
// 与代理解耦：本类只依赖 core::LlmProfileSource（配置数据），代理未启动 / 未装配一律不影响。
// HTTP 发送经 HttpFn 可注入（单测零真实网络）。
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "core/llm_endpoint.h"
#include "core/llm_profile_source.h"
#include "core/result.h"
#include "platform/http_client.h"

namespace module::agent {

class DirectLlmClient final : public core::LlmEndpoint {
public:
    // HTTP 发送缝（nullptr = 生产路径 platform::http::send）
    using HttpFn = std::function<core::Result<platform::http::Response>(
        const platform::http::Request&)>;
    explicit DirectLlmClient(core::LlmProfileSource& profiles,
                             HttpFn http = nullptr);

    // model 形参语义 = 槽位键名（AgentLoop 原样传 task.modelSlot），本函数内自解析成实际 model id
    core::Result<core::LlmResponse> send(
        const std::vector<core::LlmMessage>& messages,
        const std::string& toolsJson,
        const std::string& systemPrompt,
        const std::string& model) override;

private:
    core::LlmProfileSource& profiles_;
    HttpFn http_;
};

} // namespace module::agent
