// src/platform/http_server.h
// 基于 cpp-httplib 的本地 HTTP 服务器。
#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace platform::http {

// 请求体大小上限：>10MB 拒绝（防 OOM/DoS），Server 层单点收口（cpp-httplib payload_max_length），超出回 413。
inline constexpr size_t kMaxBodyBytes = 10u * 1024u * 1024u;

// 由绑定地址推导 Host 白名单（不写死字面量——白名单跟着实际监听地址走）：
// 回环绑定（127.0.0.1 / ::1 / localhost，含 [::1] 括号拼写、大小写不敏感）
// → 三种回环形态都放行（本机客户端可能用任一名字访问）；非回环绑定 → 只放行
// 与监听地址一致的那个 host。调用方（claude 代理）把实际 bind 地址传进来即可。
std::vector<std::string> hostWhitelistForBind(const std::string& bindAddress);

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

    // Host 头白名单（应用层校验）：配置后仅放行 Host 头 host 部分（端口可选剥除、
    // host 部分按 DNS 规则大小写不敏感）命中白名单的请求，其余一律 403、不进路由
    // handler。语义是「Host 白名单」而非「来源 IP 过滤」——连接级来源约束（如只
    // bind 127.0.0.1）不受影响，本方法只补「本机其它页面借道伪造 Host 访问」的口子。
    // 取舍：未配置（默认）/ 传空 = 放行所有 Host，向后兼容既有调用方与测试——本地
    // 服务此前从不看 Host，默认收紧会把所有旧用法打成 403；需要收紧的调用方
    // （claude 代理）显式配置，配置后严格校验。重复调用以后一次为准。
    void setAllowedHosts(std::vector<std::string> hosts);

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