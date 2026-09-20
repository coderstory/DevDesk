// src/platform/http_client.h
// 基于 libcurl 的 HTTP 客户端（详见 spec §7.1）
#pragma once

#include <map>
#include <memory>
#include <string>

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

// 流式响应句柄
class StreamingResponse {
public:
    StreamingResponse();
    ~StreamingResponse();
    StreamingResponse(const StreamingResponse&) = delete;
    StreamingResponse& operator=(const StreamingResponse&) = delete;

    ChunkResult readChunk(std::string& chunk, std::string& errorMessage);

    int statusCode() const { return statusCode_; }
    std::string finalErrorMessage() const { return finalError_; }

    void cancel();

private:
    friend class StreamingClient;
    void* handle_ = nullptr;
    int statusCode_ = 0;
    std::string finalError_;
    bool cancelled_ = false;
};

core::Result<std::unique_ptr<StreamingResponse>> sendStreaming(const Request& req);

} // namespace platform::http