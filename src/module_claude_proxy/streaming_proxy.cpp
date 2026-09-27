// src/module_claude_proxy/streaming_proxy.cpp
// SSE 流式 Claude 代理实现（spec §10.4）
#include "streaming_proxy.h"

#include "core/logging.h"
#include "core/proxy_stats.h"
#include "json_scan.h"
#include "platform/json.h"
#include "upstream_dialect.h"

#include <chrono>
#include <string>

namespace module::claude {

// ---------- SSE 事件重组 + usage 解析（设计 §3） ----------

namespace {

// 从 usage 对象里取一个数值 token 字段：存在且是数字才写入 dest 并返回 true
// （累计值口径：字段缺失/非数字时保持 dest 原值，绝不用 0 冒充）
bool takeTokenField(const platform::json::JsonObject& usage, const char* key,
                    int64_t& dest) {
    const auto v = usage.get(key);
    if (!v.isNumber()) return false;
    dest = v.asInt();
    return true;
}

// 从一条完整 SSE 事件里解析累计 usage：四类 token 写进 out（缺失的不动），
// 返回本事件的累计 output tokens（没有 / 解析失败返回 -1，与旧实现同口径）。
//   - message_start：usage 嵌在 message 里（message.usage）
//   - message_delta：usage 在顶层（usage）
int64_t parseUsageFromEvent(const std::string& event, StreamUsage& out) {
    // 逐行找 "data:" 行（事件行以 \n 分隔，容忍 \r 结尾）
    size_t lineStart = 0;
    while (lineStart <= event.size()) {
        const size_t lineEnd = event.find('\n', lineStart);
        const size_t end = (lineEnd == std::string::npos) ? event.size() : lineEnd;
        std::string line = event.substr(lineStart, end - lineStart);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.compare(0, 5, "data:") == 0) {
            std::string payload = json_scan::trim(line.substr(5));
            if (payload == "[DONE]" || payload.empty()) return -1;
            do { // 单次 do：用 break 模拟 goto skip
                auto r = platform::json::parse(payload);
                if (r.isFail() || !r.value().isObject()) break;
                platform::json::JsonObject obj = r.value().asObject();
                const auto type = obj.get("type");
                if (!type.isString()) break;

                platform::json::JsonObject usage;
                bool haveUsage = false;
                if (type.asString() == "message_start") {
                    const auto msg = obj.get("message");
                    if (msg.isObject()) {
                        const auto u = msg.asObject().get("usage");
                        if (u.isObject()) {
                            usage = u.asObject();
                            haveUsage = true;
                        }
                    }
                } else if (type.asString() == "message_delta") {
                    const auto u = obj.get("usage");
                    if (u.isObject()) {
                        usage = u.asObject();
                        haveUsage = true;
                    }
                }
                if (!haveUsage) break;

                const bool hasOutput =
                    takeTokenField(usage, "output_tokens", out.outputTokens);
                takeTokenField(usage, "input_tokens", out.inputTokens);
                takeTokenField(usage, "cache_read_input_tokens",
                               out.cacheReadTokens);
                takeTokenField(usage, "cache_creation_input_tokens",
                               out.cacheWriteTokens);
                if (!hasOutput) break;   // 本事件没给 output_tokens
                return out.outputTokens; // 本事件带 output_tokens（累计口径）
            } while (false);
            return -1; // 一条事件只看第一个 data: 行；没给 usage/output 就是 -1
        }
        if (lineEnd == std::string::npos) break;
        lineStart = lineEnd + 1;
    }
    return -1;
}

} // namespace

std::vector<int64_t> SseUsageParser::feed(const std::string& chunk) {
    // ponytail: 1MB 无事件分隔符直接清缓冲（非 SSE 响应护栏），正常 SSE 事件
    // 远小于此；需要更精细处理时改按 content-length/类型判断
    constexpr size_t kMaxBuf = 1u << 20;
    buf_ += chunk;
    std::vector<int64_t> out;
    size_t start = 0;
    while (true) {
        const size_t pLf = buf_.find("\n\n", start);
        const size_t pCr = buf_.find("\r\n\r\n", start);
        if (pLf == std::string::npos && pCr == std::string::npos) break;
        size_t sep;
        size_t sepLen;
        if (pCr == std::string::npos || (pLf != std::string::npos && pLf < pCr)) {
            sep = pLf;
            sepLen = 2;
        } else {
            sep = pCr;
            sepLen = 4;
        }
        const int64_t usage =
            parseUsageFromEvent(buf_.substr(start, sep - start), usage_);
        if (usage >= 0) out.push_back(usage);
        start = sep + sepLen;
    }
    buf_.erase(0, start);
    if (buf_.size() > kMaxBuf) buf_.clear();
    return out;
}

