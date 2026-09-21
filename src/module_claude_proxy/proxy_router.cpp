// src/module_claude_proxy/proxy_router.cpp
// ProxyRouter 实现：本地 HTTP 代理 → Claude 上游 API
#include "proxy_router.h"

#include "core/logging.h"
#include "platform/http_client.h"

#include <chrono>
#include <ctime>
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

// 查找 header 值（大小写不敏感）— 暂未使用，保留供 P10a 流式用
#if 0
std::string getHeader(const std::map<std::string, std::string>& headers,
                      const std::string& keyLower) {
    (void)headers;
    (void)keyLower;
    return std::string();
}
#endif

} // namespace

ProxyRouter::ProxyRouter(ClaudeService& service, platform::http::Server& server)
    : service_(service), server_(server) {}

void ProxyRouter::registerRoutes() {
    // 注册常见 method（POST 必选，GET/PUT/DELETE 也常见）
    for (const std::string& m : {"POST", "GET", "PUT", "DELETE", "PATCH"}) {
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

    const auto startTime = std::chrono::steady_clock::now();
    const std::time_t startedAt = std::time(nullptr);

    // ---- 1. 查 active profile ----
    auto activeR = service_.getActiveProfile();
    if (activeR.isFail()) {
        LOG_WARN("代理：无激活 profile (" + activeR.errorMessage() + ")");
        out.setStatus(503);
        out.setHeader("Content-Type", "application/json");
        out.write(std::string("{\"error\":\"no_active_profile\",\"message\":\"") +
                  activeR.errorMessage() + "\"}");
        out.end();
        return;
    }
    Profile profile = std::move(activeR).value();

    // ---- 2. 读 API key from Keychain ----
    auto keyR = service_.loadApiKey(profile.id);
    if (keyR.isFail()) {
        LOG_ERROR("代理：读取 API key 失败: " + keyR.errorMessage());
        out.setStatus(500);
        out.setHeader("Content-Type", "application/json");
        out.write(std::string("{\"error\":\"keychain_read_failed\",\"message\":\"") +
                  keyR.errorMessage() + "\"}");
        out.end();

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
    const std::string apiKey = std::move(keyR).value();

    // ---- 3. 构造上游请求 ----
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

    // ---- 4. 转发 ----
    auto respR = platform::http::send(upstream);
    auto endTime = std::chrono::steady_clock::now();
    const int64_t latencyMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();

    if (respR.isFail()) {
        // 网络失败（DNS / TLS / 连接 / 超时）
        LOG_WARN("代理：上游请求失败: " + respR.errorMessage());
        out.setStatus(502);
        out.setHeader("Content-Type", "application/json");
        out.write(std::string("{\"error\":\"upstream_unreachable\",\"message\":\"") +
                  respR.errorMessage() + "\"}");
        out.end();

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

    // ---- 5. 写回客户端 ----
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

    // ---- 6. 写日志 ----
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

} // namespace module::claude
