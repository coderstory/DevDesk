#include "http_client.h"
#include <core/logging.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
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

} // anonymous namespace

// ============ 同步 send / sendWithRetry ============

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
            int delay = 1000 << i;
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }
    }
    return last;
}

// ============ 流式响应（三线程 + 队列 + backpressure，spec §10.4）============
//
// 线程模型：
//   - Thread A (cpp-httplib worker): 调用 readChunk 阻塞拉数据
//   - Thread B (libcurl thread):      在独立 std::thread 上跑 curl_easy_perform
//   - libcurl write callback 把每个 chunk 推入内部 queue
//
// Backpressure：
//   - Thread A 监控 queue 大小；>= 100 调 curl_easy_pause(CURLPAUSE_RECV)
//     暂停 libcurl 接收；< 50 调 CURLPAUSE_RECV_CONT 恢复
//   - queue 深度稳定在 50-100 之间（spec §10.4）
//
// 取消：
//   - cancel() 设置 cancelled 标志 + curl_easy_pause + notify cv
//   - write callback 看到 cancelled 返回 0 让 libcurl 中止

struct StreamingResponse::Impl {
    // queue 与 producer/consumer 通信
    std::deque<std::string> queue;
    mutable std::mutex queueMutex;
    std::condition_variable queueCv;

    std::atomic<bool> done{false};           // libcurl 线程结束（成功或失败）
    std::atomic<bool> error{false};          // 上游错误
    std::atomic<bool> cancelled{false};      // 用户主动 cancel() 触发
    std::atomic<bool> paused{false};         // 当前是否已暂停 libcurl 接收

    // 错误信息
    std::string errorMessage;

    // status code（header callback 写，线程退出前 getinfo 兜底）
    std::atomic<int> statusCodeAtomic{0};
    // 状态码是否已知（header 到达或流结束）
    std::mutex statusMutex;
    std::condition_variable statusCv;
    bool statusReady = false;
    // 上游 Content-Type（如 text/event-stream）
    std::string contentType;

    // CURL handle + 执行线程
    CURL* curl = nullptr;
    std::thread thread;
    std::mutex curlMutex;                    // 保护 curl handle（pause/resume/getinfo）

    // backpressure 阈值（spec §10.4）
    static constexpr size_t kPauseThreshold = 100;
    static constexpr size_t kResumeThreshold = 50;
};

// libcurl write callback：把 chunk 推入 queue
size_t streamWriteCb(char* ptr, size_t size, size_t nmemb, void* userp) {
    auto* impl = static_cast<StreamingResponse::Impl*>(userp);
    const size_t total = size * nmemb;

    // 如果已取消，返回 0 让 libcurl 中止
    if (impl->cancelled.load(std::memory_order_acquire)) {
        return 0;
    }

    {
        std::lock_guard<std::mutex> lock(impl->queueMutex);
        impl->queue.emplace_back(ptr, total);
    }
    impl->queueCv.notify_one();

    return total;
}

// libcurl header callback：解析状态行 + Content-Type
size_t streamHeaderCb(char* buffer, size_t size, size_t nitems, void* userp) {
    auto* impl = static_cast<StreamingResponse::Impl*>(userp);
    const size_t total = size * nitems;
    std::string line(buffer, total);

    // 状态行形如 "HTTP/1.1 200 OK\r\n"
    if (line.compare(0, 5, "HTTP/") == 0) {
        const size_t sp1 = line.find(' ');
        if (sp1 != std::string::npos) {
            const size_t sp2 = line.find(' ', sp1 + 1);
            if (sp2 != std::string::npos) {
                const std::string codeStr = line.substr(sp1 + 1, sp2 - sp1 - 1);
                int code = 0;
                try {
                    code = std::stoi(codeStr);
                } catch (...) {
                    code = 0;
                }
                if (code > 0) {
                    impl->statusCodeAtomic.store(code, std::memory_order_release);
                    {
                        std::lock_guard<std::mutex> lock(impl->statusMutex);
                        impl->statusReady = true;
                    }
                    impl->statusCv.notify_all();
                }
            }
        }
    } else {
        // 普通 header 行："Content-Type: text/event-stream\r\n"
        const size_t colon = line.find(':');
        if (colon != std::string::npos && colon > 0) {
            std::string key = line.substr(0, colon);
            for (auto& c : key) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (key == "content-type") {
                std::string value = line.substr(colon + 1);
                while (!value.empty() &&
                       (value.front() == ' ' || value.front() == '\t')) {
                    value.erase(0, 1);
                }
                while (!value.empty() &&
                       (value.back() == '\r' || value.back() == '\n')) {
                    value.pop_back();
                }
                std::lock_guard<std::mutex> lock(impl->statusMutex);
                impl->contentType = std::move(value);
            }
        }
    }

    return total;
}

