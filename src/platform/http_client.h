// src/platform/http_client.h
// 基于 libcurl 的 HTTP 客户端（详见 spec §7.1）
#pragma once

#include <atomic>
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

// 带重试
core::Result<Response> sendWithRetry(const Request& req, int maxRetries = 3);

// 流式 chunk 状态
enum class ChunkResult {
    More,
    End,
    Error,
};

// 流式响应句柄（spec §10.4 三线程 + 队列 + backpressure）
//
// 内部架构：
//   - libcurl 在独立 std::thread 上跑 curl_easy_perform（Thread B）
//   - CURLOPT_WRITEFUNCTION 把每个 chunk 推入内部 queue
//   - 消费者通过 readChunk() 拉数据；queue >= 100 时调
//     curl_easy_pause(CURLPAUSE_RECV) 暂停 libcurl，< 50 时恢复
//   - statusCode 由 header callback 解析状态行后填充
//
// 生命周期：
//   - sendStreaming() 启动 libcurl 线程并返回 unique_ptr
//   - 析构时 cancel() + join()，所以引用方应在销毁前读完所有 chunk
//   - 或者调用 cancel() 主动取消
class StreamingResponse {
public:
    StreamingResponse();
    ~StreamingResponse();
    StreamingResponse(const StreamingResponse&) = delete;
    StreamingResponse& operator=(const StreamingResponse&) = delete;

    // 阻塞读取下一个 chunk；队列空 + 上游未结束 → 阻塞等待
    ChunkResult readChunk(std::string& chunk, std::string& errorMessage);

    // 从 header callback 解析得到的状态码
    int statusCode() const;

    // readChunk 返回 Error 时的错误信息
    std::string finalErrorMessage() const;

    // 主动取消（立即停止 libcurl 接收、解锁 readChunk）
    void cancel();

    // 当前队列深度（用于测试 / 调试）
    size_t queueDepth() const;

    // 当前是否处于已暂停接收状态（backpressure 触发）
    bool isPaused() const;

private:
    friend core::Result<std::unique_ptr<StreamingResponse>>
        sendStreaming(const Request& req);

    // 三线程 + 队列 + backpressure 的真实实现（spec §10.4）
    // 全部状态隐藏在 Impl 内；header 只暴露 Pimpl 前向声明。
    struct Impl;
    Impl* impl_ = nullptr;
};

core::Result<std::unique_ptr<StreamingResponse>> sendStreaming(const Request& req);

} // namespace platform::http
