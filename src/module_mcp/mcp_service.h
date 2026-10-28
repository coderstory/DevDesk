// MCP 服务生命周期：只绑定 127.0.0.1，Host 白名单由绑定地址推导；每次 start
// 新建监听器（换端口必须换监听器），stop 幂等并清空会话表。
#pragma once

#include <memory>
#include <mutex>
#include <string>

#include "core/mcp_tool.h"
#include "mcp_http_handler.h"
#include "mcp_protocol.h"
#include "mcp_session_store.h"

namespace platform::http {
class Server;
} // namespace platform::http

namespace module::mcp {

enum class ServiceState { Stopped, Running, Failed };

struct ServiceStatus {
    ServiceState state = ServiceState::Stopped;
    int port = 0;                 // 运行中 = 实际监听端口；其余状态 0
    std::string lastError;        // Failed 时非空；stop 成功时清空
};

class McpService {
public:
    // 构造注入注册表：生产 = 全局注册表，测试 = 局部注册表（工具互不串味）
    explicit McpService(
        core::mcp::McpToolRegistry& tools = core::mcp::globalMcpTools());
    ~McpService();
    McpService(const McpService&) = delete;
    McpService& operator=(const McpService&) = delete;

    // 0 = 临时端口（测试用）；已运行 = 幂等返回当前端口
    core::Result<int> start(int port);
    // 幂等；清空会话表。join 在锁外执行（在途工具调用按自身限时收敛）
    void stop();
    core::Result<int> restart(int port);
    bool isRunning() const;
    int port() const;
    ServiceStatus status() const;  // 加锁快照

private:
    core::mcp::McpToolRegistry& tools_;
    McpSessionStore sessions_;

    mutable std::mutex mutex_;
    ServiceState state_ = ServiceState::Stopped;
    int port_ = 0;
    std::string lastError_;

    // 声明顺序 = 依赖顺序：server 的 handler 引用 protocol，销毁必须 server 先走
    std::unique_ptr<McpProtocol> protocol_;
    std::unique_ptr<McpHttpHandler> handler_;
    std::unique_ptr<platform::http::Server> server_;
};

McpService& globalMcpService();

} // namespace module::mcp