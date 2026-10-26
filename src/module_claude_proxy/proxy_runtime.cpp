// src/module_claude_proxy/proxy_runtime.cpp
// 代理运行时实现：本地 HTTP 代理 server/路由生命周期 + 在途流登记 + 端口镜像
// + 终态事件单槽。停机序列 = cancelActiveStreams → server->stop → 状态 reset。
#include "proxy_runtime.h"

#include "claude_service.h"
#include "core/logging.h"
#include "platform/http_server.h"
#include "proxy_router.h"

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <random>
#include <sstream>
#include <utility>

namespace module::claude {

// 构造 / 析构

ProxyRuntime::ProxyRuntime(ClaudeService& service) : service_(service) {}

// 析构时确保停止代理（Server 析构会 join 线程）：与 stop() 同款停机序列——
// 先取消在途流，否则 join 被阻塞在 readChunk 的 worker 卡住
ProxyRuntime::~ProxyRuntime() {
    stop();
}

// 代理 server 生命周期

const std::string& claudeUpstreamSessionId() {
    // 32 hex（同 auth token 的生成方式）；magic static 保证线程安全与进程内唯一
    static const std::string id = [] {
        std::random_device rd;
        std::uint32_t words[4];
        for (auto& w : words) w = rd();
        std::ostringstream oss;
        for (const auto w : words) {
            oss << std::hex << std::setw(8) << std::setfill('0') << w;
        }
        return oss.str();
    }();
    return id;
}

core::Result<int> ProxyRuntime::startProxy(const std::string& bindAddress,
                                           int port) {
    if (server_) {
        return core::Result<int>::fail(core::ErrorCode::AlreadyExists,
            "代理 server 已在运行（先 stopProxy）");
    }

    // 上一轮 stop() 置位的停止标志清零（否则 startProxy 后新登记到
    // router 的流一进来就被误取消）
    {
        std::lock_guard<std::mutex> lock(streamsMutex_);
        streamsStopping_ = false;
        activeStreams_.clear();
    }

    // 构造 Server（绑定 127.0.0.1，仅本机可访问，无鉴权）
    server_ = std::make_unique<platform::http::Server>(bindAddress, port);
    // 02：应用层 Host 白名单——按实际 bind 地址推导（回环 bind → 三种回环形态），
    // 不写死；本机其它页面伪造 Host 借道访问一律 403。连接级约束（仅 bind 回环
    // 可连）不受影响，这是在它之上的应用层校验。
    server_->setAllowedHosts(platform::http::hostWhitelistForBind(bindAddress));

    // 构造并注册路由（ProxyRouter 契约持 ClaudeService&——门面引用经构造
    // 注入，本类自身不调用门面方法）
    router_ = std::make_unique<ProxyRouter>(service_, *server_);
    router_->registerRoutes();

    if (!server_->start()) {
        const int bound = server_->actualPort();
        server_.reset();
        router_.reset();
        return core::Result<int>::fail(core::ErrorCode::Internal,
            "代理 server 启动失败 (port=" + std::to_string(bound) + ")");
    }

    const int actual = server_->actualPort();
    LOG_INFO("Claude 代理已启动 " + bindAddress + ":" + std::to_string(actual) +
             "（仅本机可访问，无鉴权）");
    return core::Result<int>::ok(actual);
}

void ProxyRuntime::stop() {
    if (!server_) return;
    LOG_INFO("停止 Claude 代理");
    // 先取消在途流式上游，再停 server：server_->stop() 内部要 join httplib
    // worker，而在途流的 worker 阻塞在 readChunk 上——不取消就要等流自然结束
    //（活动生成期无空闲超时，停止/退出会挂起成幽灵进程）。
    cancelActiveStreams();
    server_->stop();
    server_.reset();
    router_.reset();
}

bool ProxyRuntime::isProxyRunning() const {
    return server_ && server_->isRunning();
}

int ProxyRuntime::proxyPort() const {
    return server_ ? server_->actualPort() : 0;
}

void ProxyRuntime::setConfiguredProxyPort(int port) {
    // 钳制到合法范围（非法输入保守回落默认值：0/负数/越界都是配置错误，
    // 拿字面值 bind 必然失败，不如统一收敛）
    if (port < ::core::kMinProxyPort || port > ::core::kMaxProxyPort) {
        LOG_WARN("代理端口配置非法 (" + std::to_string(port) +
                 ")，回落默认 " + std::to_string(::core::kDefaultProxyPort));
        port = ::core::kDefaultProxyPort;
    }
    configuredProxyPort_ = port;
}

// 在途流式上游登记

void ProxyRuntime::registerActiveStream(
    const std::shared_ptr<platform::http::StreamingResponse>& stream) {
    if (!stream) return;
    std::lock_guard<std::mutex> lock(streamsMutex_);
    // 顺手清掉已结束的条目（weak_ptr 过期 = provider/releaser 已放手），
    // 防止长期运行下 vector 单调增长
    std::erase_if(activeStreams_,
                  [](const std::weak_ptr<platform::http::StreamingResponse>& w) {
                      return w.expired();
                  });
    activeStreams_.emplace_back(stream);
    // stop() 已发起（cancelActiveStreams 跑完后 handler 才登记进来）：
    // 立即取消，消掉「取消与登记赛跑」的窗口，handler 快速走 Error 路径
    if (streamsStopping_) {
        stream->cancelWithReason("proxy stopping");
    }
}

void ProxyRuntime::cancelActiveStreams() {
    std::vector<std::shared_ptr<platform::http::StreamingResponse>> live;
    {
        std::lock_guard<std::mutex> lock(streamsMutex_);
        streamsStopping_ = true;
        live.reserve(activeStreams_.size());
        for (auto& w : activeStreams_) {
            if (auto sp = w.lock()) live.push_back(std::move(sp));
        }
        activeStreams_.clear();
    }
    // 锁外逐个 cancel：只置标志 + notify_all，阻塞在 readChunk 的 worker
    // 线程立即退出 provider 循环 → Server::stop() 的 join 不再等流结束
    for (auto& sp : live) {
        sp->cancelWithReason("proxy stopping");
    }
}

// 最近代理请求事件单槽（mailbox）

void ProxyRuntime::recordProxyEvent(int statusCode) {
    std::lock_guard<std::mutex> lock(proxyEventMutex_);
    proxyEventStatus_ = statusCode;
    proxyEventAt_ = std::time(nullptr);
}

std::optional<int> ProxyRuntime::takeProxyEvent(std::time_t nowSec) {
    std::lock_guard<std::mutex> lock(proxyEventMutex_);
    if (!proxyEventStatus_.has_value()) {
        return std::nullopt;
    }
    const int status = *proxyEventStatus_;
    const std::time_t at = proxyEventAt_;
    // 读走即清空（take 语义）：无论走哪个返回分支，槽都已消费
    proxyEventStatus_.reset();
    proxyEventAt_ = 0;
    // 过期滞留事件丢弃（桌宠关了很久的旧闻，不是「刚才发生了什么」）；
    // 时钟回摆（at > nowSec）按未过期处理——宁可多弹一次，不误吞事件
    if (at > 0 && nowSec >= at && nowSec - at > kProxyEventFreshSec) {
        return std::nullopt;
    }
    return status;
}

} // namespace module::claude