// ---------- stream 检测 ----------

bool wantsStream(const std::string& body) {
    if (body.empty()) return false;
    size_t begin = 0;
    size_t end = 0;
    if (!json_scan::findTopLevelValueSpan(body, "stream", begin, end)) {
        return false;
    }
    // 与旧 isJsonTrue 同口径：值区间前 4 字节是 "true" 即为真
    return body.compare(begin, 4, "true") == 0;
}

bool isStreamablePath(const std::string& path) {
    // Claude Code 只对 messages 走 SSE；其他路径（如 /v1/models）保持缓冲
    return path == "/v1/messages" || path.find("/v1/messages") != std::string::npos;
}

std::string buildUpstreamUrl(const std::string& baseUrl,
                             const std::string& pathOrUrl) {
    const std::string p = json_scan::trim(pathOrUrl);
    if (p.compare(0, 7, "http://") == 0 || p.compare(0, 8, "https://") == 0) {
        return p;  // 已是绝对 URL
    }
    std::string base = baseUrl;
    while (!base.empty() && base.back() == '/') base.pop_back();
    std::string path = p;
    if (path.empty()) path = "/";
    if (path.front() != '/') path = "/" + path;
    return base + path;
}

std::string buildUpstreamErrorFrame(const std::string& upstreamError) {
    // 转义引号/反斜杠/换行，保证 data 行是合法 JSON（错误 JSON 转义属 W8）
    std::string escaped;
    escaped.reserve(upstreamError.size() + 16);
    for (const char c : upstreamError) {
        if (c == '"' || c == '\\') {
            escaped.push_back('\\');
            escaped.push_back(c);
        } else if (c == '\n') {
            escaped += "\\n";
        } else if (c == '\r') {
            // 丢弃
        } else {
            escaped.push_back(c);
        }
    }
    return "event: error\ndata: {\"type\":\"error\",\"error\":"
           "{\"type\":\"upstream_stream_error\",\"message\":\"" +
           escaped + "\"}}\n\n";
}

// ---------- 上游流式请求 ----------

core::Result<std::unique_ptr<platform::http::StreamingResponse>>
sendStreamingClaude(const platform::http::Request& req,
                    const Profile& profile) {
    platform::http::Request upstream = req;

    // 1. URL 补全：req.url 是相对 path 时拼到 profile.baseUrl 上
    upstream.url = buildUpstreamUrl(profile.baseUrl, req.url);
    if (upstream.url.empty()) {
        return core::Result<std::unique_ptr<platform::http::StreamingResponse>>::fail(
            core::ErrorCode::ValidationError, "上游 URL 为空（profile.baseUrl 未配置）");
    }

    // 2. 按上游方言归一（鉴权头 / UA / 会话头 / 模型名标记）——
    //    各网关差异全部收敛在 UpstreamDialect 里，此处不再有 provider if。
    //    方言由 profile.dialectOverride 钉住（空串 = 未覆盖 → host 推断，
    //    与不传该参逐项一致；CPX-13 接线），与缓冲转发 / 连通性探测同源
    const UpstreamDialect dialect =
        UpstreamDialect::forBaseUrl(profile.baseUrl, profile.dialectOverride);
    dialect.applyHeaders(upstream.headers, profile.apiKeyPlain,
                         authFieldFromText(profile.authField));
    if (dialect.normalizeBody(upstream.body)) {
        LOG_INFO("流式代理：按上游方言剥掉模型名 [1M] 标记（" + profile.baseUrl + "）");
    }

    // 3. SSE 场景下上游不应做压缩（避免 chunk 边界被改变）
    upstream.headers["Accept-Encoding"] = "identity";

    LOG_INFO("流式转发 " + upstream.method + " " + upstream.url +
             " (profile=" + std::to_string(profile.id) + ")");

    return platform::http::sendStreaming(upstream);
}

