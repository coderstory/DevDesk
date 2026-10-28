// 纯逻辑 JSON-RPC 分发（不碰 socket，可单测）：JSON-RPC 消息 + 会话/版本头 →
// HTTP 结果建议。传输层判序（Content-Type/Origin）由 mcp_http_handler 先做，
// 本类负责会话、版本头、JSON 解析与 JSON-RPC 错误映射。
#pragma once

#include <string>

#include "core/mcp_tool.h"
#include "mcp_session_store.h"

namespace module::mcp {

// 支持的协议版本：主版本 + 兼容版本（spec §5.3）
inline constexpr const char* kMcpProtocolVersion = "2025-06-18";
inline constexpr const char* kMcpProtocolVersionLegacy = "2025-03-26";

struct HttpOutcome {
    int status = 200;              // 建议的 HTTP 状态码
    std::string body;              // 响应体（JSON-RPC 文本；通知为空）
    bool notificationOnly = false; // true = 202 且无 body、不设 Content-Type
};

class McpProtocol {
public:
    McpProtocol(McpSessionStore& sessions, core::mcp::McpToolRegistry& tools,
                std::string serverVersion);

    // newSessionIdOut：仅在 initialize 成功时写入新 session id，由 handler 写响应头；
    // 其余情况保持空串。sessionId/protocolVersionHeader 传空串 = 请求未带对应头。
    HttpOutcome handlePost(const std::string& body,
                           const std::string& sessionId,
                           const std::string& protocolVersionHeader,
                           std::string& newSessionIdOut);

    // DELETE /mcp 的会话语义（§5.2）：已知 → 移除 + 200；未知 → 404；不带 → 200
    HttpOutcome handleDelete(const std::string& sessionId);

private:
    McpSessionStore& sessions_;
    core::mcp::McpToolRegistry& tools_;
    std::string serverVersion_;
};

} // namespace module::mcp