// ============ StreamingResponse 公开接口 ============

StreamingResponse::StreamingResponse() : impl_(new Impl()) {}
StreamingResponse::~StreamingResponse() {
    cancel();
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
    delete impl_;
    impl_ = nullptr;
}

int StreamingResponse::statusCode() const {
    return impl_->statusCodeAtomic.load(std::memory_order_acquire);
}

bool StreamingResponse::waitForStatus(int timeoutMs) {
    std::unique_lock<std::mutex> lock(impl_->statusMutex);
    const bool ready = impl_->statusCv.wait_for(
        lock, std::chrono::milliseconds(timeoutMs),
        [this] { return impl_->statusReady; });
    return ready;
}

std::string StreamingResponse::contentType() const {
    std::lock_guard<std::mutex> lock(impl_->statusMutex);
    return impl_->contentType;
}

std::string StreamingResponse::finalErrorMessage() const {
    std::lock_guard<std::mutex> lock(impl_->queueMutex);
    return impl_->errorMessage;
}

ChunkResult StreamingResponse::readChunk(std::string& chunk,
                                          std::string& errorMessage) {
    std::unique_lock<std::mutex> lock(impl_->queueMutex);

    // ---- backpressure（spec §10.4）----
    // queue >= 100 → 暂停 libcurl 接收
    if (!impl_->paused.load(std::memory_order_acquire) &&
        impl_->queue.size() >= Impl::kPauseThreshold) {
        lock.unlock();
        {
            std::lock_guard<std::mutex> curlLock(impl_->curlMutex);
            if (impl_->curl) {
                curl_easy_pause(impl_->curl, CURLPAUSE_RECV);
            }
        }
        impl_->paused.store(true, std::memory_order_release);
        lock.lock();
    }

    // 阻塞等待：直到有数据 / 流结束 / 取消
    impl_->queueCv.wait(lock, [this] {
        return !impl_->queue.empty() ||
               impl_->done.load(std::memory_order_acquire) ||
               impl_->cancelled.load(std::memory_order_acquire);
    });

    // ---- 取数据 ----
    if (!impl_->queue.empty()) {
        chunk = std::move(impl_->queue.front());
        impl_->queue.pop_front();

        // queue < 50 且当前 paused → 恢复 libcurl
        if (impl_->paused.load(std::memory_order_acquire) &&
            impl_->queue.size() <= Impl::kResumeThreshold) {
            lock.unlock();
            {
                std::lock_guard<std::mutex> curlLock(impl_->curlMutex);
                if (impl_->curl) {
                    curl_easy_pause(impl_->curl, CURLPAUSE_RECV_CONT);
                }
            }
            impl_->paused.store(false, std::memory_order_release);
            lock.lock();
        }
        return ChunkResult::More;
    }

    // ---- 队列空，done 或 cancelled ----
    if (impl_->cancelled.load(std::memory_order_acquire)) {
        errorMessage = "cancelled";
        return ChunkResult::Error;
    }
    if (impl_->error.load(std::memory_order_acquire)) {
        errorMessage = impl_->errorMessage;
        return ChunkResult::Error;
    }
    return ChunkResult::End;
}

void StreamingResponse::cancel() {
    if (!impl_) return;
    if (impl_->cancelled.exchange(true, std::memory_order_acq_rel)) {
        return;  // 已经取消
    }
    // 唤醒所有阻塞在 readChunk 的等待者
    impl_->queueCv.notify_all();
    // 尝试暂停 libcurl；write callback 看到 cancelled 标志后会返回 0
    // libcurl 收到 0 返回值会中止并返回 CURLE_WRITE_ERROR
    {
        std::lock_guard<std::mutex> lock(impl_->curlMutex);
        if (impl_->curl) {
            curl_easy_pause(impl_->curl, CURLPAUSE_RECV);
        }
    }
}

