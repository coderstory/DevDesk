// tests/test_modules/test_streaming_stress.cpp
// P10b 流式压测（恢复计划 T8）
//
// 测试架构（与 test_streaming_proxy.cpp 相同）：
//   [mock 上游 httplib::Server]  ← libcurl(StreamingResponse) ← [ProxyRouter]
//                                                                     ↓
//                                                    [platform::http::Server]
//                                                                     ↓
//                                                       [httplib::Client 收]
//
// 用例（./build/tests/devdesk_tests.exe stress 可过滤）：
//   1. 10MB 大 body 流式端到端透传：字节数完整 + 顺序正确
//      （全串比对 + FNV-1a 累计哈希 + 每 64KB 块前 8 字节块号独立校验）
//   2. 10MB 大 body 缓冲（非流式）路径
//   3. soak 60s：上游每 50ms 一个 chunk，断言收到 chunk 数误差 < 5% 且顺序完整
//   4. soak 300s：DEVDESK_SOAK=1 门控；未设时打印 [SKIP] 直接通过（常规套件
//      不背 5 分钟）
//   5. 上游断连恢复：原始 socket 上游发到一半 closesocket（不发终止 chunk），
//      断言流以 End 干净收尾（不是 Error），已收数据与上游已发字节完全一致
//   6. 内存采样：10MB 流式 e2e 循环 20 轮，每 5 轮用 psapi GetProcessMemoryInfo
//      记录本进程工作集内存，断言增量 < 100MB（宽松阈值：防泄漏而非精确测量）
//
// 注意：
//   - REGISTER_TEST 的 body 是宏参数：**预处理器指令（#ifdef 等）不能出现在
//     body 里**，需要条件编译的逻辑放到 body 外的普通函数（见用例 6 内存采样）。
//   - mock 上游 provider 结束时必须 sink.done() + return true（返回 false 会被
//     cpp-httplib 当作 Canceled，不写终止 chunk，客户端看到的是截断）。
#include "doctest.h"
#include "test_support.h"  // TIN-06 收尾波：InMemoryDb

#include "claude_service.h"
#include "core/sqlite_db.h"
#include "platform/http_client.h"
#include "platform/http_server.h"
#include "proxy_router.h"
#include "streaming_proxy.h"

#include <httplib.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <psapi.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono;
using module::claude::Profile;

// 10MB = 160 块 × 64KB
constexpr size_t kBlockSize = 65536;
constexpr size_t kBlockCount = 160;

// ---------- 确定性大 body ----------

// 第 blockIndex 块的确定性字节：前 8 字节写块号（小端），其余 xorshift64 填充。
// 块号前缀让"乱序"能独立于哈希比对定位到具体块。
std::string makeBlock(size_t blockIndex, size_t size) {
    std::string s(size, '\0');
    uint64_t st = 0x9E3779B97F4A7C15ull ^
                  ((static_cast<uint64_t>(blockIndex) + 1) * 0xBF58476D1CE4E5B9ull);
    for (size_t i = 0; i < size; ++i) {
        st ^= st << 13;
        st ^= st >> 7;
        st ^= st << 17;
        s[i] = static_cast<char>(st & 0xFFu);
    }
    for (size_t b = 0; b < 8 && b < size; ++b) {
        s[b] = static_cast<char>((blockIndex >> (8 * b)) & 0xFFu);
    }
    return s;
}

std::vector<std::string> makeBlocks() {
    std::vector<std::string> blocks;
    blocks.reserve(kBlockCount);
    for (size_t i = 0; i < kBlockCount; ++i) {
        blocks.push_back(makeBlock(i, kBlockSize));
    }
    return blocks;
}

std::string joinBlocks(const std::vector<std::string>& blocks) {
    std::string out;
    out.reserve(blocks.size() * kBlockSize);
    for (size_t i = 0; i < blocks.size(); ++i) out += blocks[i];
    return out;
}

// FNV-1a 64 位累计哈希（顺序敏感：丢字节 / 乱序 / 改字节都会变）
uint64_t fnv1a64(const std::string& data) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : data) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

// ---------- 测试环境（与 test_streaming_proxy.cpp 相同的骨架）----------

