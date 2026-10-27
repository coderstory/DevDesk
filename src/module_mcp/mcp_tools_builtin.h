// 内置 MCP 工具注册。工具持有调用方注入的注册表引用（不读 globalMcpTools），
// 保证测试用局部注册表时与生产全局表互不串味。
#pragma once

#include <functional>

#include "core/mcp_tool.h"
#include "platform/http_client.h"

namespace module::mcp {

using Fetcher = std::function<core::Result<platform::http::Response>(
    const platform::http::Request&)>;

// B1 只注册 devdesk_server_info；web_fetch_markdown 在后续批次追加，
// 本批 fetcher 参数仅保留签名（不参与注册）。
void registerBuiltinMcpTools(core::mcp::McpToolRegistry& registry,
                             Fetcher fetcher = platform::http::send);

} // namespace module::mcp