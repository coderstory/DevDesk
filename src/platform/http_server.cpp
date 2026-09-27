// src/platform/http_server.cpp
// 基于 cpp-httplib 的本地 HTTP 服务器（详见 spec §7.2 / P0a PoC）
//
// 线程模型：
//   - `httplib::Server` 在用户提供的线程里运行 listen 循环
//   - 我们在 std::thread 里调 listen_after_bind()，stop() 时 join
//   - 实际请求处理由 cpp-httplib 内部线程池完成（每个请求一个任务）
//
// 绑定地址由调用方给定（DevDesk 代理固定 127.0.0.1，仅本机可访问，无鉴权）
//
// 路由模型：
//   - route(method, path, handler) 转发到 svr.Get/Post/Put/Delete/Patch/Options
//   - handler 是 std::function<void(const RequestCtx&, ResponseWriter&)>
//     我们在 cpp-httplib 的 Handler 里手动桥接 ctx → writer

#include "http_server.h"

#include <core/logging.h>

#include <httplib.h>

#include <atomic>
#include <cctype>
#include <map>
#include <mutex>
#include <thread>

namespace platform::http {

namespace {

// 把 cpp-httplib 的 Headers (multimap) 转成 std::map（保留首个同名 value）
std::map<std::string, std::string> toHeaderMap(const httplib::Headers& src) {
    std::map<std::string, std::string> dst;
    for (const auto& kv : src) {
        if (dst.find(kv.first) == dst.end()) {
            dst.emplace(kv.first, kv.second);
        }
    }
    return dst;
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

        // ---- 流式路径（spec §10.4）----
        // ResponseWriter 设置了 chunk provider → 用 chunked 传输编码流式回写。
        // provider 由 cpp-httplib 在其 worker 线程反复调用；我们把它桥接到
        // 用户提供的 ChunkProvider（内部从 queue 拉数据、实现 backpressure）。
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
                        // cpp-httplib 的约定：provider 返回 false 会被当作
                        // Canceled（不写终止 chunk，客户端会看到截断）。
                        // 正确做法是调用 sink.done() 写 "0\r\n\r\n" 正常收尾，
                        // 然后返回 true 让写循环按 data_available=false 退出。
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
            const auto& hmap = rw.headers();
            auto it = hmap.find("Content-Type");
            if (it != hmap.end()) {
                res.set_content(rw.body(), it->second);
            } else {
                res.set_content(rw.body(), "text/plain; charset=utf-8");
            }
        }
    };
}

} // namespace

// ---------- ResponseWriter 实现 ----------

void ResponseWriter::setStatus(int code) { status_ = code; }
void ResponseWriter::setHeader(const std::string& key,
                               const std::string& value) {
    headers_[key] = value;
}
void ResponseWriter::write(const std::string& data) {
    body_ += data;
    ended_ = false;
}
void ResponseWriter::end() { ended_ = true; }

void ResponseWriter::setChunkProvider(const std::string& contentType,
                                      ChunkProvider provider,
                                      ChunkReleaser releaser) {
    streaming_ = true;
    streamingContentType_ = contentType;
    chunkProvider_ = std::move(provider);
    chunkReleaser_ = std::move(releaser);
    ended_ = false;
}

// ---------- Server::Impl ----------

struct Server::Impl {
    httplib::Server svr;
    std::thread worker;
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> listenStarted{false};
    std::mutex routeMutex;
};

// ---------- Server 实现 ----------

Server::Server(std::string bindAddress, int port)
    : bindAddress_(std::move(bindAddress)),
      port_(port),
      impl_(new Impl()) {
    // 请求体上限（spec §10.4）：所有路由单点收口，超出由 cpp-httplib 回 413
    impl_->svr.set_payload_max_length(kMaxBodyBytes);
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