// 轮询等待谓词成立
template <typename Fn>
bool waitUntil(Fn fn, int waitMs) {
    const auto deadline = steady_clock::now() + milliseconds(waitMs);
    while (steady_clock::now() < deadline) {
        if (fn()) return true;
        std::this_thread::sleep_for(milliseconds(10));
    }
    return fn();
}

Profile makeProfile(const std::string& baseUrl) {
    Profile p;
    p.id = 1;
    p.name = "stress-test";
    p.baseUrl = baseUrl;
    p.apiKeyPlain = "sk-ant-stress-key";
    p.model = "claude-sonnet-4-5";
    p.isActive = true;
    return p;
}

// 把 Result<unique_ptr<T>> 里的指针 move 出来（避免拷贝）
std::unique_ptr<platform::http::StreamingResponse> takeStream(
    core::Result<std::unique_ptr<platform::http::StreamingResponse>>& r) {
    if (r.isFail()) return nullptr;
    auto& ref = r.value();
    return std::unique_ptr<platform::http::StreamingResponse>(std::move(ref));
}

// 读完整个流；返回最后的 ChunkResult（End / Error），数据拼进 out
platform::http::ChunkResult drainStreamResult(
    platform::http::StreamingResponse& sr, std::string& out, std::string& err) {
    int guard = 0;
    while (guard++ < 2000000) {
        std::string chunk;
        const auto cr = sr.readChunk(chunk, err);
        if (cr == platform::http::ChunkResult::More) {
            out += chunk;
            continue;
        }
        return cr;
    }
    err = "drain guard limit hit";
    return platform::http::ChunkResult::Error;
}

// 用 cpp-httplib Client 发带 content receiver 的 POST（客户端流式接收）
struct ClientStreamResult {
    bool ok = false;
    int status = 0;
    std::string contentType;
    std::string body;
    size_t receiverCalls = 0;
};

ClientStreamResult clientPostStream(httplib::Client& cli,
                                    const std::string& path,
                                    const std::string& body) {
    ClientStreamResult out;

    httplib::Request req;
    req.method = "POST";
    req.path = path;
    req.body = body;
    req.headers.emplace("Content-Type", "application/json");

    req.response_handler = [&out](const httplib::Response& r) {
        out.status = r.status;
        auto it = r.headers.find("Content-Type");
        if (it != r.headers.end()) out.contentType = it->second;
        return true;
    };
    req.content_receiver = [&out](const char* data, size_t len,
                                  uint64_t /*offset*/, uint64_t /*total*/) {
        out.body.append(data, len);
        ++out.receiverCalls;
        return true;
    };

    auto res = cli.send(req);
    out.ok = static_cast<bool>(res);
    return out;
}

// ---------- mock 上游：大 body（流式 + 缓冲两种供给方式）----------

class StressUpstream {
public:
    StressUpstream() = default;
    ~StressUpstream() { stop(); }
    StressUpstream(const StressUpstream&) = delete;
    StressUpstream& operator=(const StressUpstream&) = delete;

    bool start() {
        port_ = svr_.bind_to_any_port("127.0.0.1");
        if (port_ <= 0) return false;
        thread_ = std::thread([this] { svr_.listen_after_bind(); });
        svr_.wait_until_ready();
        return true;
    }

    void stop() {
        svr_.stop();
        if (thread_.joinable()) thread_.join();
    }

    int port() const { return port_; }
    std::string baseUrl() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }

    // POST /v1/messages：chunked 逐块发（流式路径用）
    // 注意：idx 必须在 handler 内创建（每请求独立），注册期创建会被
    // 多个请求共享——第 2 个请求一开始就 idx==size，直接回空 200。
    void onStreamBlocks(std::vector<std::string> blocks) {
        auto blocksPtr =
            std::make_shared<std::vector<std::string>>(std::move(blocks));
        svr_.Post("/v1/messages",
                  [blocksPtr](const httplib::Request&, httplib::Response& res) {
            res.status = 200;
            auto idx = std::make_shared<size_t>(0);
            res.set_chunked_content_provider(
                "text/event-stream",
                [blocksPtr, idx](size_t /*offset*/,
                                 httplib::DataSink& sink) -> bool {
                    if (*idx >= blocksPtr->size()) {
                        sink.done();   // 写 0\r\n\r\n：干净收尾
                        return true;   // 让写循环按 data_available=false 退出
                    }
                    const std::string& b = (*blocksPtr)[*idx];
                    if (!sink.write(b.data(), b.size())) return false;
                    ++(*idx);
                    return true;
                });
        });
    }

    // GET /v1/models：缓冲一次性回整段（缓冲路径用）
    void onBufferedBody(const std::string& body) {
        svr_.Get("/v1/models",
                 [body](const httplib::Request&, httplib::Response& res) {
            res.set_content(body, "application/json");
        });
    }

