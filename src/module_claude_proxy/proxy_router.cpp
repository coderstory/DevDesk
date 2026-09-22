// src/module_claude_proxy/proxy_router.cpp
// ProxyRouter 实现：本地 HTTP 代理 → Claude 上游 API
//   - stream:true + /v1/messages → SSE 流式转发（spec §10.4 / P10a）
//   - 其余 → 缓冲转发（P9）
#include "proxy_router.h"

#include "core/logging.h"
#include "platform/http_client.h"
#include "streaming_proxy.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <memory>
#include <string>

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
        out.emplace(kv.first, kv.second);
    }
    return out;
}

// 检查 headers 是否大小写不敏感地包含某个 key
bool hasHeader(const std::map<std::string, std::string>& headers,
               const std::string& keyLower) {
    for (const auto& kv : headers) {
        std::string k = kv.first;
        std::transform(k.begin(), k.end(), k.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (k == keyLower) return true;
    }
    return false;
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

// 统一 JSON 错误响应：message 可能含引号/反斜杠/换行（来自 errorMessage()），
// 不转义会产出非法 JSON，Claude Code 侧只会看到解析错误而非真实原因
void writeErrorJson(platform::http::ResponseWriter& out, int status,
                    const char* error, const std::string& message) {
    out.setStatus(status);
    out.setHeader("Content-Type", "application/json");
    out.write(std::string("{\"error\":\"") + error +
              "\",\"message\":\"" + jsonEscape(message) + "\"}");
    out.end();
}

} // namespace

ProxyRouter::ProxyRouter(ClaudeService& service, platform::http::Server& server)
    : service_(service), server_(server) {}

void ProxyRouter::registerRoutes() {
    // 注册常见 method（POST 必选，GET/PUT/DELETE 也常见）
    // 注意：必须用 const char*（而非 const std::string&）——
    // initializer_list<const char*> 绑 std::string 引用会创建临时，
    // GCC 16 -Werror(dangling-reference) 直接挂
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

    // ---- 0. 流式分支（spec §10.4）：POST /v1/messages + "stream": true ----
    if (shouldUseStreaming(path, body)) {
        handleStreamingRequest(method, path, headers, body, out);
        return;
    }

    const auto startTime = std::chrono::steady_clock::now();
    const std::time_t startedAt = std::time(nullptr);

    // ---- 1. 取 profile（含明文 API key）----
    // 注入路径（测试 / UI）：profileProvider_ 直接给出完整 profile（与流式
    // 路径一致），绕过 DB / Keychain——否则测试的内存库里没有 profile，
    // 缓冲转发永远 503（Windows 首跑 stream:false 用例失败根因）。
    Profile profile;
    if (profileProvider_) {
        auto profileR = resolveProfile();
        if (profileR.isFail()) {
            LOG_WARN("代理：取 profile 失败 (" + profileR.errorMessage() + ")");
            writeErrorJson(out, 503, "no_active_profile", profileR.errorMessage());
            return;
        }
        profile = std::move(profileR).value();
    } else {
        // 生产路径：SQLite 取 active profile → Keychain 取明文 key（P9 原行为）
        auto activeR = service_.getActiveProfile();
        if (activeR.isFail()) {
            LOG_WARN("代理：无激活 profile (" + activeR.errorMessage() + ")");
            writeErrorJson(out, 503, "no_active_profile", activeR.errorMessage());
            return;
        }
        profile = std::move(activeR).value();

        auto keyR = service_.loadApiKey(profile.id);
        if (keyR.isFail()) {
            LOG_ERROR("代理：读取 API key 失败: " + keyR.errorMessage());
            writeErrorJson(out, 500, "keychain_read_failed", keyR.errorMessage());

            // 记录日志（status=500, error=keychain）
            ClaudeService::CallLogEntry entry;
            entry.profileId = profile.id;
            entry.method = method;
            entry.path = path;
            entry.statusCode = 500;
            entry.error = "keychain_read_failed";
            entry.startedAt = startedAt;
            service_.logCall(entry);
            return;
        }
        profile.apiKeyPlain = std::move(keyR).value();
    }
    const std::string apiKey = profile.apiKeyPlain;

    // ---- 2. 构造上游请求 ----
    platform::http::Request upstream;
    upstream.method = method;
    upstream.url = stripTrailingSlash(profile.baseUrl) + ensureLeadingSlash(path);
    upstream.body = body;
    upstream.timeoutMs = 120000;     // 2 分钟
    upstream.verifyTls = true;

    // 复制客户端 headers（去掉 hop-by-hop / authorization）
    auto fwd = copyHeaders(headers);

    // 强制覆盖：x-api-key 用 profile 的 API key
    // 如果客户端没指定 anthropic-version，用默认
    if (!hasHeader(fwd, "anthropic-version")) {
        fwd["anthropic-version"] = "2023-06-01";
    }
    fwd["x-api-key"] = apiKey;
    fwd["Host"] = "";  // 让 libcurl 自己填

    upstream.headers = std::move(fwd);

    // ---- 3. 转发 ----
    auto respR = platform::http::send(upstream);
    auto endTime = std::chrono::steady_clock::now();
    const int64_t latencyMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();

    if (respR.isFail()) {
        // 网络失败（DNS / TLS / 连接 / 超时）
        LOG_WARN("代理：上游请求失败: " + respR.errorMessage());
        writeErrorJson(out, 502, "upstream_unreachable", respR.errorMessage());

        ClaudeService::CallLogEntry entry;
        entry.profileId = profile.id;
        entry.method = method;
        entry.path = path;
        entry.statusCode = 0;
        entry.latencyMs = latencyMs;
        entry.requestBytes = static_cast<int64_t>(body.size());
        entry.error = respR.errorMessage();
        entry.startedAt = startedAt;
        service_.logCall(entry);
        return;
    }

    auto& resp = respR.unsafeValue();

    // ---- 4. 写回客户端 ----
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

    // ---- 5. 写日志 ----
    ClaudeService::CallLogEntry entry;
    entry.profileId = profile.id;
    entry.method = method;
    entry.path = path;
    entry.statusCode = resp.statusCode;
    entry.latencyMs = latencyMs;
    entry.requestBytes = static_cast<int64_t>(body.size());
    entry.responseBytes = static_cast<int64_t>(resp.body.size());
    entry.startedAt = startedAt;
    auto logR = service_.logCall(entry);
    if (logR.isFail()) {
        LOG_WARN("代理：写 claude_call_logs 失败: " + logR.errorMessage());
    }

    LOG_INFO("代理 " + method + " " + path + " -> " +
             std::to_string(resp.statusCode) + " (" +
             std::to_string(latencyMs) + "ms, profile=" +
             std::to_string(profile.id) + ")");
}

// ==================== 流式转发（spec §10.4 / P10a）====================

bool ProxyRouter::shouldUseStreaming(const std::string& path,
                                     const std::string& body) {
    return isStreamablePath(path) && wantsStream(body);
}

core::Result<Profile> ProxyRouter::resolveProfile() {
    // 测试 / UI 注入路径：直接返回（已含 apiKeyPlain）
    if (profileProvider_) {
        return profileProvider_();
    }

    // 默认路径：SQLite 取 active profile → Keychain 取明文 key
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

    const auto startTime = std::chrono::steady_clock::now();
    const std::time_t startedAt = std::time(nullptr);

    // ---- 1. 取 profile（含明文 API key）----
    auto profileR = resolveProfile();
    if (profileR.isFail()) {
        LOG_WARN("流式代理：取 profile 失败 (" + profileR.errorMessage() + ")");
        writeErrorJson(out, 503, "no_active_profile", profileR.errorMessage());
        return;
    }
    Profile profile = std::move(profileR).value();

    // ---- 2. 构造上游请求 ----
    platform::http::Request upstream;
    upstream.method = method;
    upstream.url = path;              // 相对路径，sendStreamingClaude 会拼 baseUrl
    upstream.body = body;
    // W7 P0-3：SSE 长连接不用总墙钟超时（原 300s 会把长生成整个砍断），
    // 改空闲超时——速率 <1 B/s 持续 60s（即无新 chunk 60s）才判死。
    // 注：libcurl 低速检测按 ~5s 滑动窗口采样，实际中止在 60-70s 间
    upstream.timeoutMs = 0;             // 0 = 无总上限（sendStreaming 语义）
    upstream.lowSpeedBytesPerSec = 1;   // 1 B/s
    upstream.lowSpeedTimeSec = 60;      // 持续 60s → CURLE_OPERATION_TIMEDOUT
    upstream.verifyTls = true;
    upstream.headers = copyHeaders(headers);
    if (!hasHeader(upstream.headers, "anthropic-version")) {
        upstream.headers["anthropic-version"] = "2023-06-01";
    }

    // ---- 3. 启动三线程流式转发（libcurl 线程 + 队列 + backpressure）----
    auto streamR = sendStreamingClaude(upstream, profile);
    if (streamR.isFail()) {
        LOG_WARN("流式代理：启动上游失败: " + streamR.errorMessage());
        writeErrorJson(out, 502, "upstream_unreachable", streamR.errorMessage());

        ClaudeService::CallLogEntry entry;
        entry.profileId = profile.id;
        entry.method = method;
        entry.path = path;
        entry.statusCode = 0;
        entry.requestBytes = static_cast<int64_t>(body.size());
        entry.error = streamR.errorMessage();
        entry.startedAt = startedAt;
        service_.logCall(entry);
        return;
    }

    auto& upstreamRef = streamR.value();
    auto upstreamStream =
        std::shared_ptr<platform::http::StreamingResponse>(std::move(upstreamRef));

    // ---- 4. 等上游响应头（拿到状态码，最多 30s）----
    upstreamStream->waitForStatus(30000);
    const int upstreamStatus = upstreamStream->statusCode();

    if (upstreamStatus == 0) {
        // 连接失败 / 超时：回 502
        LOG_WARN("流式代理：上游无响应头");
        out.setStatus(502);
        out.setHeader("Content-Type", "application/json");
        out.write("{\"error\":\"upstream_unreachable\",\"message\":\"上游无响应\"}");
        out.end();

        ClaudeService::CallLogEntry entry;
        entry.profileId = profile.id;
        entry.method = method;
        entry.path = path;
        entry.statusCode = 502;
        entry.requestBytes = static_cast<int64_t>(body.size());
        entry.error = "no_response_header";
        entry.startedAt = startedAt;
        service_.logCall(entry);
        return;
    }

    // ---- 5. 写响应头 + 交给 chunked provider 流式回写 ----
    // SSE 场景 Content-Type 必须是 text/event-stream（spec §10.4）
    std::string contentType = upstreamStream->contentType();
    if (contentType.empty()) contentType = sse::kContentType;

    out.setStatus(upstreamStatus);
    out.setHeader("Content-Type", contentType);
    out.setHeader("Cache-Control", "no-cache");
    out.setHeader("Connection", "keep-alive");
    out.setHeader("X-Accel-Buffering", "no");   // 反向代理场景禁用缓冲

    forwardStream(out, upstreamStream, contentType);

    const int64_t latencyMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startTime).count();

    ClaudeService::CallLogEntry entry;
    entry.profileId = profile.id;
    entry.method = method;
    entry.path = path;
    entry.statusCode = upstreamStatus;
    entry.latencyMs = latencyMs;
    entry.requestBytes = static_cast<int64_t>(body.size());
    entry.startedAt = startedAt;
    auto logR = service_.logCall(entry);
    if (logR.isFail()) {
        LOG_WARN("流式代理：写 claude_call_logs 失败: " + logR.errorMessage());
    }

    LOG_INFO("流式代理 " + method + " " + path + " -> " +
             std::to_string(upstreamStatus) + " (profile=" +
             std::to_string(profile.id) + ", content-type=" + contentType + ")");
}

} // namespace module::claude
