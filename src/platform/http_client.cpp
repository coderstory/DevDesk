#include "http_client.h"
#include "http_client_ca.h"
#include <core/logging.h>

#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <system_error>
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

// curl_global_init 非线程安全：代理 worker 与连通性测试线程可能并发首发请求，check-then-set 是数据竞争。
void globalInit() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

} // anonymous namespace

core::Result<CaPolicy> resolveCaPolicy(const std::string& envValue) {
    if (envValue.empty()) return core::Result<CaPolicy>::ok(CaPolicy{});
    const std::filesystem::path bundle(envValue);
    std::error_code fsEc;
    if (!std::filesystem::exists(bundle, fsEc)) {
        return core::Result<CaPolicy>::fail(
            core::ErrorCode::Internal,
            std::string(kCaBundleEnv) + " 指向的文件不存在: " + envValue);
    }
    if (!std::filesystem::is_regular_file(bundle, fsEc)) {
        // 目录/异常路径会让 CAINFO 在 perform 期才报 libcurl 英文错,提前到可读中文
        return core::Result<CaPolicy>::fail(
            core::ErrorCode::Internal,
            std::string(kCaBundleEnv) + " 指向的不是常规文件: " + envValue);
    }
    CaPolicy policy;
    policy.useNative = false;  // bundle 即完整校验：换信任根，不叠加 NATIVE、不弱化
    policy.caInfo = envValue;
    return core::Result<CaPolicy>::ok(policy);
}

void applyCaPolicy(CURL* curl, const CaPolicy& policy) {
    if (policy.useNative) {
        applyCaOptions(curl);
        return;
    }
    // DEVDESK_CA_BUNDLE 覆盖口：bundle 即完整校验（不叠加 NATIVE）
    curl_easy_setopt(curl, CURLOPT_CAINFO, policy.caInfo.c_str());
}

core::Result<ProxyDecision> resolveProxy(ProxyMode mode, const std::string& proxyUrl) {
    ProxyDecision decision;
    switch (mode) {
        case ProxyMode::FollowSystem:
            // 现状语义（零回归）：proxy 非空 = 显式代理（历史行为）；空 = 环境变量 + 回环排除
            if (!proxyUrl.empty()) {
                decision.kind = ProxyDecision::Kind::Custom;
                decision.proxyUrl = "http://" + proxyUrl;
            }
            return core::Result<ProxyDecision>::ok(std::move(decision));
        case ProxyMode::Disabled:
            decision.kind = ProxyDecision::Kind::Disabled;
            return core::Result<ProxyDecision>::ok(std::move(decision));
        case ProxyMode::Custom:
            if (proxyUrl.empty()) {
                return core::Result<ProxyDecision>::fail(
                    core::ErrorCode::ValidationError,
                    "ProxyMode::Custom 需要非空代理地址");
            }
            decision.kind = ProxyDecision::Kind::Custom;
            decision.proxyUrl = "http://" + proxyUrl;
            return core::Result<ProxyDecision>::ok(std::move(decision));
    }
    return core::Result<ProxyDecision>::fail(core::ErrorCode::Internal,
                                             "未知 ProxyMode 枚举值");
}

// 把 ProxyDecision 接到 curl 句柄（send 与 sendStreaming 共用；决策失败已在调用方拦截）。
void applyProxy(CURL* curl, const ProxyDecision& decision) {
    switch (decision.kind) {
        case ProxyDecision::Kind::UseSystem:
            // 回环地址永不走代理：libcurl 默认读环境变量代理，公司代理在场时会劫持 127.0.0.1 请求。
            curl_easy_setopt(curl, CURLOPT_NOPROXY, "localhost,127.0.0.1,::1");
            break;
        case ProxyDecision::Kind::Disabled:
            // 显式置空 CURLOPT_PROXY = 直连，压过环境变量代理。
            curl_easy_setopt(curl, CURLOPT_PROXY, "");
            break;
        case ProxyDecision::Kind::Custom:
            // 显式代理：全部目标（含回环，SSRF 层已另行拦截）都经代理走。
            curl_easy_setopt(curl, CURLOPT_PROXY, decision.proxyUrl.c_str());
            curl_easy_setopt(curl, CURLOPT_NOPROXY, "");
            break;
    }
}

