// src/module_claude_proxy/proxy_router.cpp
// ProxyRouter 实现：本地 HTTP 代理 → Claude 上游 API
//（stream:true + /v1/messages → SSE 流式转发；其余 → 缓冲转发）
#include "proxy_router.h"

#include "core/logging.h"
#include "platform/http_client.h"
#include "streaming_proxy.h"
#include "upstream_dialect.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace module::claude {

namespace {

// 去除 path 末尾的 '/'
std::string stripTrailingSlash(const std::string& s) {
    if (!s.empty() && s.back() == '/') return s.substr(0, s.size() - 1);
    return s;
}

// 去除 path 前导的 '/'（用于 baseUrl + path 拼接）
std::string ensureLeadingSlash(const std::string& s) {
    if (s.empty()) return "/";
    if (s.front() != '/') return "/" + s;
    return s;
}

// 把 std::map<string,string> 复制成 platform::http::Request.headers
std::map<std::string, std::string> copyHeaders(
    const std::map<std::string, std::string>& src) {
    std::map<std::string, std::string> out;
    for (const auto& kv : src) {
        // 跳过 hop-by-hop header 和客户端鉴权
        std::string k = kv.first;
        std::transform(k.begin(), k.end(), k.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (k == "authorization" || k == "host" || k == "content-length" ||
            k == "connection" || k == "transfer-encoding") {
            continue;
        }
        // cpp-httplib 注入的本地连接伪头：转发无意义且泄漏本机地址/端口、干扰网关头校验（剥离是纵深防御）
        if (k == "remote_addr" || k == "remote_port" ||
            k == "local_addr" || k == "local_port") {
            continue;
        }
        out.emplace(kv.first, kv.second);
    }
    return out;
}

// JSON 字符串转义（RFC 8259：引号/反斜杠/控制字符；UTF-8 原样透传）
std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned>(c) & 0xFFu);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

// 统一 JSON 错误响应：message 可能含引号/换行，不转义会产出非法 JSON，客户端只见解析错误
void writeErrorJson(platform::http::ResponseWriter& out, int status,
                    const char* error, const std::string& message) {
    out.setStatus(status);
    out.setHeader("Content-Type", "application/json");
    out.write(std::string("{\"error\":\"") + error +
              "\",\"message\":\"" + jsonEscape(message) + "\"}");
    out.end();
}

// 取 profile 失败的统一出口：两条转发路径共用同一状态码与文案（恒 503/no_active_profile）；logTag 只进日志
void writeNoProfileError(platform::http::ResponseWriter& out,
                         ClaudeService& service,
                         const std::string& logTag,
                         const std::string& message) {
    LOG_WARN(logTag + "取 profile 失败 (" + message + ")");
    writeErrorJson(out, 503, "no_active_profile", message);
    service.recordProxyEvent(503);
}

} // namespace

ProxyRouter::ProxyRouter(ClaudeService& service, platform::http::Server& server)
    : service_(service), server_(server) {}

void ProxyRouter::registerRoutes() {
    // 注册常见 method。必须用 const char*：initializer_list 绑 std::string 引用会创建临时，GCC dangling-reference 判挂
    for (const char* const m : {"POST", "GET", "PUT", "DELETE", "PATCH"}) {
        // regex "/(.*)" 匹配所有路径
        server_.route(m, R"(/(.*))",
            [this](const platform::http::RequestCtx& ctx,
                   platform::http::ResponseWriter& out) {
                handleRequest(ctx.method, ctx.path, ctx.headers, ctx.body, out);
            });
    }
    LOG_INFO("ProxyRouter 已注册路由（所有 method + 任意 path）");
}