private:
    httplib::Server svr_;
    std::thread thread_;
    int port_ = 0;
};

// ---------- mock 上游：固定节奏 soak ----------

class SoakUpstream {
public:
    SoakUpstream() = default;
    ~SoakUpstream() { stop(); }
    SoakUpstream(const SoakUpstream&) = delete;
    SoakUpstream& operator=(const SoakUpstream&) = delete;

    bool start() {
        port_ = svr_.bind_to_any_port("127.0.0.1");
        if (port_ <= 0) return false;
        thread_ = std::thread([this] { svr_.listen_after_bind(); });
        svr_.wait_until_ready();
        return true;
    }

    void stop() {
        svr_.stop();
        if (thread_.joinable()) thread_.join();
    }

    std::string baseUrl() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }

    // POST /v1/messages：每 intervalMs 发一个 seq chunk，共 count 个
    //（seq 在 handler 内创建：每请求独立；sent_ 复用同一 atomic，请求开始清零）
    void onPacedChunks(long intervalMs, size_t count) {
        svr_.Post("/v1/messages",
                  [sent = sent_, intervalMs, count](const httplib::Request&,
                                                    httplib::Response& res) {
            res.status = 200;
            sent->store(0);
            auto seq = std::make_shared<std::atomic<size_t>>(0);
            res.set_chunked_content_provider(
                "text/event-stream",
                [seq, sent, intervalMs, count](size_t /*offset*/,
                                               httplib::DataSink& sink) -> bool {
                    const size_t i = seq->load();
                    if (i >= count) {
                        sink.done();
                        return true;
                    }
                    std::string c =
                        "data: {\"seq\":" + std::to_string(i) + "}\n\n";
                    if (!sink.write(c.data(), c.size())) return false;
                    seq->store(i + 1);
                    sent->store(i + 1);
                    if (intervalMs > 0) {
                        std::this_thread::sleep_for(milliseconds(intervalMs));
                    }
                    return true;
                });
        });
    }

    size_t sent() const { return sent_ ? sent_->load() : 0; }

private:
    httplib::Server svr_;
    std::thread thread_;
    int port_ = 0;
    std::shared_ptr<std::atomic<size_t>> sent_ =
        std::make_shared<std::atomic<size_t>>(0);
};

// ---------- 代理环境（ProxyRouter + InMemoryDb 内存库，不触碰 Keychain）----------

// 模块 glue，keep-local（TIN-06）：InMemoryDb 基座 + ClaudeService 构造。
struct StressEnv {
    test_support::InMemoryDb db;
    std::unique_ptr<module::claude::ClaudeService> service;
    std::unique_ptr<platform::http::Server> proxy;

    bool start(const std::string& upstreamBase) {
        service = std::make_unique<module::claude::ClaudeService>(
            db.handle());
        proxy = std::make_unique<platform::http::Server>("127.0.0.1", 0);
        router_ = std::make_unique<module::claude::ProxyRouter>(*service, *proxy);
        router_->setProfileProvider([upstreamBase]() {
            return core::Result<Profile>::ok(makeProfile(upstreamBase));
        });
        router_->registerRoutes();
        return proxy->start();
    }

    void stop() {
        if (proxy) proxy->stop();
    }

    int port() const { return proxy->actualPort(); }

private:
    // 声明在最后：析构顺序 router → proxy → service → db（handler 捕获
    // router 的 this，必须先于 proxy 销毁；server stop 后不再有调用）
    std::unique_ptr<module::claude::ProxyRouter> router_;
};

// ---------- 原始 socket 上游（断连恢复用）----------

#ifdef _WIN32
using RawSocket = SOCKET;
using SockLen = int;
inline void closeRawSocket(RawSocket s) { closesocket(s); }
#else
using RawSocket = int;
using SockLen = socklen_t;
inline void closeRawSocket(RawSocket s) { ::close(s); }
#endif

#ifdef _WIN32
bool ensureWinsock() {
    static const bool ok = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    return ok;
}
#else
bool ensureWinsock() { return true; }
#endif