size_t StreamingResponse::queueDepth() const {
    if (!impl_) return 0;
    std::lock_guard<std::mutex> lock(impl_->queueMutex);
    return impl_->queue.size();
}

bool StreamingResponse::isPaused() const {
    if (!impl_) return false;
    return impl_->paused.load(std::memory_order_acquire);
}

// ============ 流式 sendStreaming ============
//
// 启动 libcurl 线程；线程退出时 curl_easy_cleanup 并标记 done。
// 消费者通过 readChunk 拉数据；statusCode() 读上游状态码。

core::Result<std::unique_ptr<StreamingResponse>> sendStreaming(const Request& req) {
    globalInit();

    auto sr = std::unique_ptr<StreamingResponse>(new StreamingResponse());

    {
        std::lock_guard<std::mutex> lock(sr->impl_->curlMutex);
        sr->impl_->curl = curl_easy_init();
    }
    if (!sr->impl_->curl) {
        return core::Result<std::unique_ptr<StreamingResponse>>::fail(
            core::ErrorCode::Internal, "curl_easy_init failed");
    }

    CURL* curl = sr->impl_->curl;

    curl_easy_setopt(curl, CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, req.method.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, req.timeoutMs);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, streamWriteCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, sr->impl_);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, streamHeaderCb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, sr->impl_);
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

    // ---- 启动 libcurl 线程 ----
    StreamingResponse::Impl* implPtr = sr->impl_;
    sr->impl_->thread = std::thread([implPtr, headers,
                                     url = req.url, method = req.method]() {
        CURLcode rc;
        if (implPtr->cancelled.load(std::memory_order_acquire)) {
            rc = CURLE_ABORTED_BY_CALLBACK;
        } else {
            rc = curl_easy_perform(implPtr->curl);
        }

        // 兜底读 status code（如果 header callback 没解析到）
        long status = 0;
        {
            std::lock_guard<std::mutex> lock(implPtr->curlMutex);
            if (implPtr->curl) {
                curl_easy_getinfo(implPtr->curl, CURLINFO_RESPONSE_CODE, &status);
            }
        }
        if (status > 0) {
            implPtr->statusCodeAtomic.store(static_cast<int>(status),
                                            std::memory_order_release);
        }
        // 状态码已定（或流已结束）：唤醒 waitForStatus 的等待者
        {
            std::lock_guard<std::mutex> lock(implPtr->statusMutex);
            implPtr->statusReady = true;
        }
        implPtr->statusCv.notify_all();

        // 分类错误
        //   CURLE_OK             → 正常结束
        //   CURLE_PARTIAL_FILE   → 上游提前断开（chunked 流未写终止 chunk）：
        //                          已收到的数据保留，按正常结束处理（spec §10.4
        //                          截断语义：流干净结束，不算 Error）
        //   主动取消（CURLE_ABORTED_BY_CALLBACK / CURLE_WRITE_ERROR） → 算 cancelled
        //   其他 → 网络/超时错误
        if (implPtr->cancelled.load(std::memory_order_acquire)) {
            // 主动取消：不算 error，由 cancelled 状态表示
        } else if (rc == CURLE_PARTIAL_FILE) {
            // 上游截断：不算 error，已入队数据照常消费，流以 End 收尾
        } else if (rc != CURLE_OK) {
            std::lock_guard<std::mutex> lock(implPtr->queueMutex);
            implPtr->error.store(true, std::memory_order_release);
            implPtr->errorMessage = curl_easy_strerror(rc);
        }

        if (headers) curl_slist_free_all(headers);

        // 清理 curl handle（在锁内做，保证后续 pause 不再使用）
        {
            std::lock_guard<std::mutex> lock(implPtr->curlMutex);
            if (implPtr->curl) {
                curl_easy_cleanup(implPtr->curl);
                implPtr->curl = nullptr;
            }
        }

        // 标记 done 并唤醒所有消费者
        {
            std::lock_guard<std::mutex> lock(implPtr->queueMutex);
            implPtr->done.store(true, std::memory_order_release);
        }
        implPtr->queueCv.notify_all();

        LOG_DEBUG("HTTP stream thread end: " + method + " " + url +
                  " rc=" + std::to_string(static_cast<int>(rc)) +
                  " status=" + std::to_string(status));
    });

    return core::Result<std::unique_ptr<StreamingResponse>>::ok(std::move(sr));
}

} // namespace platform::http
