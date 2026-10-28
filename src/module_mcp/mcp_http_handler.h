// MCP HTTP 适配器：把 platform::http 的 RequestCtx/ResponseWriter 接到
// McpProtocol，并做传输层判序（Content-Type / Origin / 方法级拒绝）。
#pragma once

#include "mcp_protocol.h"

namespace platform::http {
class Server;
struct RequestCtx;
class ResponseWriter;
} // namespace platform::http

namespace module::mcp {

class McpHttpHandler {
public:
    explicit McpHttpHandler(McpProtocol& protocol);

    // POST/GET/DELETE/OPTIONS "/mcp"
    void registerRoutes(platform::http::Server& server);

private:
    void handlePost(const platform::http::RequestCtx& ctx,
                    platform::http::ResponseWriter& writer);
    void handleDelete(const platform::http::RequestCtx& ctx,
                      platform::http::ResponseWriter& writer);
    void handleMethodNotAllowed(platform::http::ResponseWriter& writer);

    McpProtocol& protocol_;
};

} // namespace module::mcp