// select 等待 socket 可读（带超时）
bool waitReadable(RawSocket s, int timeoutMs) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(s, &rfds);
    timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
#ifdef _WIN32
    const int rc = select(0, &rfds, nullptr, nullptr, &tv);
#else
    const int rc = select(static_cast<int>(s) + 1, &rfds, nullptr, nullptr, &tv);
#endif
    return rc > 0;
}

bool sendAll(RawSocket s, const char* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        const int n = static_cast<int>(
            send(s, data + off, static_cast<int>(len - off), 0));
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

// 从请求头里解析 Content-Length（找不到返回 0）
size_t parseContentLength(const std::string& headers) {
    const size_t p = headers.find("Content-Length:");
    if (p == std::string::npos) return 0;
    size_t i = p + 15;
    while (i < headers.size() && headers[i] == ' ') ++i;
    size_t v = 0;
    while (i < headers.size() && headers[i] >= '0' && headers[i] <= '9') {
        v = v * 10 + static_cast<size_t>(headers[i] - '0');
        ++i;
    }
    return v;
}

// 原始 socket mock 上游：收一个请求，按节奏回 chunked SSE，
// 发完 chunkCount 个后【不发终止 chunk】直接 closesocket——模拟上游中途断连。
class AbruptCloseUpstream {
public:
    AbruptCloseUpstream() = default;
    ~AbruptCloseUpstream() { stop(); }
    AbruptCloseUpstream(const AbruptCloseUpstream&) = delete;
    AbruptCloseUpstream& operator=(const AbruptCloseUpstream&) = delete;

    bool start(int chunkCount, int gapMs) {
        if (!ensureWinsock()) return false;
        listen_ = socket(AF_INET, SOCK_STREAM, 0);
#ifdef _WIN32
        if (listen_ == INVALID_SOCKET) return false;
#else
        if (listen_ < 0) return false;
#endif
        sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(0x7F000001u);   // 127.0.0.1
        addr.sin_port = 0;
        if (bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            closeRawSocket(listen_);
            listen_ = kClosed;
            return false;
        }
        if (listen(listen_, 1) != 0) {
            closeRawSocket(listen_);
            listen_ = kClosed;
            return false;
        }
        sockaddr_in bound;
        SockLen blen = sizeof(bound);
        if (getsockname(listen_, reinterpret_cast<sockaddr*>(&bound), &blen) != 0) {
            closeRawSocket(listen_);
            listen_ = kClosed;
            return false;
        }
        port_ = ntohs(bound.sin_port);
        thread_ = std::thread([this, chunkCount, gapMs] { run(chunkCount, gapMs); });
        return true;
    }

    void stop() {
        if (listen_ != kClosed) {
            closeRawSocket(listen_);
            listen_ = kClosed;
        }
        if (thread_.joinable()) thread_.join();
    }

    int port() const { return port_; }
    std::string baseUrl() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }
    // 上游实际发出的 chunk 载荷（用于和客户端收到的比对）
    const std::string& sentPayload() const { return sentPayload_; }
    bool closed() const { return closed_.load(); }

private:
    void run(int chunkCount, int gapMs) {
        // 10s 内等一个连接（stop() 关掉 listen socket 时 select 出错退出）
        if (!waitReadable(listen_, 10000)) return;
        sockaddr_in peer;
        SockLen peerLen = sizeof(peer);
        RawSocket c =
            accept(listen_, reinterpret_cast<sockaddr*>(&peer), &peerLen);
#ifdef _WIN32
        if (c == INVALID_SOCKET) return;
#else
        if (c < 0) return;
#endif

        // 接收超时兜底：客户端不发请求时 recv 不会永久挂死
#ifdef _WIN32
        DWORD rcvMs = 5000;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&rcvMs), sizeof(rcvMs));
#else
        timeval rtv;
        rtv.tv_sec = 5;
        rtv.tv_usec = 0;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&rtv), sizeof(rtv));
