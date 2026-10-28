#include "mcp_service.h"

#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "platform/http_server.h"

namespace module::mcp {

namespace {

#ifdef DEVDESK_VERSION
constexpr const char* kServerVersion = DEVDESK_VERSION;
#else
constexpr const char* kServerVersion = "dev";
#endif

constexpr const char* kBindAddress = "127.0.0.1";

} // namespace

McpService::McpService(core::mcp::McpToolRegistry& tools) : tools_(tools) {}

McpService::~McpService() { stop(); }

core::Result<int> McpService::start(int requestedPort) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == ServiceState::Running && server_) {
        return core::Result<int>::ok(port_);
    }

    // 每次 start 构造新的 Server：换端口必须换监听器。局部对象按声明逆序析构
    //（server 最后销毁），失败路径也不会让 server 先于 handler/protocol 消失。
    auto protocol =
        std::make_unique<McpProtocol>(sessions_, tools_, kServerVersion);
    auto handler = std::make_unique<McpHttpHandler>(*protocol);
    auto server =
        std::make_unique<platform::http::Server>(kBindAddress, requestedPort);
    // Host 白名单跟着实际绑定地址走（回环绑定 → 三种回环拼写放行）
    server->setAllowedHosts(platform::http::hostWhitelistForBind(kBindAddress));
    handler->registerRoutes(*server);

    if (!server->start()) {
        // Server::start() 只返回 bool，没有失败细粒度原因
        lastError_ = "端口 " + std::to_string(requestedPort) +
                     " 被占用或无法监听";
        state_ = ServiceState::Failed;
        port_ = 0;
        return core::Result<int>::fail(core::ErrorCode::Internal, lastError_);
    }

    protocol_ = std::move(protocol);
    handler_ = std::move(handler);
    server_ = std::move(server);
    port_ = server_->actualPort();  // port 0 → OS 分配的临时端口
    state_ = ServiceState::Running;
    lastError_.clear();
    return core::Result<int>::ok(port_);
}

void McpService::stop() {
    std::unique_ptr<McpProtocol> protocol;
    std::unique_ptr<McpHttpHandler> handler;
    std::unique_ptr<platform::http::Server> server;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        server = std::move(server_);
        handler = std::move(handler_);
        protocol = std::move(protocol_);
        sessions_.clear();  // 停止/重启后旧 session id 一律 404
        port_ = 0;
        state_ = ServiceState::Stopped;
        lastError_.clear();
    }
    // join 必须在锁外：在途工具调用最多阻塞到自身限时（抓取工具总时限 10s），
    // 持锁 join 会让 status()/isRunning() 整段冻结。
    if (server) server->stop();
    server.reset();
    handler.reset();
    protocol.reset();
}

core::Result<int> McpService::restart(int requestedPort) {
    stop();
    return start(requestedPort);
}

bool McpService::isRunning() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == ServiceState::Running && server_ != nullptr;
}

int McpService::port() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return port_;
}

ServiceStatus McpService::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ServiceStatus{state_, port_, lastError_};
}

McpService& globalMcpService() {
    // 永不析构持有：atexit 里的 stop() 仍需访问本对象（同 core::ToolRegistry）
    static auto& s = *new McpService();
    return s;
}

} // namespace module::mcp