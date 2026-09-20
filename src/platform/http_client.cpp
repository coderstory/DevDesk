#include "http_client.h"
#include <core/logging.h>

#include <chrono>
#include <cstring>
#include <curl/curl.h>

namespace platform::http {

namespace {

size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* resp = static_cast<Response*>(userp);
    size_t total = size * nmemb;
    resp->body.append(static_cast<char*>(contents), total);
    return total;
}

size_t headerCallback(char* buffer, size_t size, size_t nitems, void* userp) {
    auto* resp = static_cast<Response*>(userp);
    size_t total = size * nitems;
    std::string line(buffer, total);
    auto colon = line.find(':');
    if (colon != std::string::npos) {
        std::string key = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        // 去掉前后空白
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(0, 1);
        while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) value.pop_back();
        if (!key.empty()) resp->headers[key] = value;
    }
    return total;
}

void globalInit() {
    static bool initialized = false;
    if (!initialized) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        initialized = true;
    }
}

} // namespace

core::Result<Response> send(const Request& req) {
    globalInit();
    Response resp;

    CURL* curl = curl_easy_init();
    if (!curl) {
        return core::Result<Response>::fail(core::ErrorCode::Internal, "curl_easy_init failed");
    }

    auto start = std::chrono::steady_clock::now();

    curl_easy_setopt(curl, CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, req.method.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, req.timeoutMs);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, req.verifyTls ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, req.verifyTls ? 2L : 0L);

    if (req.method == "POST" || req.method == "PUT" || req.method == "PATCH") {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(req.body.size()));
    }

    struct curl_slist* headers = nullptr;
    for (const auto& [k, v] : req.headers) {
        std::string h = k + ": " + v;
        headers = curl_slist_append(headers, h.c_str());
    }
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode rc = curl_easy_perform(curl);
    auto end = std::chrono::steady_clock::now();
    resp.elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    if (rc != CURLE_OK) {
        resp.networkError = true;
        resp.errorMessage = curl_easy_strerror(rc);
        if (headers) curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        LOG_ERROR("HTTP " + req.method + " " + req.url + " failed: " + resp.errorMessage);
        return core::Result<Response>::fail(core::ErrorCode::NetworkUnreachable,
            resp.errorMessage);
    }

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    resp.statusCode = static_cast<int>(status);

    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return core::Result<Response>::ok(std::move(resp));
}

core::Result<Response> sendWithRetry(const Request& req, int maxRetries) {
    core::Result<Response> last;
    for (int i = 0; i <= maxRetries; ++i) {
        auto result = send(req);
        if (result.isOk()) return result;
        last = std::move(result);
        if (i < maxRetries) {
            // 简单指数退避
            int delay = 1000 << i;
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }
    }
    return last;
}

// 流式响应（占位实现 - 完整三线程实现见 spec §10.4 PoC 后）
struct StreamingClient {
    CURL* curl = nullptr;
    std::string buffer;
    bool done = false;
    std::string errorMessage;
    int statusCode = 0;
    bool cancelled = false;

    static size_t writeCb(void* contents, size_t size, size_t nmemb, void* userp) {
        auto* self = static_cast<StreamingClient*>(userp);
        size_t total = size * nmemb;
        self->buffer.append(static_cast<char*>(contents), total);
        return total;
    }
};

StreamingResponse::StreamingResponse() : handle_(new StreamingClient()) {}
StreamingResponse::~StreamingResponse() { delete static_cast<StreamingClient*>(handle_); }

ChunkResult StreamingResponse::readChunk(std::string& chunk, std::string& errorMessage) {
    auto* self = static_cast<StreamingClient*>(handle_);
    if (cancelled_) {
        errorMessage = "cancelled";
        return ChunkResult::Error;
    }
    if (!self->buffer.empty()) {
        chunk = std::move(self->buffer);
        self->buffer.clear();
        return ChunkResult::More;
    }
    if (self->done) return ChunkResult::End;
    errorMessage = self->errorMessage;
    return self->errorMessage.empty() ? ChunkResult::End : ChunkResult::Error;
}

void StreamingResponse::cancel() {
    cancelled_ = true;
    auto* self = static_cast<StreamingClient*>(handle_);
    if (self->curl) {
        curl_easy_perform(self->curl);
        // 实际取消要更复杂的实现
    }
}

core::Result<std::unique_ptr<StreamingResponse>> sendStreaming(const Request& req) {
    // 简化实现：单次发送全部响应作为单 chunk
    auto resp = send(req);
    if (resp.isFail()) return core::Result<std::unique_ptr<StreamingResponse>>::fail(
        resp.errorCode(), resp.errorMessage());

    auto sr = std::make_unique<StreamingResponse>();
    auto* sc = static_cast<StreamingClient*>(sr.get());
    sc->buffer = std::move(resp.value().body);
    sc->statusCode = resp.value().statusCode;
    sc->done = true;
    return core::Result<std::unique_ptr<StreamingResponse>>::ok(std::move(sr));
}

} // namespace platform::http