#endif

        // 读请求（头 + Content-Length 的 body——读干净再回，避免未读数据触发 RST）
        std::string reqBuf;
        size_t headerEnd = std::string::npos;
        size_t contentLen = 0;
        for (int guard = 0; guard < 200; ++guard) {
            char buf[4096];
            const int n = recv(c, buf, sizeof(buf), 0);
            if (n <= 0) break;
            reqBuf.append(buf, static_cast<size_t>(n));
            if (headerEnd == std::string::npos) {
                headerEnd = reqBuf.find("\r\n\r\n");
                if (headerEnd != std::string::npos) {
                    contentLen = parseContentLength(
                        reqBuf.substr(0, headerEnd));
                }
            }
            if (headerEnd != std::string::npos &&
                reqBuf.size() >= headerEnd + 4 + contentLen) {
                break;
            }
        }

        // 回 chunked SSE 响应头
        static const char kHead[] =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Transfer-Encoding: chunked\r\n"
            "\r\n";
        if (!sendAll(c, kHead, sizeof(kHead) - 1)) {
            closeRawSocket(c);
            return;
        }

        // 逐 chunk（带节奏）发，记录已发载荷
        for (int i = 0; i < chunkCount; ++i) {
            std::string payload =
                "data: {\"seq\":" + std::to_string(i) + "}\n\n";
            char head[32];
            std::snprintf(head, sizeof(head), "%zx\r\n",
                          static_cast<size_t>(payload.size()));
            std::string framed =
                std::string(head) + payload + "\r\n";
            if (!sendAll(c, framed.data(), framed.size())) {
                closeRawSocket(c);
                return;
            }
            sentPayload_ += payload;
            if (gapMs > 0) std::this_thread::sleep_for(milliseconds(gapMs));
        }

        // 传输中途断连：不给 0\r\n\r\n 终止 chunk，直接关 socket。
        // 稍等一下确保客户端确实在"等下一个 chunk"的状态中被断。
        std::this_thread::sleep_for(milliseconds(200));
        closeRawSocket(c);
        closed_.store(true);
    }

    static constexpr RawSocket kClosed =
#ifdef _WIN32
        INVALID_SOCKET;
#else
        -1;
#endif
    RawSocket listen_ = kClosed;
    std::thread thread_;
    int port_ = 0;
    std::string sentPayload_;
    std::atomic<bool> closed_{false};
};

// ---------- 内存采样（Windows psapi）----------

// 读本进程当前工作集内存（KB）；失败返回 -1。
// psapi GetProcessMemoryInfo 是进程内 Win32 API：不 shell、不解析文本，
// 每毫秒级返回。旧实现 _popen("tasklist ...") 每轮起子进程，本机 tasklist
// 卡/慢且偶发解析失败 → 返回 0 且把用例拖到 300s+，故弃用。
// MinGW 下 psapi.h 在 NTDDI_VERSION >= WIN7 时把 GetProcessMemoryInfo
// 宏转发到 kernel32 的 K32GetProcessMemoryInfo，默认链接集即可用，无需 -lpsapi。
// [[maybe_unused]]：本函数唯一的调用点在 `#ifdef _WIN32` 块内（stressMemoryBody），
// 非 Windows 编译时函数体走 `#else return -1`，无人调用 → clang
// -Wunused-function 报错（GCC -Wall 不报此告警，故 Windows 侧从不复现）。
// 保留函数而非按平台围 #ifdef：Windows 行为零变化，macOS 侧结构也零变化。
[[maybe_unused]] long long selfMemoryKb() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return -1;
    }
    const long long kb =
        static_cast<long long>(pmc.WorkingSetSize / 1024);
    return kb > 0 ? kb : -1;
#else
    return -1;
#endif
}

// ---------- soak 实现（60s / 300s 共用）----------

