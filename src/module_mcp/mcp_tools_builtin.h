// 内置 MCP 工具注册。工具持有调用方注入的注册表引用（不读 globalMcpTools），
// 保证测试用局部注册表时与生产全局表互不串味。
#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <vector>

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

// 本地代理探测缝(代理参数批;参照 FetchLimits 先例):probe 为空 = 生产实现
// platform::http::tcpReachable(真 TCP 连通探测,单候选阻塞 ≤ perProbeTimeout);
// 测试注入假探针,不触真端口。
using TcpProbe = std::function<bool(const std::string& hostPort)>;

struct ProxyOptions {
    // 自动探测候选,优先级序(useProxy=true 且未给显式 proxy 时逐个探,首个
    // 可连通者生效;全部落空 → 直连 + 响应附注)
    std::vector<std::string> autoCandidates = {
        "127.0.0.1:7897", "127.0.0.1:10808", "127.0.0.1:51295"};
    std::chrono::milliseconds perProbeTimeout{300};
    TcpProbe probe;
};

void registerBuiltinMcpTools(core::mcp::McpToolRegistry& registry,
                             Fetcher fetcher = platform::http::send,
                             const FetchLimits& limits = FetchLimits{},
                             const ProxyOptions& proxyOptions = ProxyOptions{});

} // namespace module::mcp
