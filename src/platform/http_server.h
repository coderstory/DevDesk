// src/platform/http_server.h
// 基于 cpp-httplib 的本地 HTTP 服务器。
#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <utility>

namespace platform::http {

// 请求体大小上限：>10MB 拒绝（防 OOM/DoS），Server 层单点收口（cpp-httplib payload_max_length），超出回 413。
inline constexpr size_t kMaxBodyBytes = 10u * 1024u * 1024u;

struct RequestCtx {
    std::string method;
    std::string path;
    std::map<std::string, std::string> headers;
    std::string body;
    std::string clientAddress;
    // 客户端连接是否已断开（由 cpp-httplib 提供，流式写循环用于提前停止上游）
    std::function<bool()> isConnectionClosed;
};

class ResponseWriter {
public:
    void setStatus(int code);
    void setHeader(const std::string& key, const std::string& value);
    void write(const std::string& data);
    // 空操作：响应在 handler 返回时统一提交；保留签名是既有调用协议。
    void end();

    // 流式 API：与 write()/end() 互斥，调用后走 chunked 传输编码。
    // provider 被 cpp-httplib 反复调用：writeChunk 返回 false = 客户端已断；provider 返回 true
    // 继续、false 结束响应。releaser 在响应彻底结束（含客户端断开）时调用一次，用于 cancel 上游。
    using ChunkWriter = std::function<bool(const char* data, size_t len)>;
    using IsWritableFn = std::function<bool()>;
    using ChunkProvider = std::function<bool(ChunkWriter writeChunk, IsWritableFn isWritable)>;
    using ChunkReleaser = std::function<void(bool success)>;

    void setChunkProvider(const std::string& contentType,
                         ChunkProvider provider,
                         ChunkReleaser releaser = nullptr);

    // 内部访问器（仅由 Server 的内部 handler 适配层使用）
    int status() const { return status_; }
    const std::map<std::string, std::string>& headers() const { return headers_; }
    const std::string& body() const { return body_; }

    bool isStreaming() const { return streaming_; }
    const std::string& streamingContentType() const { return streamingContentType_; }
    // 取走 provider / releaser（std::function 只能 move 一次）
    ChunkProvider takeChunkProvider() { return std::move(chunkProvider_); }
    ChunkReleaser takeChunkReleaser() { return std::move(chunkReleaser_); }

private:
    int status_ = 200;
    std::map<std::string, std::string> headers_;
    std::string body_;

    bool streaming_ = false;
    std::string streamingContentType_;
    ChunkProvider chunkProvider_;
    ChunkReleaser chunkReleaser_;
};

using RouteHandler = std::function<void(const RequestCtx&, ResponseWriter&)>;

class Server {
public:
    Server(std::string bindAddress, int port);
    ~Server();

    void route(const std::string& method, const std::string& path, RouteHandler handler);

    bool start();
    void stop();

    bool isRunning() const { return running_; }
    int actualPort() const { return port_; }

private:
    struct Impl;
    std::string bindAddress_;
    int port_;
    bool running_ = false;
    Impl* impl_ = nullptr;
};

} // namespace platform::http