core::Result<Response> send(const Request& req) {
    globalInit();

    // CA 策略（CA 批）：DEVDESK_CA_BUNDLE 覆盖口先于句柄创建解析，失败可读报错。
    const char* caEnv = std::getenv(kCaBundleEnv);
    const auto caPolicy = resolveCaPolicy(caEnv != nullptr ? caEnv : "");
    if (caPolicy.isFail()) {
        LOG_ERROR(std::string(kCaBundleEnv) + " 解析失败: " +
                  caPolicy.errorMessage());
        return core::Result<Response>::fail(core::ErrorCode::Internal,
                                            caPolicy.errorMessage());
    }

    // 代理决策（S6a）：Custom 空地址在句柄创建前即报错，无需清理。
    const auto proxyDecision = resolveProxy(req.proxyMode, req.proxy);
    if (proxyDecision.isFail()) {
        return core::Result<Response>::fail(proxyDecision.errorCode(),
                                            proxyDecision.errorMessage());
    }

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
    applyProxy(curl, proxyDecision.value());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, req.verifyTls ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, req.verifyTls ? 2L : 0L);
    applyCaPolicy(curl, caPolicy.value());

    // DELETE 也要带 body：GitHub Contents API 的删除要求 JSON 体（message + sha）。
    // 设 POSTFIELDS 会把内部方法改成 POST，但 CURLOPT_CUSTOMREQUEST 已把方法串钉成 req.method，
    // 故 DELETE 仍按 DELETE 发出、只是多了请求体。去掉 DELETE 会让删除请求丢 body。
    if (req.method == "POST" || req.method == "PUT" || req.method == "PATCH" ||
        req.method == "DELETE") {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(req.body.size()));
    }

    struct curl_slist* headers = nullptr;
    for (const auto& [k, v] : req.headers) {
        if (v.empty()) continue;  // 空值头不发（`X: ` 是畸形请求头，实测 CF 回 400）
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
        // 超时单列成 NetworkTimeout（其余传输错误仍 NetworkUnreachable）——调用方
        // 只透传 errorMessage，不分支此码；超时/拒连可分是检索类工具的降级文案前提
        return core::Result<Response>::fail(
            rc == CURLE_OPERATION_TIMEDOUT ? core::ErrorCode::NetworkTimeout
                                           : core::ErrorCode::NetworkUnreachable,
            resp.errorMessage);
    }

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    resp.statusCode = static_cast<int>(status);

    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return core::Result<Response>::ok(std::move(resp));
}

bool tcpReachable(const std::string& hostPort, std::chrono::milliseconds timeout) {
    // CONNECT_ONLY + http:// scheme = 纯 TCP 握手探测,不发送任何应用层字节;
    // FRESH_CONNECT 防连接复用把「上一秒还活着」误判为可达。
    globalInit();  // 与 send/sendStreaming 同规:并发首触 curl_global_init 非线程安全
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    const std::string url = "http://" + hostPort + "/";
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 1L);
    curl_easy_setopt(curl, CURLOPT_FRESH_CONNECT, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    // 探测必须反映本机直达性:钉掉环境变量代理,否则 env 代理在场时探测经
    // 第三方代理连「代理的回环」,结果失真(直连口径与 send() 的直连分支一致)
    curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,
                     static_cast<long>(timeout.count()));
    const CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return rc == CURLE_OK;
}

// 流式响应（三线程 + 队列 + backpressure）：readChunk 消费，curl_easy_perform 生产，write callback 推 chunk 入队。
// Backpressure（>=100 停 / <=50 恢复）阻塞在 write callback 内由 libcurl 停读 socket 收敛——唯一线程安全的背压方式；跨线程 curl_easy_pause 会令 callback 不再被调而挂死到超时。
// 取消走 cancel() 标志 + notify_all + 进度回调兜底；CURLE_PARTIAL_FILE（上游截断）按正常结束处理。

struct StreamingResponse::Impl {
    std::deque<std::string> queue;
    mutable std::mutex queueMutex;
    std::condition_variable queueCv;

    std::atomic<bool> done{false};           // libcurl 线程结束（成功或失败）
    std::atomic<bool> error{false};          // 上游错误
    std::atomic<bool> cancelled{false};      // 用户主动 cancel()
    std::atomic<bool> producerThrottled{false};  // 生产者被背压阻塞