// 返回 false 时已打印失败原因。DOCTEST_* 宏在 helper 里 fail 即 return false。
bool soakRun(int durationSec) {
    const long intervalMs = 50;
    const size_t count =
        static_cast<size_t>((static_cast<long long>(durationSec) * 1000) /
                            intervalMs);

    SoakUpstream upstream;
    DOCTEST_REQUIRE(upstream.start());
    upstream.onPacedChunks(intervalMs, count);

    platform::http::Request req;
    req.method = "POST";
    req.url = "/v1/messages";
    req.body = "{\"stream\":true}";
    // 2 倍时长 + 60s 余量（Windows sleep 粒度可能让实际节奏偏慢）
    req.timeoutMs = durationSec * 2000 + 60000;

    auto r = module::claude::sendStreamingClaude(req,
                                                 makeProfile(upstream.baseUrl()));
    DOCTEST_REQUIRE(r.isOk());
    auto sr = takeStream(r);
    if (!sr) return false;

    std::string all;
    std::string err;
    const auto t0 = steady_clock::now();
    const auto last = drainStreamResult(*sr, all, err);
    const long elapsedMs =
        duration_cast<milliseconds>(steady_clock::now() - t0).count();

    DOCTEST_CHECK_MESSAGE(last == platform::http::ChunkResult::End,
                          "soak ended with error: " + err);
    DOCTEST_CHECK(err.empty());

    // 解析 seq 序列：必须 0,1,2,... 严格递增（顺序 + 无重复）
    size_t received = 0;
    size_t pos = 0;
    while (true) {
        const size_t hit = all.find("data: {\"seq\":", pos);
        if (hit == std::string::npos) break;
        const size_t numStart = hit + 13;
        size_t numEnd = numStart;
        while (numEnd < all.size() && all[numEnd] >= '0' && all[numEnd] <= '9') {
            ++numEnd;
        }
        if (numEnd == numStart || numEnd >= all.size() || all[numEnd] != '}') {
            std::cerr << "soak: malformed event at offset " << hit << std::endl;
            return false;
        }
        const unsigned long long seqV = std::strtoull(
            all.substr(numStart, numEnd - numStart).c_str(), nullptr, 10);
        if (seqV != received) {
            std::cerr << "soak: seq out of order: got " << seqV
                      << " expected " << received << std::endl;
            return false;
        }
        ++received;
        pos = numEnd;
    }

    std::cout << "[stress] soak " << durationSec << "s: received " << received
              << " chunks, upstream sent " << upstream.sent() << ", elapsed "
              << elapsedMs << "ms" << std::endl;

    // 验收口径：收到 chunk 数误差 < 5%（期望 = 时长 / 50ms）
    const long long expected = static_cast<long long>(count);
    const long long got = static_cast<long long>(received);
    const long long diff = got > expected ? got - expected : expected - got;
    DOCTEST_CHECK_MESSAGE(diff * 100 <= expected * 5,
                          "soak chunk count error " +
                              std::to_string(diff * 100 / (expected > 0 ? expected : 1)) +
                              "% > 5%");
    // 更强的无丢失断言：TCP + 干净结束 → 必须收到上游发出的每一个 chunk
    DOCTEST_CHECK_EQ(received, upstream.sent());
    // 兜底：至少收到了九成（防止上游根本没节奏地发）
    DOCTEST_CHECK(got * 10 >= expected * 9);

    upstream.stop();
    return true;
}

// ---------- 内存采样用例的 body（需要 #ifdef，不能放进宏参数）----------

#ifdef _WIN32
bool stressMemoryBody() {
    std::vector<std::string> blocks = makeBlocks();
    const std::string expected = joinBlocks(blocks);

    StressUpstream upstream;
    if (!upstream.start()) return false;
    upstream.onStreamBlocks(blocks);

    StressEnv env;
    if (!env.start(upstream.baseUrl())) return false;

    httplib::Client cli("127.0.0.1", env.port());
    cli.set_read_timeout(120, 0);
    cli.set_write_timeout(60, 0);

    const long long m0 = selfMemoryKb();
    DOCTEST_CHECK(m0 > 0);
    std::cout << "[stress] mem sample round 0: " << m0 << " KB" << std::endl;

    const auto t0 = steady_clock::now();
    long long mLast = m0;
    for (int round = 1; round <= 20; ++round) {
        ClientStreamResult res =
            clientPostStream(cli, "/v1/messages", "{\"stream\":true}");
        DOCTEST_REQUIRE_MESSAGE(res.ok && res.status == 200,
                                "round " + std::to_string(round) +
                                    ": request failed");
        if (res.body.size() != expected.size() || res.body != expected) {
            std::cerr << "round " << round << ": got " << res.body.size()
                      << " bytes (expected " << expected.size() << ")"
                      << ", status=" << res.status << ", ok=" << res.ok
                      << ", receiverCalls=" << res.receiverCalls
                      << ", head=[" << res.body.substr(0, 48) << "] tail=["
                      << res.body.substr(res.body.size() > 48 ? res.body.size() - 48 : 0)
                      << "]" << std::endl;
        }
        DOCTEST_CHECK_EQ(res.body.size(), expected.size());
        DOCTEST_CHECK_MESSAGE(res.body == expected,
                              "round " + std::to_string(round) +
                                  ": body mismatch");
        if (round % 5 == 0) {
            mLast = selfMemoryKb();
            std::cout << "[stress] mem sample round " << round << ": " << mLast
                      << " KB" << std::endl;
        }
    }
    const long elapsedMs =
        duration_cast<milliseconds>(steady_clock::now() - t0).count();

    const long long deltaKb = mLast - m0;
    std::cout << "[stress] 20 rounds x 10MB streaming: " << elapsedMs
              << "ms total, memory delta " << deltaKb
              << " KB (limit 102400 KB)" << std::endl;

    DOCTEST_CHECK(mLast > 0);
    // 宽松阈值 100MB：防泄漏而非精确测量（正常应稳定在个位数 MB 增量）
    DOCTEST_CHECK(deltaKb < 102400);

    env.stop();
    upstream.stop();
    return true;
}
#else
bool stressMemoryBody() {
    DOCTEST_SKIP("stress: memory sampling is Windows-only (psapi)");
}
#endif

} // namespace

