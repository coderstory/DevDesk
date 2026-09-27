// src/platform/http_server.h
// 基于 cpp-httplib 的本地 HTTP 服务器（详见 spec §7.2）
#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <string>

namespace platform::http {

// 请求体大小上限（spec §10.4：>10MB 拒绝，防 OOM/DoS）。
// 在 Server 层单点收口（cpp-httplib payload_max_length），超出直接回 413。
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
    void end();

    // ---- 流式 API（spec §10.4）----
    // 与 write()/end() 互斥：调用 setChunkProvider 后走 chunked 传输编码。
    //
    // provider 会被 cpp-httplib 反复调用（每轮写一块）：
    //   - writeChunk(data, len) 写一块；返回 false 表示客户端已断开
    //   - isWritable()          查询客户端连接是否仍然可写
    //   - 返回 true  → 继续调用（还有数据）
    //   - 返回 false → 结束响应
    //
    // releaser 在响应彻底结束时调用一次（含客户端断开），用于 cancel 上游。
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
    bool ended() const { return ended_; }

    bool isStreaming() const { return streaming_; }
    const std::string& streamingContentType() const { return streamingContentType_; }
    // 取走 provider / releaser（std::function 只能 move 一次）
    ChunkProvider takeChunkProvider() { return std::move(chunkProvider_); }
    ChunkReleaser takeChunkReleaser() { return std::move(chunkReleaser_); }

private:
    int status_ = 200;
    std::map<std::string, std::string> headers_;
    std::string body_;
    bool ended_ = false;

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