    std::string errorMessage;
    // cancel 原因（"cancelled" / "proxy stopping"...）；queueMutex 保护
    std::string cancelReason = "cancelled";

    // status code：header callback 写，线程退出前 getinfo 兜底；statusReady = 状态码已知（header 到达或流结束）
    std::atomic<int> statusCodeAtomic{0};
    std::mutex statusMutex;
    std::condition_variable statusCv;
    bool statusReady = false;
    // 上游 Content-Type（如 text/event-stream）
    std::string contentType;

    // 请求体副本：CURLOPT_POSTFIELDS 只存指针，而 sendStreaming 异步、调用方 Request 常是临时对象——拷一份与 StreamingResponse 同生命周期，防悬垂（上游收到乱码）。
    std::string requestBody;

    CURL* curl = nullptr;
    std::thread thread;
    std::mutex curlMutex;                    // 保护 curl handle（getinfo/cleanup）

    // 背压阻塞在 write callback 内时 CURLOPT_TIMEOUT_MS 不生效，生产者用 deadline 兜底（<=0 视为不限时）。
    std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::time_point::max();

    static constexpr size_t kPauseThreshold = 100;
    static constexpr size_t kResumeThreshold = 50;
};

// libcurl write callback：把 chunk 推入 queue（生产者）
size_t streamWriteCb(char* ptr, size_t size, size_t nmemb, void* userp) {
    auto* impl = static_cast<StreamingResponse::Impl*>(userp);
    const size_t total = size * nmemb;

    std::unique_lock<std::mutex> lock(impl->queueMutex);

    if (impl->queue.size() >= StreamingResponse::Impl::kPauseThreshold) {
        impl->producerThrottled.store(true, std::memory_order_release);
        const bool woke = impl->queueCv.wait_until(lock, impl->deadline, [&impl] {
            return impl->cancelled.load(std::memory_order_acquire) ||
                   impl->queue.size() <=
                       StreamingResponse::Impl::kResumeThreshold;
        });
        impl->producerThrottled.store(false, std::memory_order_release);
        if (!woke) {
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
    impl->queueCv.notify_one();
    return total;
}

// libcurl header callback：解析状态行 + Content-Type
size_t streamHeaderCb(char* buffer, size_t size, size_t nitems, void* userp) {
    auto* impl = static_cast<StreamingResponse::Impl*>(userp);
    const size_t total = size * nitems;
    std::string line(buffer, total);

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

// 进度回调（CURLOPT_XFERINFOFUNCTION + NOPROGRESS=0）：既有两条取消唤醒路径都要求有人堵在队列上，上游连着但一个字节不发时叫不动 libcurl 线程。
// 本回调约每秒被调，读 cancelled 原子量决定中止（返回非零 → CURLE_ABORTED_BY_CALLBACK）；只读一个 atomic，不加锁。
int streamXferInfoCb(void* clientp, curl_off_t /*dltotal*/, curl_off_t /*dlnow*/,
                     curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
    auto* cancelled = static_cast<std::atomic<bool>*>(clientp);
    return cancelled->load(std::memory_order_acquire) ? 1 : 0;
}

StreamingResponse::StreamingResponse() : impl_(std::make_unique<Impl>()) {}

// 票 12：Impl 改由 unique_ptr<Impl> 持有，裸 delete impl_ 消失（消除手动析构的
// 泄漏/双删风险点）。pimpl + unique_ptr 的经典坑：默认 deleter 在**析构函数定义点**
// 实例化，要求 Impl 在此是完整类型——所以本析构函数必须留在 http_client.cpp
// （Impl 定义之后），绝不能挪回头文件内联。行为零变化：cancel() → join 的顺序不变，
// Impl 的析构由 unique_ptr 成员在函数体结束后接管（与原 `delete impl_;` 同一时点）。
StreamingResponse::~StreamingResponse() {
    cancel();
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
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

    impl_->queueCv.wait(lock, [this] {
        return !impl_->queue.empty() ||
               impl_->done.load(std::memory_order_acquire) ||
               impl_->cancelled.load(std::memory_order_acquire);
    });

    if (!impl_->queue.empty()) {
        chunk = std::move(impl_->queue.front());
        impl_->queue.pop_front();
        // 队列腾出了空间：唤醒被背压阻塞的生产者
        impl_->queueCv.notify_one();
        return ChunkResult::More;
    }

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
        // 置标志必须在 queueMutex 内：持锁 store 保证谓词评估与置标志对 mutex 串行，
        // wait(lock,pred) 不会带着过期谓词睡着而错过唤醒。
        std::lock_guard<std::mutex> lock(impl_->queueMutex);
        if (impl_->cancelled.load(std::memory_order_relaxed)) {
            return;
        }
        impl_->cancelReason = reason;
        impl_->cancelled.store(true, std::memory_order_release);
    }
    // 唤醒所有等待者。不要在这里调 curl_easy_pause——跨线程暂停会阻止 callback 再被调，挂死到超时。
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

// sendStreaming：启动 libcurl 线程，线程退出时 curl_easy_cleanup 并标记 done；消费者经 readChunk 拉数据。

core::Result<std::unique_ptr<StreamingResponse>> sendStreaming(const Request& req) {
    globalInit();

    // CA 策略（CA 批）：先于句柄创建解析，失败可读报错（与 send 同口径）。
    const char* caEnv = std::getenv(kCaBundleEnv);
    const auto caPolicy = resolveCaPolicy(caEnv != nullptr ? caEnv : "");
    if (caPolicy.isFail()) {
        LOG_ERROR(std::string(kCaBundleEnv) + " 解析失败: " +
                  caPolicy.errorMessage());
        return core::Result<std::unique_ptr<StreamingResponse>>::fail(
            core::ErrorCode::Internal, caPolicy.errorMessage());
    }

    // 代理决策（S6a）：Custom 空地址在句柄创建前即报错，无需清理。
    const auto proxyDecision = resolveProxy(req.proxyMode, req.proxy);
    if (proxyDecision.isFail()) {
        return core::Result<std::unique_ptr<StreamingResponse>>::fail(
            proxyDecision.errorCode(), proxyDecision.errorMessage());
    }

    auto sr = std::unique_ptr<StreamingResponse>(new StreamingResponse());

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
    // 流式空闲超时：速率低于 lowSpeedBytesPerSec 持续 lowSpeedTimeSec 秒即中止。
    if (req.lowSpeedBytesPerSec > 0 && req.lowSpeedTimeSec > 0) {
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, req.lowSpeedBytesPerSec);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, req.lowSpeedTimeSec);
    }
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    applyProxy(curl, proxyDecision.value());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, streamWriteCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, sr->impl_.get());
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, streamHeaderCb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, sr->impl_.get());
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, streamXferInfoCb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &sr->impl_->cancelled);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, req.verifyTls ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, req.verifyTls ? 2L : 0L);
    applyCaPolicy(curl, caPolicy.value());

    if (req.method == "POST" || req.method == "PUT" || req.method == "PATCH") {
        sr->impl_->requestBody = req.body;  // 保活：见 Impl::requestBody 注释
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS,
                         sr->impl_->requestBody.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                         static_cast<long>(sr->impl_->requestBody.size()));
    }

    struct curl_slist* headers = nullptr;
    for (const auto& [k, v] : req.headers) {
        if (v.empty()) continue;  // 空值头不发（`X: ` 是畸形请求头，实测 CF 回 400）
        std::string h = k + ": " + v;
        headers = curl_slist_append(headers, h.c_str());
    }
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    StreamingResponse::Impl* implPtr = sr->impl_.get();
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

        // 分类错误：CURLE_OK 正常；PARTIAL_FILE 上游截断按 End 消费；主动取消 → cancelled；其他 → 网络/超时错误。
        if (implPtr->cancelled.load(std::memory_order_acquire)) {
        } else if (rc == CURLE_PARTIAL_FILE) {
        } else if (rc != CURLE_OK) {
            std::lock_guard<std::mutex> lock(implPtr->queueMutex);
            // write callback 超时路径可能已写入更具体的信息，别覆盖
            if (!implPtr->error.load(std::memory_order_acquire)) {
                implPtr->error.store(true, std::memory_order_release);
                implPtr->errorMessage = curl_easy_strerror(rc);
            }
        }

        if (headers) curl_slist_free_all(headers);

        {
            std::lock_guard<std::mutex> lock(implPtr->curlMutex);
            if (implPtr->curl) {
                curl_easy_cleanup(implPtr->curl);
                implPtr->curl = nullptr;
            }
        }

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