// ==================== 1. 10MB 大 body：流式端到端透传 ====================

REGISTER_TEST(stress_body_10mb_streaming_e2e,
              "stress: 10MB body streaming end-to-end passthrough", {
    std::vector<std::string> blocks = makeBlocks();
    const std::string expected = joinBlocks(blocks);
    const uint64_t expectedHash = fnv1a64(expected);

    StressUpstream upstream;
    DOCTEST_REQUIRE(upstream.start());
    upstream.onStreamBlocks(blocks);

    StressEnv env;
    DOCTEST_REQUIRE(env.start(upstream.baseUrl()));

    httplib::Client cli("127.0.0.1", env.port());
    cli.set_read_timeout(120, 0);
    cli.set_write_timeout(60, 0);

    const auto t0 = steady_clock::now();
    ClientStreamResult res =
        clientPostStream(cli, "/v1/messages", "{\"stream\":true}");
    const long elapsedMs =
        duration_cast<milliseconds>(steady_clock::now() - t0).count();

    DOCTEST_CHECK(res.ok);
    DOCTEST_CHECK_EQ(res.status, 200);
    DOCTEST_CHECK(res.contentType.find("text/event-stream") !=
                  std::string::npos);
    // 字节数完整
    DOCTEST_CHECK_EQ(res.body.size(), expected.size());
    // 顺序正确（全串比对，任何丢/乱/改字节都会挂）
    DOCTEST_CHECK_MESSAGE(res.body == expected,
                          "10MB streaming body mismatch, got " +
                              std::to_string(res.body.size()) + " bytes");
    // 累计哈希（第二重校验 + 留档数字）
    DOCTEST_CHECK_EQ(fnv1a64(res.body), expectedHash);
    // 块序独立校验：每 64KB 块的前 8 字节是块号
    bool blockOrderOk = true;
    for (size_t i = 0; i < kBlockCount; ++i) {
        uint64_t prefix = 0;
        std::memcpy(&prefix, res.body.data() + i * kBlockSize, sizeof(prefix));
        if (prefix != static_cast<uint64_t>(i)) {
            blockOrderOk = false;
            break;
        }
    }
    DOCTEST_CHECK(blockOrderOk);

    std::cout << "[stress] 10MB streaming e2e: " << elapsedMs << "ms, "
              << res.receiverCalls << " receiver calls" << std::endl;

    env.stop();
    upstream.stop();
    return true;
})

// ==================== 2. 10MB 大 body：缓冲（非流式）路径 ====================

REGISTER_TEST(stress_body_10mb_buffered_path,
              "stress: 10MB body buffered (non-streaming) path", {
    std::vector<std::string> blocks = makeBlocks();
    const std::string expected = joinBlocks(blocks);
    const uint64_t expectedHash = fnv1a64(expected);

    StressUpstream upstream;
    DOCTEST_REQUIRE(upstream.start());
    upstream.onBufferedBody(expected);

    StressEnv env;
    DOCTEST_REQUIRE(env.start(upstream.baseUrl()));

    httplib::Client cli("127.0.0.1", env.port());
    cli.set_read_timeout(120, 0);
    cli.set_write_timeout(60, 0);

    const auto t0 = steady_clock::now();
    auto res = cli.Get("/v1/models");
    const long elapsedMs =
        duration_cast<milliseconds>(steady_clock::now() - t0).count();

    DOCTEST_REQUIRE(res);
    DOCTEST_CHECK_EQ(res->status, 200);
    DOCTEST_CHECK_EQ(res->body.size(), expected.size());
    DOCTEST_CHECK(res->body == expected);
    DOCTEST_CHECK_EQ(fnv1a64(res->body), expectedHash);

    std::cout << "[stress] 10MB buffered path: " << elapsedMs << "ms"
              << std::endl;

    env.stop();
    upstream.stop();
    return true;
})

