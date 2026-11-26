// 纯逻辑 JSON-RPC 分发（不碰 socket，可单测）：JSON-RPC 消息 + 会话/版本头 →
// HTTP 结果建议。传输层判序（Content-Type/Origin）由 mcp_http_handler 先做，
// 本类负责会话、版本头、JSON 解析与 JSON-RPC 错误映射。
#pragma once

#include <atomic>
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

    // 页头「破坏性工具」开关的实时值；默认 false = 破坏性工具一律拒。
    // 原子：UI 线程写，HTTP worker 读。
    void setAllowDestructive(bool allow);
    bool allowDestructive() const;

private:
    McpSessionStore& sessions_;
    core::mcp::McpToolRegistry& tools_;
    std::string serverVersion_;
    std::atomic<bool> allowDestructive_{false};
};

// need301②（原 bug#28 观测补强）：外层总闸超时后弃置（detach）的 worker
// 累计数。进程级单调原子计数，仅供观测/测试断言；detach 线程不可回收。
[[nodiscard]] int abandonedToolWorkers();

// 在途工具调用数（已占并发槽的 tools/call worker，含超时 detach 后仍在
// 跑的那些）。进程级观测：占槽/归还都由 worker 生命周期驱动，不提供重置。
[[nodiscard]] int inflightToolCalls();

} // namespace module::mcp