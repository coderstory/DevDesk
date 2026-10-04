// src/module_claude_proxy/proxy_router.cpp
// ProxyRouter 实现：本地 HTTP 代理 → Claude 上游 API
//（stream:true + /v1/messages → SSE 流式转发；其余 → 缓冲转发）
#include "proxy_router.h"

#include "core/database.h"
#include "core/logging.h"
#include "platform/http_client.h"
#include "streaming_proxy.h"
#include "upstream_dialect.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <ctime>
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
        // cpp-httplib 0.18.x 注入的本地连接伪头（0.58.0 起上游侧主动禁止）；剥离是
        // 纵深防御——转发无意义且泄漏本机地址/端口、干扰网关头校验
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

// 取 profile 失败的统一出口：两条转发路径共用同一状态码与文案，保证同一失败在
// stream:false / stream:true 下响应逐字一致（审计 CPX-10 合并后恒
// 503/no_active_profile）。logTag 只进日志，不进响应体。
void writeNoProfileError(platform::http::ResponseWriter& out,
                         ClaudeService& service,
                         const std::string& logTag,
                         const std::string& message) {
    LOG_WARN(logTag + "取 profile 失败 (" + message + ")");
    writeErrorJson(out, 503, "no_active_profile", message);
    service.recordProxyEvent(503);
}

// 取本连接最后一次 INSERT 的 rowid（调用方须持 db.mutex()，见调用点注释）。
// 失败返回 0 —— 0 不是合法 rowid，调用方据此跳过用量回填（降级，不报错）。
int64_t lastInsertRowId(core::data::SqliteDb& db) {
    auto prep = db.prepare("SELECT last_insert_rowid() AS id");
    if (prep.isFail()) return 0;
    auto rows = prep.value().fetchAll();
    if (rows.isFail() || rows.value().empty()) return 0;
    auto id = rows.value()[0].getInt64("id");
    return id.isOk() ? id.value() : 0;
}

// 流末回填 claude_call_logs 的用量列：-1（没拿到）一律写 NULL——
// 与真实的 0 区分（"没采集到" ≠ "0 个 token"）
void writeCallLogUsage(core::data::SqliteDb& db, int64_t callLogId,
                       const StreamUsage& u) {
    auto prep = db.prepare(
        "UPDATE claude_call_logs SET input_tokens = ?, output_tokens = ?, "
        "cache_read_tokens = ?, cache_write_tokens = ?, first_event_ms = ? "
        "WHERE id = ?");
    if (prep.isFail()) return;
    auto& stmt = prep.value();
    const auto bindMaybeNull = [&stmt](int idx, int64_t v) {
        if (v < 0) {
            stmt.bind(idx, nullptr);
        } else {
            stmt.bind(idx, v);
        }
    };
    bindMaybeNull(1, u.inputTokens);
    bindMaybeNull(2, u.outputTokens);
    bindMaybeNull(3, u.cacheReadTokens);
    bindMaybeNull(4, u.cacheWriteTokens);
    bindMaybeNull(5, u.firstEventMs);
    stmt.bind(6, callLogId);
    auto stepR = stmt.step();
    if (stepR.isFail()) {
        LOG_WARN("流式代理：回填用量列失败: " + stepR.errorMessage());
    }
}

} // namespace

ProxyRouter::ProxyRouter(ClaudeService& service, platform::http::Server& server)
    : service_(service), server_(server) {}