// ==================== 3. soak 60s（常跑）====================

REGISTER_TEST(stress_soak_60s, "stress: soak 60s paced chunks all received", {
    if (!std::getenv("DEVDESK_SOAK")) DOCTEST_SKIP("DEVDESK_SOAK=1 未启用");
    return soakRun(60);
})

// ==================== 4. soak 300s（DEVDESK_SOAK=1 门控）====================

REGISTER_TEST(stress_soak_300s_gated,
              "stress: soak 300s full run (DEVDESK_SOAK=1)", {
    const char* gate = std::getenv("DEVDESK_SOAK");
    const bool enabled =
        gate != nullptr && *gate != '\0' && std::strcmp(gate, "0") != 0;
    if (!enabled) {
        DOCTEST_SKIP(
            "stress: soak 300s 需要 DEVDESK_SOAK=1（常规套件不背 5 分钟）");
    }
    return soakRun(300);
})

// ==================== 5. 上游断连恢复 ====================

REGISTER_TEST(stress_upstream_disconnect_mid_stream,
              "stress: upstream disconnect mid-stream ends clean", {
    // ---- 直接层：StreamingResponse 遇上游中途断连 → End（非 Error）+ 数据完整 ----
    AbruptCloseUpstream raw1;
    DOCTEST_REQUIRE(raw1.start(5, 150));

    platform::http::Request req;
    req.method = "POST";
    req.url = "/v1/messages";
    req.body = "{\"stream\":true}";
    req.timeoutMs = 20000;

    auto r = module::claude::sendStreamingClaude(req,
                                                 makeProfile(raw1.baseUrl()));
    DOCTEST_REQUIRE(r.isOk());
    auto sr = takeStream(r);
    DOCTEST_REQUIRE(sr != nullptr);

    std::string got;
    std::string err;
    const auto last = drainStreamResult(*sr, got, err);

    DOCTEST_CHECK(raw1.closed() ||
                  waitUntil([&raw1] { return raw1.closed(); }, 5000));
    // 断连后流必须以 End 干净收尾，不是 Error
    DOCTEST_CHECK(last == platform::http::ChunkResult::End);
    DOCTEST_CHECK(err.empty());
    // 已收数据完整：与上游实际发出的字节完全一致
    DOCTEST_CHECK_EQ(got, raw1.sentPayload());
    DOCTEST_CHECK(got.size() > 0);

    raw1.stop();

    // ---- 端到端：经 ProxyRouter，客户端看到的是干净收尾的完整响应 ----
    AbruptCloseUpstream raw2;
    DOCTEST_REQUIRE(raw2.start(5, 150));

    StressEnv env;
    DOCTEST_REQUIRE(env.start(raw2.baseUrl()));

    httplib::Client cli("127.0.0.1", env.port());
    cli.set_read_timeout(30, 0);
    cli.set_write_timeout(30, 0);

    ClientStreamResult res =
        clientPostStream(cli, "/v1/messages", "{\"stream\":true}");

    DOCTEST_CHECK(raw2.closed() ||
                  waitUntil([&raw2] { return raw2.closed(); }, 5000));
    DOCTEST_CHECK(res.ok);
    DOCTEST_CHECK_EQ(res.status, 200);
    DOCTEST_CHECK(res.contentType.find("text/event-stream") !=
                  std::string::npos);
    DOCTEST_CHECK_EQ(res.body, raw2.sentPayload());

    env.stop();
    raw2.stop();
    return true;
})

// ==================== 6. 内存采样：10MB 流式 × 20 轮 ====================

REGISTER_TEST(stress_memory_20_rounds_10mb,
              "stress: memory bounded over 20x10MB streams", {
    return stressMemoryBody();
})
