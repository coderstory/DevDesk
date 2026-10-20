// 联网抓取工具 "net.fetch"：GET 一个 http/https URL，返回响应文本。
//
// 行为契约：仅 GET、仅 http/https、10 秒超时；响应文本按 32KB 截断（对齐
// AgentLoop 的 kMaxToolResultBytes 上下文护栏——本工具在源头自截并计提示前缀/尾注
// 预算，保证喂回 LLM 的最终文本不超限）；Content-Type 非文本家族时前缀
// 「二进制/非文本」提示。失败（超时/DNS/非 2xx）返回可读中文错误文本
// （isError=true），不抛异常（全仓铁律，走 Result）。
//
// 安全注记：外部请求，仅 GET，10s 超时；不做重放/并发限制——单次调用语义，
// 调用频度由 AgentLoop 的迭代上限承担。
//
// 线程契约：execute() 在 AgentLoop 工作线程执行；fetcher 由构造注入（单测用
// fake / 本地 HttpServer，绝不真外网），默认实现转发 platform::http::send。
#pragma once

#include <functional>

#include "core/result.h"
#include "core/tool.h"
#include "platform/http_client.h"

namespace module::agent {

// 生产默认超时（毫秒）：外部请求 10 秒
inline constexpr long kNetFetchDefaultTimeoutMs = 10 * 1000;

// 抓取函数：与 platform::http::send 同形（默认实现即转发它）
using NetFetcher = std::function<core::Result<platform::http::Response>(
    const platform::http::Request&)>;

core::Result<platform::http::Response> defaultNetFetcher(
    const platform::http::Request& request);

class NetFetchTool : public core::ITool {
public:
    explicit NetFetchTool(
        NetFetcher fetcher = defaultNetFetcher,
        long timeoutMs = kNetFetchDefaultTimeoutMs);

    std::string name() const override { return "net.fetch"; }
    std::string schemaJson() const override;
    core::Result<core::ToolResult> execute(const core::ToolArgs& args) override;

private:
    NetFetcher fetcher_;
    long timeoutMs_;
};

} // namespace module::agent
