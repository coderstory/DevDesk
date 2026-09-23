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
#include <utility>
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

// curl_global_init 非线程安全：代理 worker 线程（httplib 线程池）与 Claude
// 连通性测试后台线程可能并发首发请求，普通 bool check-then-set 是数据竞争
//（审计 2026-09-23 [4][27]）。std::call_once 一行解决。
void globalInit() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
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
//   - Thread A (cpp-httplib worker): 调用 readChunk 阻塞拉数据（消费者）
//   - Thread B (libcurl thread):      在独立 std::thread 上跑 curl_easy_perform
//   - libcurl write callback 把每个 chunk 推入内部 queue（生产者）
//
// Backpressure（队列 >= 100 / <= 50 滞回）：
//   生产者在 write callback 内部阻塞等待消费者腾出队列空间。
//   这是 libcurl 唯一线程安全的背压方式——回调阻塞期间 libcurl 自然停止
//   从 socket 读取，TCP 接收窗口收敛，压力传回上游。
//   （旧实现从消费者线程调 curl_easy_pause(CURLPAUSE_RECV)：暂停后 write
//   callback 永远不会再被调用，既无法 resume 也无法中止，传输会一直挂死
//   到 CURLOPT_TIMEOUT_MS——Windows 首跑 300s 卡死的根因。）
//
// 取消：
//   cancel() 只置标志 + notify_all：
//     - 消费者立即解除阻塞（Error/"cancelled"）
//     - 被背压阻塞的生产者醒来返回 0 → libcurl 以 CURLE_WRITE_ERROR 中止
//       并关闭上游连接（上游随即观察到断开）
//
// 上游截断（CURLE_PARTIAL_FILE，如 chunked 流未写终止 chunk 即断开）：
//   已到达的数据保留，流按正常结束（End）处理，不算 Error。

struct StreamingResponse::Impl {
    // queue 与 producer/consumer 通信
    std::deque<std::string> queue;
    mutable std::mutex queueMutex;
    std::condition_variable queueCv;

    std::atomic<bool> done{false};           // libcurl 线程结束（成功或失败）
    std::atomic<bool> error{false};          // 上游错误
    std::atomic<bool> cancelled{false};      // 用户主动 cancel() 触发
    std::atomic<bool> producerThrottled{false};  // 生产者当前被背压阻塞

    // 错误信息
    std::string errorMessage;
    // cancel 原因（"cancelled" / "proxy stopping"...）；queueMutex 保护
    std::string cancelReason = "cancelled";

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
    std::mutex curlMutex;                    // 保护 curl handle（getinfo/cleanup）

    // 整个传输的截止时刻（来自 Request::timeoutMs）。
    // 背压阻塞发生在 write callback 内部时 CURLOPT_TIMEOUT_MS 无法生效，
    // 生产者用它兜底，避免无限期阻塞。
    std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::time_point::max();

    // backpressure 阈值（spec §10.4）
    static constexpr size_t kPauseThreshold = 100;
    static constexpr size_t kResumeThreshold = 50;
};

// libcurl write callback：把 chunk 推入 queue（生产者）
size_t streamWriteCb(char* ptr, size_t size, size_t nmemb, void* userp) {
    auto* impl = static_cast<StreamingResponse::Impl*>(userp);
    const size_t total = size * nmemb;

    std::unique_lock<std::mutex> lock(impl->queueMutex);

    // ---- backpressure：队列满时阻塞在本回调内 ----
    // libcurl 会停在那里不继续收数据（TCP 窗口收敛），等消费者腾出空间。
    if (impl->queue.size() >= StreamingResponse::Impl::kPauseThreshold) {
        impl->producerThrottled.store(true, std::memory_order_release);
        const bool woke = impl->queueCv.wait_until(lock, impl->deadline, [&impl] {
            return impl->cancelled.load(std::memory_order_acquire) ||
                   impl->queue.size() <=
                       StreamingResponse::Impl::kResumeThreshold;
        });
        impl->producerThrottled.store(false, std::memory_order_release);
        if (!woke) {
            // 背压等待超过了总超时（CURLOPT_TIMEOUT_MS 在回调内不生效）
            impl->error.store(true, std::memory_order_release);
            impl->errorMessage = "stream timed out (consumer too slow)";
            return 0;
        }
    }

    // 如果已取消，返回 0 让 libcurl 中止（CURLE_WRITE_ERROR → 线程按 cancelled 分类）
    if (impl->cancelled.load(std::memory_order_acquire)) {
        return 0;
    }

    impl->queue.emplace_back(ptr, total);
    impl->queueCv.notify_one();   // 唤醒消费者
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
        // 队列腾出了空间：唤醒被背压阻塞的生产者
        impl_->queueCv.notify_one();
        return ChunkResult::More;
    }

    // ---- 队列空，done 或 cancelled ----
    if (impl_->cancelled.load(std::memory_order_acquire)) {
        errorMessage = impl_->cancelReason;
        return ChunkResult::Error;
    }
    if (impl_->error.load(std::memory_order_acquire)) {
        errorMessage = impl_->errorMessage;
        return ChunkResult::Error;
    }
    return ChunkResult::End;
}

