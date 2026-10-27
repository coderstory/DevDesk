// 内置 MCP 工具注册。工具持有调用方注入的注册表引用（不读 globalMcpTools），
// 保证测试用局部注册表时与生产全局表互不串味。
#pragma once

#include <chrono>
#include <functional>

#include "core/mcp_tool.h"
#include "platform/http_client.h"

namespace module::mcp {

using Fetcher = std::function<core::Result<platform::http::Response>(
    const platform::http::Request&)>;

// B2 抓取限额:默认 = spec §6.1 生产值(总时限 10s、重定向 ≤5 跳);测试以更小
// 预算注入超时分支,生产路径不感知此参数。
struct FetchLimits {
    std::chrono::milliseconds totalBudget{10000};
    int maxRedirectHops = 5;
};

void registerBuiltinMcpTools(core::mcp::McpToolRegistry& registry,
                             Fetcher fetcher = platform::http::send,
                             const FetchLimits& limits = FetchLimits{});

} // namespace module::mcp