void ProxyRouter::registerRoutes() {
    // 注册常见 method。必须用 const char*——initializer_list<const char*> 绑
    // std::string 引用会创建临时，GCC 16 -Werror(dangling-reference) 直接挂
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
    // 唯一入口 resolveProfile：注入路径与生产路径都在它内部收口，与流式路径共用
    // 同一失败语义（见 writeNoProfileError）。
    auto profileR = resolveProfile();
    if (profileR.isFail()) {
        writeNoProfileError(out, service_, "代理：", profileR.errorMessage());
        return;
    }
    Profile profile = std::move(profileR).value();
    Profile servingProfile = profile;   // 实际服务的那家（failover 换家后 = 备用）

    // ---- 2/3. 构造上游 + 转发（failover 单跳）----
    // 换家判据 = shouldFailoverTo(statusCode, errorKind)（send 失败或状态码命中
    // 换家表）；有可用备用则换家重试一次，两次都失败 → 返回第二次的结果（错误
    // 更相关）。注入模式（profileProvider_ 已设）显式跳过换家。单次上游请求 =
    // 头复制 + 方言归一，换家后按备用重算 forBaseUrl 与 upstream.url。
    const auto buildUpstream = [&](const Profile& p) {
        platform::http::Request up;
        up.method = method;
        up.url = stripTrailingSlash(p.baseUrl) + ensureLeadingSlash(path);
        up.body = body;
        up.timeoutMs = 120000;     // 2 分钟
        up.verifyTls = true;

        auto fwd = copyHeaders(headers);
        const UpstreamDialect dialect =
            UpstreamDialect::forBaseUrl(p.baseUrl, p.dialectOverride);
        dialect.applyHeaders(fwd, p.apiKeyPlain, authFieldFromText(p.authField));
        up.headers = std::move(fwd);
        if (dialect.normalizeBody(up.body)) {
            LOG_INFO("代理：按上游方言剥掉模型名 [1M] 标记（" + p.baseUrl + "）");
        }
        return up;
    };

    core::Result<platform::http::Response> respR;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (attempt > 0) {
            if (profileProvider_) break;    // 注入模式守卫：不换家（见上）
            auto fb = service_.resolveFallbackProfile(servingProfile);
            if (!fb) break;    // 无备用 / 备用失效 / 备用无 key → 原样回第一次结果
            servingProfile = std::move(*fb);
        }
        respR = platform::http::send(buildUpstream(servingProfile));
        if (attempt == 0) {
            const int sc = respR.isOk() ? respR.value().statusCode : 0;
            const std::string kind =
                respR.isOk() ? std::string() : std::string("upstream_unreachable");
            if (!shouldFailoverTo(sc, kind)) break;
            continue;    // 换家：下一圈用备用重试（只换一跳）
        }
        break;    // 第二次尝试：无论结果，这就是最终结果
    }
    auto endTime = std::chrono::steady_clock::now();
    const int64_t latencyMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();

    if (respR.isFail()) {
        // 网络失败（DNS / TLS / 连接 / 超时；第二次尝试失败 = 回第二次的结果）
        LOG_WARN("代理：上游请求失败: " + respR.errorMessage());
        writeErrorJson(out, 502, "upstream_unreachable", respR.errorMessage());

        ClaudeService::CallLogEntry entry;
        entry.profileId = servingProfile.id;
        entry.method = method;
        entry.path = path;
        entry.statusCode = 0;
        entry.latencyMs = latencyMs;
        entry.requestBytes = static_cast<int64_t>(body.size());
        entry.error = respR.errorMessage();
        entry.startedAt = startedAt;
        service_.logCall(entry);
        // 0 = 上游不可达（无响应码），与日志 entry 口径一致
        service_.recordProxyEvent(0);
        return;
    }

    auto& resp = respR.value();

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
    entry.profileId = servingProfile.id;
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
    // 气泡③：正常转发完成（4xx/5xx 原样透传，pet 侧按码分类文案）
    service_.recordProxyEvent(resp.statusCode);

    LOG_INFO("代理 " + method + " " + path + " -> " +
             std::to_string(resp.statusCode) + " (" +
             std::to_string(latencyMs) + "ms, profile=" +
             std::to_string(servingProfile.id) +
             (servingProfile.id != profile.id ? " [failover]" : "") + ")");
}

bool ProxyRouter::shouldUseStreaming(const std::string& path,
                                     const std::string& body) {
    return isStreamablePath(path) && wantsStream(body);
}