void StreamingResponse::cancel() {
    cancelWithReason("cancelled");
}

void StreamingResponse::cancelWithReason(const std::string& reason) {
    if (!impl_) return;
    {
        // 置标志必须在 queueMutex 内（审计 2026-09-23 [9] 的 stop 路径依赖
        // cancel 能立即叫醒阻塞在 readChunk 的消费者）：旧实现 exchange 不带
        // 锁，存在「消费者判完谓词（false）还没睡下 → notify 落空 → 睡到
        // 生产者/空闲超时才被叫醒」的丢唤醒窗口。持锁 store 保证谓词评估与
        // 置标志对 queueMutex 串行，wait(lock, pred) 不可能带着过期谓词睡着。
        std::lock_guard<std::mutex> lock(impl_->queueMutex);
        if (impl_->cancelled.load(std::memory_order_relaxed)) {
            return;  // 已经取消
        }
        impl_->cancelReason = reason;
        impl_->cancelled.store(true, std::memory_order_release);
    }
    // 唤醒所有等待者：
    //   - readChunk 的消费者（立即返回 Error/cancelReason）
    //   - 被背压阻塞的生产者（返回 0 → libcurl 以 CURLE_WRITE_ERROR 中止
    //     并关闭上游连接）
    // 注意：不要在这里调 curl_easy_pause——从其它线程暂停会阻止 write
    // callback 再次被调用，传输将挂死到 CURLOPT_TIMEOUT_MS。
    impl_->queueCv.notify_all();
}

size_t StreamingResponse::queueDepth() const {
    if (!impl_) return 0;
    std::lock_guard<std::mutex> lock(impl_->queueMutex);
    return impl_->queue.size();
}

bool StreamingResponse::isPaused() const {
    if (!impl_) return false;
    return impl_->producerThrottled.load(std::memory_order_acquire);
}

// ============ 流式 sendStreaming ============
//
// 启动 libcurl 线程；线程退出时 curl_easy_cleanup 并标记 done。
// 消费者通过 readChunk 拉数据；statusCode() 读上游状态码。

core::Result<std::unique_ptr<StreamingResponse>> sendStreaming(const Request& req) {
    globalInit();

    auto sr = std::unique_ptr<StreamingResponse>(new StreamingResponse());

    // 背压阻塞发生在 write callback 内部时 CURLOPT_TIMEOUT_MS 不生效，
    // 生产者用 deadline 兜底（timeoutMs <= 0 视为不限时）
    if (req.timeoutMs > 0) {
        sr->impl_->deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(req.timeoutMs);
    }

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
    // 流式空闲超时（W7 P0-3）：timeoutMs=0（无总墙钟上限）时由它兜底
    // "无新 chunk 才 close" 的语义——速率低于 lowSpeedBytesPerSec 持续
    // lowSpeedTimeSec 秒即中止
    if (req.lowSpeedBytesPerSec > 0 && req.lowSpeedTimeSec > 0) {
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, req.lowSpeedBytesPerSec);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, req.lowSpeedTimeSec);
    }
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
        //   主动取消（CURLE_ABORTED_BY_CALLBACK / CURLE_WRITE_ERROR）→ 算 cancelled
        //   其他 → 网络/超时错误
        if (implPtr->cancelled.load(std::memory_order_acquire)) {
            // 主动取消：不算 error，由 cancelled 状态表示
        } else if (rc == CURLE_PARTIAL_FILE) {
            // 上游截断：不算 error，已入队数据照常消费，流以 End 收尾
        } else if (rc != CURLE_OK) {
            std::lock_guard<std::mutex> lock(implPtr->queueMutex);
            // write callback 超时路径可能已写入更具体的信息，别覆盖
            if (!implPtr->error.load(std::memory_order_acquire)) {
                implPtr->error.store(true, std::memory_order_release);
                implPtr->errorMessage = curl_easy_strerror(rc);
            }
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
