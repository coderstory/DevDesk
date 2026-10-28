// src/platform/http_client.h
// 基于 libcurl 的 HTTP 客户端。同步 send + 流式 sendStreaming（三线程 + 队列 + backpressure）。
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <core/result.h>

namespace platform::http {

struct Request {
    std::string method;                              // "GET" / "POST"
    std::string url;
    std::map<std::string, std::string> headers;
    std::string body;
    long timeoutMs = 30000;
    bool verifyTls = true;
    // 非空 = 经此 HTTP 代理（"host:port"，CURLOPT_PROXY 口径）；空 = 直连。
    // send 与 sendStreaming 同款接线；代理是传输通道：目标侧 SSRF 校验与本字段无关
    // （由调用方先行完成）。
    std::string proxy;
    // 流式专用空闲超时（仅 sendStreaming）：速率低于 lowSpeedBytesPerSec 持续 lowSpeedTimeSec 秒
    // → curl 中止（CURLE_OPERATION_TIMEDOUT），替代总墙钟超时；任一 <= 0 表示不启用。
    long lowSpeedBytesPerSec = 0;
    long lowSpeedTimeSec = 0;
};

struct Response {
    int statusCode = 0;
    std::string body;
    std::map<std::string, std::string> headers;
    long elapsedMs = 0;
    bool networkError = false;
    std::string errorMessage;
};

// 同步发送
core::Result<Response> send(const Request& req);

// TCP 可连通性探测(本地代理预检):hostPort = "host:port"。阻塞至多 timeout,
// 连得上(TCP 握手完成)即 true,不发任何应用层字节。
bool tcpReachable(const std::string& hostPort, std::chrono::milliseconds timeout);

// 流式 chunk 状态
enum class ChunkResult {
    More,
    End,
    Error,
};

// 流式响应句柄（三线程 + 队列 + backpressure）
// 内部：libcurl 在独立线程跑 curl_easy_perform，每个 chunk 推入内部 queue，消费方经 readChunk() 拉；
// 队列 >= 100 时生产者阻塞在 write callback 内（libcurl 停止收数据、TCP 窗口收敛），<= 50 恢复——
// 回调内阻塞是 libcurl 唯一线程安全的背压方式，不使用跨线程 curl_easy_pause。
// 生命周期：sendStreaming() 启动线程并返回 unique_ptr；析构 = cancel() + join()，引用方应在销毁前读完。
class StreamingResponse {
public:
    StreamingResponse();
    ~StreamingResponse();
    StreamingResponse(const StreamingResponse&) = delete;
    StreamingResponse& operator=(const StreamingResponse&) = delete;

    // 阻塞读取下一个 chunk；队列空 + 上游未结束 → 阻塞等待
    ChunkResult readChunk(std::string& chunk, std::string& errorMessage);

    // header callback 解析得到的状态码
    int statusCode() const;

    // 等上游响应头到达（状态码可用），最多 timeoutMs 毫秒；true = 状态码已知或流已结束，
    // false = 超时。代理场景用于在写客户端响应头之前拿到上游状态码。
    bool waitForStatus(int timeoutMs);

    // 上游 Content-Type（如 text/event-stream）；未到达时返回空串
    std::string contentType() const;

    // readChunk 返回 Error 时的错误信息
    std::string finalErrorMessage() const;

    // 主动取消（立即停止 libcurl 接收、解锁 readChunk）；readChunk 以 Error + "cancelled" 返回。
    void cancel();

    // 带原因的主动取消：readChunk 返回 Error + reason（消费方据此区分断开与主动中止）。
    void cancelWithReason(const std::string& reason);

    // 当前队列深度（测试/调试用）
    size_t queueDepth() const;

    // 当前生产者是否被背压阻塞（queue >= 100）
    bool isPaused() const;

private:
    friend core::Result<std::unique_ptr<StreamingResponse>>
        sendStreaming(const Request& req);

    // libcurl 回调需要访问 Impl（在 http_client.cpp 中定义）
    friend size_t streamWriteCb(char* ptr, size_t size, size_t nmemb, void* userp);
    friend size_t streamHeaderCb(char* buffer, size_t size, size_t nitems, void* userp);

    // 三线程 + 队列 + backpressure 的真实实现；全部状态藏在 Impl 内，header 只暴露前向声明。
    // 票 12：Impl 由 unique_ptr 持有（裸 delete 消失）。析构点必须是完整类型的经典约束
    // 由「析构函数定义在 http_client.cpp（Impl 定义之后）」满足——头文件只声明析构，
    // 不要内联，否则每个包含本头的 TU 都会对不完整类型实例化 deleter 而编译失败。
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

core::Result<std::unique_ptr<StreamingResponse>> sendStreaming(const Request& req);

} // namespace platform::http
