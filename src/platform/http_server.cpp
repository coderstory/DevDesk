// src/platform/http_server.cpp
// 基于 cpp-httplib 的本地 HTTP 服务器。
// 线程模型：std::thread 里跑 httplib listen 循环，stop() 时 join；请求由 cpp-httplib 内部线程池处理。
// 绑定地址由调用方给定（本地代理固定 127.0.0.1，仅本机可访问，无鉴权）。
// 应用层 Host 头白名单（setAllowedHosts，默认不启用）：配置后仅放行 host 部分
// 命中白名单的请求、其余 403，防本机其它页面伪造 Host 借道访问。

#include "http_server.h"
#include "http_headers.h"

#include <core/logging.h>

#include <httplib.h>

#include <atomic>
#include <cctype>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

namespace platform::http {

namespace {

// cpp-httplib Headers（大小写不敏感 multimap）→ std::map：键统一小写，同名多条保留首个 value。
std::map<std::string, std::string> toHeaderMap(const httplib::Headers& src) {
    return toLowerKeyHeaderMap(src);
}

// 把 RouteHandler 适配成 httplib::Server::Handler
httplib::Server::Handler toLibHandler(RouteHandler userHandler) {
    return [userHandler = std::move(userHandler)](
               const httplib::Request& req, httplib::Response& res) {
        ResponseWriter rw;
        RequestCtx rctx;
        rctx.method = req.method;
        rctx.path = req.path;
        rctx.headers = toHeaderMap(req.headers);
        rctx.body = req.body;
        rctx.clientAddress = req.remote_addr;
        rctx.isConnectionClosed = req.is_connection_closed;

        userHandler(rctx, rw);

        // 把 ResponseWriter 的状态写回 cpp-httplib Response
        res.status = rw.status();

        // 流式路径：ResponseWriter 设了 chunk provider → 用 chunked 传输编码流式回写。provider 由
        // cpp-httplib 在 worker 线程反复调用，桥接到用户的 ChunkProvider（内部从 queue 拉数据、实现背压）。
        if (rw.isStreaming()) {
            for (const auto& kv : rw.headers()) {
                res.set_header(kv.first, kv.second);
            }

            auto provider = rw.takeChunkProvider();
            auto releaser = rw.takeChunkReleaser();
            // 捕获 is_connection_closed（内部引用 strm，仅在本请求处理期间有效）
            auto connClosed = req.is_connection_closed;

            res.set_chunked_content_provider(
                rw.streamingContentType(),
                [provider, connClosed](size_t /*offset*/, httplib::DataSink& sink) -> bool {
                    auto writeChunk = [&sink](const char* data, size_t len) -> bool {
                        return sink.write(data, len);
                    };
                    auto isWritable = [&sink, connClosed]() -> bool {
                        if (connClosed && connClosed()) return false;
                        if (sink.is_writable && !sink.is_writable()) return false;
                        return true;
                    };
                    const bool more = provider(writeChunk, isWritable);
                    if (!more) {
                        // provider 返回 false 会被 cpp-httplib 当 Canceled（客户端看到截断）；
                        // sink.done() 写 "0\r\n\r\n" 正常收尾，再返回 true 让写循环退出。
                        sink.done();
                    }
                    return true;
                },
                [releaser](bool success) {
                    if (releaser) releaser(success);
                });
            return;
        }

        for (const auto& kv : rw.headers()) {
            res.set_header(kv.first, kv.second);
        }
        if (!rw.body().empty()) {
            // 头名大小写不敏感：消费方可能写 "content-type" 或 "Content-Type"，都要命中；否则 body 被标成 text/plain
            if (const char* ctype = findHeaderCI(rw.headers(), "content-type")) {
                res.set_content(rw.body(), ctype);
            } else {
                res.set_content(rw.body(), "text/plain; charset=utf-8");
            }
        }
    };
}

// Host 头值 / 白名单条目 → 可比较的 host 键：剥首尾空白 + 剥可选端口 + 小写 +
// 剥 IPv6 方括号。"[::1]:8080" / "[::1]" / "::1" → "::1"；"127.0.0.1:8080" →
// "127.0.0.1"；"LOCALhost" → "localhost"。端口剥除只认两种形态：方括号地址截到
// ']'；其余「恰有一个 ':'」的 host:port。IPv6 裸字面量（"::1"）自带多个 ':'，
// 按 RFC 3986 不带端口（带端口必须加方括号），不能按第一个 ':' 截。残缺输入
// 返回空串（空键永不命中白名单：条目侧丢弃空键、Host 侧空键不匹配，fail closed）。
std::string normalizeHostPart(const std::string& raw) {
    std::string s = raw;
    while (!s.empty() &&
           std::isspace(static_cast<unsigned char>(s.front()))) {
        s.erase(s.begin());
    }
    while (!s.empty() &&
           std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
    if (!s.empty() && s.front() == '[') {
        const auto close = s.find(']');
        if (close == std::string::npos) return {};
        s = s.substr(0, close + 1);
    } else {
        // 恰有一个 ':' 才是 host:port（剥端口）；裸 IPv6（"::1"）自带多个 ':'，
        // 不能按第一个 ':' 截——那会把 "::1" 截成空串（bind 推导因此返回空表）。
        const auto first = s.find(':');
        if (first != std::string::npos && first == s.rfind(':')) {
            s = s.substr(0, first);
        }
    }
    for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (s.size() >= 2 && s.front() == '[' && s.back() == ']') {
        s = s.substr(1, s.size() - 2);
    }
    return s;
}

} // namespace

// Host 白名单（02-proxy-host-validation）

std::vector<std::string> hostWhitelistForBind(const std::string& bindAddress) {
    const std::string bind = normalizeHostPart(bindAddress);
    // 回环绑定（三种回环名的任一拼写/大小写）→ 三种回环形态都放行：本机客户端
    // 可能用任一名字访问同一个监听。条目与 Host 头都经 normalizeHostPart 归一
    //（剥端口/剥方括号/小写），所以 "[::1]" 条目同时覆盖裸 "::1" 拼写。
    if (bind == "127.0.0.1" || bind == "::1" || bind == "localhost") {
        return {"127.0.0.1", "localhost", "[::1]"};
    }
    // 不可解析的绑定地址 → 不配置（空 = 放行）：这种地址后续 bind 也会失败，
    // 不值得为它引入「起了服务但全 403」的半死状态。
    if (bind.empty()) return {};
    // 非回环绑定 → 只放行监听地址本身：白名单跟着实际 bind 走，不写死回环集。
    return {bind};
}

// ResponseWriter 实现

void ResponseWriter::setStatus(int code) { status_ = code; }
void ResponseWriter::setHeader(const std::string& key,
                               const std::string& value) {
    headers_[key] = value;
}
void ResponseWriter::write(const std::string& data) {
    body_ += data;
}
// 空操作：响应在 handler 返回时由适配层统一提交（见 http_server.h 注释）。
void ResponseWriter::end() {}

void ResponseWriter::setChunkProvider(const std::string& contentType,
                                      ChunkProvider provider,
                                      ChunkReleaser releaser) {
    streaming_ = true;
    streamingContentType_ = contentType;
    chunkProvider_ = std::move(provider);
    chunkReleaser_ = std::move(releaser);
}

// Server::Impl

struct Server::Impl {
    httplib::Server svr;
    std::thread worker;
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> listenStarted{false};
    std::mutex routeMutex;
    // Host 白名单（setAllowedHosts 写入归一后的条目）：空 = 未配置 = 放行（向后
    // 兼容，取舍见 http_server.h）。pre-routing 钩子在 httplib worker 线程读、
    // setAllowedHosts 在调用方线程写 → mutex 保护。
    std::mutex allowedHostsMutex;
    std::vector<std::string> allowedHosts;
};

// Server 实现

// Host 白名单：归一化后存 Impl（空 = 未配置 = 放行），pre-routing 钩子按表匹配
void Server::setAllowedHosts(std::vector<std::string> hosts) {
    std::vector<std::string> normalized;
    normalized.reserve(hosts.size());
    for (auto& h : hosts) {
        std::string key = normalizeHostPart(h);
        if (!key.empty()) normalized.push_back(std::move(key));
    }
    std::lock_guard<std::mutex> lock(impl_->allowedHostsMutex);
    impl_->allowedHosts = std::move(normalized);
}

Server::Server(std::string bindAddress, int port)
    : bindAddress_(std::move(bindAddress)),
      port_(port),
      impl_(new Impl()) {
    // 请求体上限：所有路由单点收口，超出由 cpp-httplib 回 413
    impl_->svr.set_payload_max_length(kMaxBodyBytes);

    // Host 白名单 pre-routing 钩子：构造期就装（无论是否已配置白名单），之后
    // setAllowedHosts 只换数据不动钩子——避免「先 start 后 set」出现钩子缺失的
    // 窗口。空表 = 未配置 = 放行所有 Host（向后兼容，见 http_server.h 的取舍）。
    impl_->svr.set_pre_routing_handler(
        [this](const httplib::Request& req, httplib::Response& res) {
            std::vector<std::string> allowed;
            {
                std::lock_guard<std::mutex> lock(impl_->allowedHostsMutex);
                if (impl_->allowedHosts.empty()) {
                    return httplib::Server::HandlerResponse::Unhandled;
                }
                allowed = impl_->allowedHosts;
            }
            // fail closed：配置了白名单就不接待无 Host / Host 不可解析的请求
            //（HTTP/1.1 起必备 Host；缺失只可能是畸形请求或老 HTTP/1.0 客户端）
            const std::string host = req.get_header_value("Host");
            const std::string key = normalizeHostPart(host);
            for (const auto& entry : allowed) {
                if (entry == key) {
                    return httplib::Server::HandlerResponse::Unhandled;
                }
            }
            LOG_WARN("HTTP server 拒绝 Host 不在白名单: \"" + host + "\" (" +
                     req.remote_addr + " " + req.method + " " + req.path + ")");
            res.status = 403;
            res.set_content("Forbidden: Host not allowed",
                            "text/plain; charset=utf-8");
            return httplib::Server::HandlerResponse::Handled;
        });
}

Server::~Server() {
    stop();
    delete impl_;
}

void Server::route(const std::string& method, const std::string& path,
                   RouteHandler handler) {
    std::lock_guard<std::mutex> lock(impl_->routeMutex);

    auto libHandler = toLibHandler(std::move(handler));

    // method 大小写不敏感
    std::string m = method;
    for (auto& c : m) c = static_cast<char>(std::toupper(c));

    if (m == "GET") {
        impl_->svr.Get(path, libHandler);
    } else if (m == "POST") {
        impl_->svr.Post(path, libHandler);
    } else if (m == "PUT") {
        impl_->svr.Put(path, libHandler);
    } else if (m == "DELETE") {
        impl_->svr.Delete(path, libHandler);
    } else if (m == "PATCH") {
        impl_->svr.Patch(path, libHandler);
    } else if (m == "OPTIONS") {
        impl_->svr.Options(path, libHandler);
    } else {
        LOG_ERROR("Server::route unsupported method: " + method);
    }
}

bool Server::start() {
    if (running_) return true;
    if (impl_->listenStarted.load()) return true;

    int boundPort = port_;
    if (port_ == 0) {
        boundPort = impl_->svr.bind_to_any_port(bindAddress_);
        if (boundPort <= 0) {
            LOG_ERROR("HTTP server bind_to_any_port failed on " + bindAddress_);
            return false;
        }
    } else {
        if (!impl_->svr.bind_to_port(bindAddress_, port_)) {
            LOG_ERROR("HTTP server bind_to_port failed on " + bindAddress_ +
                      ":" + std::to_string(port_));
            return false;
        }
    }
    port_ = boundPort;
    impl_->listenStarted.store(true);

    LOG_INFO("HTTP server listening on " + bindAddress_ + ":" +
             std::to_string(port_));

    impl_->worker = std::thread([this]() {
        impl_->svr.listen_after_bind();
    });

    impl_->svr.wait_until_ready();
    running_ = true;
    return true;
}

void Server::stop() {
    if (!running_) return;

    LOG_INFO("HTTP server stopping...");
    impl_->svr.stop();
    if (impl_->worker.joinable()) impl_->worker.join();
    running_ = false;
    impl_->listenStarted.store(false);

    LOG_INFO("HTTP server stopped");
}

} // namespace platform::http