// src/module_claude_proxy/proxy_runtime.h
// 代理运行时：HTTP 代理 server 生命周期 + 在途流登记 + 端口镜像 + 终态事件邮箱。
// 停机顺序不可变：cancelActiveStreams → server->stop（否则退出留幽灵进程）；stop() 幂等。
// worker 线程写登记表 / UI 线程发起 cancel，streamsMutex_ 保护。
// 持有 ClaudeService& 仅用于构造 ProxyRouter，运行时自身不调门面。
#pragma once

#include <ctime>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/proxy_port.h"
#include "core/result.h"
#include "platform/http_client.h"

namespace platform::http { class Server; }

namespace module::claude {

class ClaudeService;
class ProxyRouter;

// claudeUpstreamSessionId() 声明在 claude_service.h，定义在 .cpp 的
// 「代理 server 生命周期」段（上游会话标识属代理运行时域）

// 门面 ClaudeService 编排（起代理后接管 / 停代理后还原），本类只管生命周期。
class ProxyRuntime {
public:
    // service 仅作为 ProxyRouter 的宿主引用传递（见文件头注释），本类不调用它
    explicit ProxyRuntime(ClaudeService& service);
    ~ProxyRuntime();

    ProxyRuntime(const ProxyRuntime&) = delete;
    ProxyRuntime& operator=(const ProxyRuntime&) = delete;

    // 启动本地 HTTP 代理（固定绑定 127.0.0.1，仅本机可访问，无鉴权）。
    // port 0 = 由 OS 分配，默认值取 core/proxy_port.h。不含 settings.json
    // 接管（接管尾在门面 startProxy，顺序：启动成功 → 接管）
    core::Result<int> startProxy(const std::string& bindAddress,
                                 int port = ::core::kDefaultProxyPort);

    // 停机顺序不变式：cancelActiveStreams → server_->stop → 状态 reset
    //（否则 httplib worker join 阻塞在 provider 的 readChunk，停止/退出挂起）；
    // 幂等
    void stop();

    bool isProxyRunning() const;
    int proxyPort() const;  // 0 if not running

    // 设置页「代理端口」的运行时镜像；setter 钳制到
    // [kMinProxyPort, kMaxProxyPort]（取值见 core/proxy_port.h）
    int configuredProxyPort() const { return configuredProxyPort_; }
    void setConfiguredProxyPort(int port);

    // ---- 在途流式上游登记 ----
    // ProxyRouter 启动上游流后立即 registerActiveStream；stop() / 析构先调
    // cancelActiveStreams() 再停 server——否则 httplib worker 阻塞在
    // readChunk，Server::stop() 的 join 要等流自然结束（Claude 活动生成期
    // 无空闲超时，停止/退出挂起成幽灵进程）
    void registerActiveStream(
        const std::shared_ptr<platform::http::StreamingResponse>& stream);
    void cancelActiveStreams();

    // ---- 最近代理请求事件（桌宠气泡内容源）----
    // 每条请求终态记一次 statusCode 进单槽（后写覆盖先写）：0 = 上游不可达；
    // 其余为回给客户端的状态码。takeProxyEvent() 读走即清（take 语义，一个
    // 事件最多弹一次）；滞留超 kProxyEventFreshSec 作废丢弃（窗口 10min，
    // 覆盖气泡 roll 的典型间隔）。写方 = proxy worker、读方 = UI 线程，mutex 保护。
    static constexpr std::time_t kProxyEventFreshSec = 600;
    void recordProxyEvent(int statusCode);
    // nowSec 默认取当前墙钟（测试可显式传值验证过期分支）
    std::optional<int> takeProxyEvent(std::time_t nowSec = std::time(nullptr));

private:
    ClaudeService& service_;  // 仅作 ProxyRouter 宿主引用（见文件头注释）

    // 代理 server 资源（延迟构造）
    std::unique_ptr<platform::http::Server> server_;
    std::unique_ptr<ProxyRouter> router_;

    // 在途流式上游登记。streamsStopping_：stop/析构已发起，期间新登记的流
    // 立即被取消（消掉「cancel 跑完才有流登记进来」的窗口）
    std::mutex streamsMutex_;
    std::vector<std::weak_ptr<platform::http::StreamingResponse>> activeStreams_;
    bool streamsStopping_ = false;

    // 设置页配置的代理端口（默认 = core/proxy_port.h 的默认端口）
    int configuredProxyPort_ = ::core::kDefaultProxyPort;

    // 最近代理请求事件单槽：proxy worker 写 / UI take 读
    std::mutex proxyEventMutex_;
    std::optional<int> proxyEventStatus_;
    std::time_t proxyEventAt_ = 0;
};

} // namespace module::claude