core::Result<Profile> ProxyRouter::resolveProfile() {
    // 两条转发路径唯一的取 profile 入口：失败一律经 writeNoProfileError 出同一份
    // 响应，不要在任何一条路径里再内联一套取 profile 逻辑（CPX-10）。
    //
    // 测试 / UI 注入路径：直接返回（已含 apiKeyPlain）
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

    const auto startTime = std::chrono::steady_clock::now();
    const std::time_t startedAt = std::time(nullptr);

    // ---- 1. 取 profile（含明文 API key）----
    // 与缓冲路径共用同一入口 + 同一失败出口（CPX-10）
    auto profileR = resolveProfile();
    if (profileR.isFail()) {
        writeNoProfileError(out, service_, "流式代理：", profileR.errorMessage());
        return;
    }
    Profile profile = std::move(profileR).value();
    Profile servingProfile = profile;   // 实际服务的那家（failover 换家后 = 备用）

    // ---- 2/3/4. 启动流式转发 + 等响应头（failover 单跳）----
    // 判定点 = waitForStatus 之后、写响应头之前：upstreamStatus==0 或命中换家表
    //（shouldFailoverTo，同缓冲路径）→ 确认备用可用后 cancelWithReason("failover")
    // 丢弃本次流（半截 error body 随对象销毁），用备用起一条新流（sendStreamingClaude
    // 按备用重算 baseUrl 与方言）。只换一跳；注入模式守卫同缓冲路径。
    core::Result<std::unique_ptr<platform::http::StreamingResponse>> streamR;
    std::shared_ptr<platform::http::StreamingResponse> upstreamStream;
    int upstreamStatus = 0;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (attempt > 0) {
            if (profileProvider_) break;    // 注入模式守卫：不换家（见上）
            auto fb = service_.resolveFallbackProfile(servingProfile);
            if (!fb) break;    // 无备用 → 原样回第一次结果（流未取消，body 完整）
            // 换家：丢弃本次流（半截 error body 随对象销毁）
            upstreamStream->cancelWithReason("failover");
            servingProfile = std::move(*fb);
            upstreamStream.reset();   // 旧流对象析构（cancel + join libcurl 线程）
            upstreamStatus = 0;
        }

        // 构造上游请求
        platform::http::Request upstream;
        upstream.method = method;
        upstream.url = path;              // 相对路径，sendStreamingClaude 会拼 baseUrl
        upstream.body = body;
        // SSE 长连接不用总墙钟超时（会砍断长生成），改空闲超时：无新 chunk 60s 判死。
        // libcurl 低速检测按 ~5s 滑动窗口采样，实际中止在 60-70s 间
        upstream.timeoutMs = 0;             // 0 = 无总上限（sendStreaming 语义）
        upstream.lowSpeedBytesPerSec = 1;   // 1 B/s
        upstream.lowSpeedTimeSec = 60;      // 持续 60s → CURLE_OPERATION_TIMEDOUT
        upstream.verifyTls = true;
        upstream.headers = copyHeaders(headers);
        // 头与 body 的上游方言化（鉴权 / UA / 模型名标记）由 sendStreamingClaude 统一处理

        // 启动三线程流式转发（libcurl 线程 + 队列 + backpressure）
        streamR = sendStreamingClaude(upstream, servingProfile);
        if (streamR.isFail()) {
            if (attempt == 0 &&
                shouldFailoverTo(0, "upstream_unreachable")) {
                continue;    // 换家（传输失败恒可换；注入模式守卫在下一圈入口）
            }
            break;
        }
        upstreamStream = std::shared_ptr<platform::http::StreamingResponse>(
            std::move(streamR.value()));

        // 登记到 ClaudeService 在途流表：stopProxy()/应用退出会 cancelActiveStreams()，
        // 立即解锁阻塞在 readChunk 的 provider（否则 Server::stop() 的 join 挂到流
        // 自然结束）。若停止已发起，这里被立即 cancel、waitForStatus 快速返回。
        service_.registerActiveStream(upstreamStream);

        // 等上游响应头（拿到状态码，最多 30s）
        upstreamStream->waitForStatus(30000);
        upstreamStatus = upstreamStream->statusCode();

        if (attempt == 0) {
            const std::string kind = (upstreamStatus == 0)
                ? std::string("upstream_unreachable") : std::string();
            if (shouldFailoverTo(upstreamStatus, kind)) {
                continue;    // 换家：旧流在下一圈入口 cancel
            }
        }
        break;    // 第二次尝试（或无可换状态）：这就是最终流
    }

    if (streamR.isFail()) {
        // 启动上游失败（libcurl 起线程前失败；连接层失败走 waitForStatus==0）
        LOG_WARN("流式代理：启动上游失败: " + streamR.errorMessage());
        writeErrorJson(out, 502, "upstream_unreachable", streamR.errorMessage());

        ClaudeService::CallLogEntry entry;
        entry.profileId = servingProfile.id;
        entry.method = method;
        entry.path = path;
        entry.statusCode = 0;
        entry.requestBytes = static_cast<int64_t>(body.size());
        entry.error = streamR.errorMessage();
        entry.startedAt = startedAt;
        service_.logCall(entry);
        service_.recordProxyEvent(0);  // 上游不可达（无响应码）
        return;
    }

    if (upstreamStatus == 0) {
        // 连接失败 / 超时（两次尝试都无响应头 → 回第二次的 502）：回 502
        LOG_WARN("流式代理：上游无响应头");
        out.setStatus(502);
        out.setHeader("Content-Type", "application/json");
        out.write("{\"error\":\"upstream_unreachable\",\"message\":\"上游无响应\"}");
        out.end();

        ClaudeService::CallLogEntry entry;
        entry.profileId = servingProfile.id;
        entry.method = method;
        entry.path = path;
        entry.statusCode = 502;
        entry.requestBytes = static_cast<int64_t>(body.size());
        entry.error = "no_response_header";
        entry.startedAt = startedAt;
        service_.logCall(entry);
        service_.recordProxyEvent(502);
        return;
    }

    // ---- 5. 写响应头 + 交给 chunked provider 流式回写（SSE 必须是 text/event-stream）----
    std::string contentType = upstreamStream->contentType();
    if (contentType.empty()) contentType = sse::kContentType;

    out.setStatus(upstreamStatus);
    out.setHeader("Content-Type", contentType);
    out.setHeader("Cache-Control", "no-cache");
    out.setHeader("Connection", "keep-alive");
    out.setHeader("X-Accel-Buffering", "no");   // 反向代理场景禁用缓冲

    const int64_t latencyMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startTime).count();

    ClaudeService::CallLogEntry entry;
    entry.profileId = servingProfile.id;
    entry.method = method;
    entry.path = path;
    entry.statusCode = upstreamStatus;
    entry.latencyMs = latencyMs;
    entry.requestBytes = static_cast<int64_t>(body.size());
    entry.startedAt = startedAt;

    // 用量回填：token 只有流末才知道，先插行（status/latency 已知），把 rowid
    // 交给流末回调。持 db 锁跨「插行 + 取 rowid」，并发 rowid 不会被串认（锁
    // 可重入，logCall 内部同把）。未注册 app 连接（单测）跳过取号 → 用量列留 NULL。
    int64_t callLogId = 0;
    core::data::SqliteDb* appDb = core::appDatabase();
    {
        std::unique_lock<std::recursive_mutex> lock;
        if (appDb) lock = std::unique_lock<std::recursive_mutex>(appDb->mutex());
        auto logR = service_.logCall(entry);
        if (logR.isFail()) {
            LOG_WARN("流式代理：写 claude_call_logs 失败: " + logR.errorMessage());
        } else if (appDb) {
            callLogId = lastInsertRowId(*appDb);
        }
    }

    // 流末回执（恰好一次）：token 四类 + 首事件毫秒 UPDATE 进刚插的行。
    forwardStream(out, upstreamStream, contentType,
                  [appDb, callLogId](const StreamUsage& u) {
                      if (!appDb || callLogId == 0) return;  // 降级：不写用量
                      writeCallLogUsage(*appDb, callLogId, u);
                  });

    // 气泡③：流式转发完成（4xx/5xx 上游状态码原样记录，pet 侧分类文案）
    service_.recordProxyEvent(upstreamStatus);

    LOG_INFO("流式代理 " + method + " " + path + " -> " +
             std::to_string(upstreamStatus) + " (profile=" +
             std::to_string(servingProfile.id) +
             (servingProfile.id != profile.id ? " [failover]" : "") +
             ", content-type=" + contentType + ")");
}

} // namespace module::claude