void ProxyRouter::handleRequest(
    const std::string& method,
    const std::string& path,
    const std::map<std::string, std::string>& headers,
    const std::string& body,
    platform::http::ResponseWriter& out) {

    // 0. 流式分支：POST /v1/messages + "stream": true
    if (shouldUseStreaming(path, body)) {
        handleStreamingRequest(method, path, headers, body, out);
        return;
    }

    const auto startTime = std::chrono::steady_clock::now();

    // 1. 取 profile（含明文 API key）：唯一入口 resolveProfile，与流式路径共用失败语义
    auto profileR = resolveProfile();
    if (profileR.isFail()) {
        writeNoProfileError(out, service_, "代理：", profileR.errorMessage());
        return;
    }
    Profile profile = std::move(profileR).value();

    // 2/3. 构造上游 + 转发
    const auto buildUpstream = [&](const Profile& p) {
        platform::http::Request up;
        up.method = method;
        up.url = stripTrailingSlash(p.baseUrl) + ensureLeadingSlash(path);
        up.body = body;
        up.timeoutMs = 120000;     // 2 分钟
        up.verifyTls = true;

        auto fwd = copyHeaders(headers);
        const UpstreamDialect dialect = UpstreamDialect::forBaseUrl(p.baseUrl);
        dialect.applyHeaders(fwd, p.apiKeyPlain, authFieldFromText(p.authField));
        up.headers = std::move(fwd);
        if (dialect.normalizeBody(up.body)) {
            LOG_INFO("代理：按上游方言剥掉模型名 [1M] 标记（" + p.baseUrl + "）");
        }
        return up;
    };

    core::Result<platform::http::Response> respR =
        platform::http::send(buildUpstream(profile));
    auto endTime = std::chrono::steady_clock::now();
    const int64_t latencyMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();

    if (respR.isFail()) {
        // 网络失败（DNS / TLS / 连接 / 超时）
        LOG_WARN("代理：上游请求失败: " + respR.errorMessage());
        writeErrorJson(out, 502, "upstream_unreachable", respR.errorMessage());
        // 0 = 上游不可达（无响应码）
        service_.recordProxyEvent(0);
        return;
    }

    auto& resp = respR.value();

    // 4. 写回客户端
    out.setStatus(resp.statusCode);
    bool hasContentType = false;
    for (const auto& kv : resp.headers) {
        std::string k = kv.first;
        std::transform(k.begin(), k.end(), k.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        // 跳过 hop-by-hop
        if (k == "transfer-encoding" || k == "connection" ||
            k == "content-length" || k == "keep-alive") {
            continue;
        }
        if (k == "content-type") hasContentType = true;
        out.setHeader(kv.first, kv.second);
    }
    if (!hasContentType && !resp.body.empty()) {
        out.setHeader("Content-Type", "application/json");
    }
    if (!resp.body.empty()) {
        out.write(resp.body);
    }
    out.end();

    // 正常转发完成（4xx/5xx 原样透传，pet 侧按码分类文案）
    service_.recordProxyEvent(resp.statusCode);

    LOG_INFO("代理 " + method + " " + path + " -> " +
             std::to_string(resp.statusCode) + " (" +
             std::to_string(latencyMs) + "ms, profile=" +
             std::to_string(profile.id) + ")");
}

bool ProxyRouter::shouldUseStreaming(const std::string& path,
                                     const std::string& body) {
    return isStreamablePath(path) && wantsStream(body);
}

core::Result<Profile> ProxyRouter::resolveProfile() {
    // 两条转发路径唯一的取 profile 入口：失败一律经 writeNoProfileError 出同一份响应。
    // 测试 / UI 注入路径直接返回（已含 apiKeyPlain）
    if (profileProvider_) {
        return profileProvider_();
    }

    // 默认路径：SQLite 取 active profile → 凭据列取明文 key
    auto activeR = service_.getActiveProfile();
    if (activeR.isFail()) {
        return core::Result<Profile>::fail(activeR.errorCode(),
                                          activeR.errorMessage());
    }
    Profile profile = std::move(activeR).value();

    auto keyR = service_.loadApiKey(profile.id);
    if (keyR.isFail()) {
        return core::Result<Profile>::fail(core::ErrorCode::InvalidApiKey,
            "读取 API key 失败: " + keyR.errorMessage());
    }
    profile.apiKeyPlain = std::move(keyR).value();
    return core::Result<Profile>::ok(std::move(profile));
}

void ProxyRouter::handleStreamingRequest(
    const std::string& method,
    const std::string& path,
    const std::map<std::string, std::string>& headers,
    const std::string& body,
    platform::http::ResponseWriter& out) {

    // 1. 取 profile：与缓冲路径共用同一入口 + 同一失败出口
    auto profileR = resolveProfile();
    if (profileR.isFail()) {
        writeNoProfileError(out, service_, "流式代理：", profileR.errorMessage());
        return;
    }
    Profile profile = std::move(profileR).value();

    // 2/3/4. 启动流式转发 + 等响应头
    // 构造上游请求
    platform::http::Request upstream;
    upstream.method = method;
    upstream.url = path;              // 相对路径，sendStreamingClaude 会拼 baseUrl
    upstream.body = body;
    // SSE 长连接不用总墙钟超时（会砍断长生成），改空闲超时：无新 chunk 60s 判死（libcurl 按 ~5s 窗口采样，实际中止 60-70s）
    upstream.timeoutMs = 0;             // 0 = 无总上限（sendStreaming 语义）
    upstream.lowSpeedBytesPerSec = 1;   // 1 B/s
    upstream.lowSpeedTimeSec = 60;      // 持续 60s → CURLE_OPERATION_TIMEDOUT
    upstream.verifyTls = true;
    upstream.headers = copyHeaders(headers);
    // 头与 body 的上游方言化（鉴权 / UA / 模型名标记）由 sendStreamingClaude 统一处理

    // 启动三线程流式转发（libcurl 线程 + 队列 + backpressure）
    auto streamR = sendStreamingClaude(upstream, profile);

    if (streamR.isFail()) {
        // 启动上游失败（libcurl 起线程前失败；连接层失败走 waitForStatus==0）
        LOG_WARN("流式代理：启动上游失败: " + streamR.errorMessage());
        writeErrorJson(out, 502, "upstream_unreachable", streamR.errorMessage());
        service_.recordProxyEvent(0);  // 上游不可达（无响应码）
        return;
    }

    auto upstreamStream = std::shared_ptr<platform::http::StreamingResponse>(
        std::move(streamR.value()));

    // 登记在途流：stopProxy()/退出会 cancelActiveStreams() 解锁阻塞在 readChunk 的 provider（否则 Server::stop() join 挂到流自然结束）；停止已发起时立即 cancel
    service_.registerActiveStream(upstreamStream);

    // 等上游响应头（拿到状态码，最多 30s）
    upstreamStream->waitForStatus(30000);
    const int upstreamStatus = upstreamStream->statusCode();

    if (upstreamStatus == 0) {
        // 连接失败 / 超时：回 502
        LOG_WARN("流式代理：上游无响应头");
        out.setStatus(502);
        out.setHeader("Content-Type", "application/json");
        out.write("{\"error\":\"upstream_unreachable\",\"message\":\"上游无响应\"}");
        out.end();
        service_.recordProxyEvent(502);
        return;
    }

    // 5. 写响应头 + 交给 chunked provider 流式回写（SSE 必须是 text/event-stream）
    std::string contentType = upstreamStream->contentType();
    if (contentType.empty()) contentType = sse::kContentType;

    out.setStatus(upstreamStatus);
    out.setHeader("Content-Type", contentType);
    out.setHeader("Cache-Control", "no-cache");
    out.setHeader("Connection", "keep-alive");
    out.setHeader("X-Accel-Buffering", "no");   // 反向代理场景禁用缓冲

    forwardStream(out, upstreamStream, contentType);

    // 流式转发完成（4xx/5xx 上游状态码原样记录，pet 侧分类文案）
    service_.recordProxyEvent(upstreamStatus);

    // 成功不记：代理流量大，每请求一条 INFO 会刷屏；失败路径各有独立 LOG_WARN
}

} // namespace module::claude