// ---------- 上游 → 客户端 桥接 ----------

void forwardStream(platform::http::ResponseWriter& out,
                   std::shared_ptr<platform::http::StreamingResponse> upstream,
                   const std::string& contentType,
                   StreamUsageSink onUsage) {
    if (!upstream) {
        out.setStatus(502);
        out.setHeader("Content-Type", "application/json");
        out.write("{\"error\":\"upstream_unreachable\",\"message\":\"null stream\"}");
        out.end();
        return;
    }

    // 统计接线（设计 §3）：流生命周期内共享一个重组器 + 流 id，
    // endStream 幂等，provider 三条 return false 路径 + releaser 都调
    const uint64_t statsId = core::proxy_stats::beginStream();

    // 用量状态（migration v5）：parser 与「首个 chunk 时刻」都随流生命周期。
    // provider 与 releaser 由 cpp-httplib 在同一 worker 线程顺序调用（releaser
    // 必在 provider 收尾之后），故这两个字段不需要加锁。
    struct UsageState {
        SseUsageParser parser;      // 只由 provider 写
        int64_t firstEventMs = -1;  // provider 写，releaser 读
    };
    auto usageState = std::make_shared<UsageState>();
    const auto streamStart = std::chrono::steady_clock::now();

    // provider 由 cpp-httplib 反复调用，每轮拉一个 chunk 写给客户端
    auto provider =
        [upstream, statsId, usageState, streamStart](
            platform::http::ResponseWriter::ChunkWriter writeChunk,
            platform::http::ResponseWriter::IsWritableFn isWritable) -> bool {
        // 客户端已断开 → 立即停止上游（spec §10.4 取消语义）
        if (isWritable && !isWritable()) {
            LOG_INFO("客户端断开，取消上游流式请求");
            upstream->cancel();
            core::proxy_stats::endStream(statsId);
            return false;
        }

        std::string chunk;
        std::string err;
        const auto r = upstream->readChunk(chunk, err);

        if (r == platform::http::ChunkResult::End) {
            // 上游自然结束：不写终止 chunk，让 cpp-httplib 发 0\r\n\r\n
            core::proxy_stats::endStream(statsId);
            return false;
        }
        if (r == platform::http::ChunkResult::Error) {
            LOG_WARN("上游流式错误: " + err);
            // W7 P0-2：上游断流/超时不再静默收尾——先合成一个 event: error
            // SSE 帧写给客户端（Claude Code 能感知截断），再正常结束 chunked
            // 传输。err == "cancelled"（客户端已断开）时跳过，写也写不进去。
            if (err != "cancelled") {
                const std::string frame = buildUpstreamErrorFrame(err);
                if (!writeChunk(frame.data(), frame.size())) {
                    LOG_INFO("写 error 帧失败（对端已断开）");
                }
            }
            upstream->cancel();
            core::proxy_stats::endStream(statsId);
            return false;
        }
        if (chunk.empty()) {
            return true;  // 空 chunk：继续
        }
        // 首事件毫秒：流开始 → 第一个非空 chunk（上游边产边发，一个 chunk
        // 至少含一条事件的开头；只在第一次记录）
        if (usageState->firstEventMs < 0) {
            usageState->firstEventMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - streamStart)
                    .count();
        }
        // 顺路看一眼 SSE 事件（不影响转发，解析失败自动跳过）
        for (const int64_t cum : usageState->parser.feed(chunk)) {
            core::proxy_stats::updateStream(statsId, cum);
        }
        if (!writeChunk(chunk.data(), chunk.size())) {
            LOG_INFO("写客户端失败（对端断开），取消上游");
            upstream->cancel();
            core::proxy_stats::endStream(statsId);
            return false;
        }
        return true;
    };

    // releaser：响应彻底结束时触发（含客户端断开），确保上游被取消 + 线程回收；
    // 也是用量回执的唯一出口（此刻累计值已定格，且只回调一次）
    auto releaser = [upstream, statsId, usageState, onUsage](bool /*success*/) {
        upstream->cancel();
        core::proxy_stats::endStream(statsId);
        if (onUsage) {
            StreamUsage u = usageState->parser.usage();
            u.firstEventMs = usageState->firstEventMs;
            onUsage(u);
        }
    };

    out.setChunkProvider(contentType, provider, releaser);
}

} // namespace module::claude
