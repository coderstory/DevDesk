// src/module_claude_proxy/streaming_proxy.cpp
// SSE 流式 Claude 代理实现（spec §10.4）
#include "streaming_proxy.h"

#include "core/logging.h"

#include <cctype>
#include <chrono>
#include <string>

namespace module::claude {

namespace {

// 跳过 JSON 空白
void skipWs(const std::string& s, size_t& i) {
    while (i < s.size() &&
           (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) {
        ++i;
    }
}

// 从 s[i]（应指向 '"'）读出一个 JSON 字符串，返回其内容并推进 i
// 简单实现：不支持 \uXXXX 转义（对检测 key 名足够）
std::string readJsonString(const std::string& s, size_t& i) {
    std::string out;
    if (i >= s.size() || s[i] != '"') return out;
    ++i;  // 跳过开引号
    while (i < s.size()) {
        const char c = s[i];
        if (c == '\\') {
            if (i + 1 < s.size()) {
                out.push_back(s[i + 1]);
                i += 2;
                continue;
            }
            break;
        }
        if (c == '"') {
            ++i;  // 跳过闭引号
            break;
        }
        out.push_back(c);
        ++i;
    }
    return out;
}

// 跳过 JSON 值（用于跨过我们不需要的字段）
// 返回 false 表示解析到非法字符
bool skipJsonValue(const std::string& s, size_t& i) {
    skipWs(s, i);
    if (i >= s.size()) return false;
    const char c = s[i];
    if (c == '"') {
        readJsonString(s, i);
        return true;
    }
    if (c == '{' || c == '[') {
        const char open = c;
        const char close = (c == '{') ? '}' : ']';
        int depth = 0;
        while (i < s.size()) {
            const char x = s[i];
            if (x == '"') {
                readJsonString(s, i);
                continue;
            }
            if (x == open) ++depth;
            else if (x == close) {
                --depth;
                ++i;
                if (depth == 0) return true;
                continue;
            }
            ++i;
        }
        return false;
    }
    // null / true / false / 数字：吃到分隔符
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' &&
           s[i] != ' ' && s[i] != '\t' && s[i] != '\n' && s[i] != '\r') {
        ++i;
    }
    return true;
}

// 在顶层对象里找 key，返回其原始值文本（未做类型转换）
bool findTopLevelField(const std::string& body,
                       const std::string& wantedKey,
                       std::string& rawValueOut) {
    size_t i = 0;
    skipWs(body, i);
    if (i >= body.size() || body[i] != '{') return false;
    ++i;  // 进入对象

    int depth = 1;
    while (i < body.size()) {
        skipWs(body, i);
        if (i >= body.size()) break;
        if (body[i] == '}') {
            --depth;
            ++i;
            if (depth == 0) break;
            continue;
        }
        if (body[i] == ',') {
            ++i;
            continue;
        }
        if (body[i] != '"') {
            // 非预期 token：跳过它，避免死循环
            ++i;
            continue;
        }

        const size_t keyStart = i;
        const std::string key = readJsonString(body, i);
        skipWs(body, i);
        if (i >= body.size() || body[i] != ':') {
            // 非法结构：跳过
            if (i < body.size()) ++i;
            continue;
        }
        ++i;  // 跳过 ':'
        skipWs(body, i);

        if (depth == 1 && key == wantedKey) {
            const size_t valStart = i;
            if (!skipJsonValue(body, i)) return false;
            rawValueOut = body.substr(valStart, i - valStart);
            return true;
        }
        if (!skipJsonValue(body, i)) return false;
        (void)keyStart;
    }
    return false;
}

// 判断原始 JSON 值文本是否为 true
bool isJsonTrue(const std::string& raw) {
    size_t i = 0;
    skipWs(raw, i);
    return raw.compare(i, 4, "true") == 0;
}

// 去掉字符串首尾空白
std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

} // namespace

// ---------- stream 检测 ----------

bool wantsStream(const std::string& body) {
    if (body.empty()) return false;
    std::string raw;
    if (!findTopLevelField(body, "stream", raw)) return false;
    return isJsonTrue(raw);
}

bool isStreamablePath(const std::string& path) {
    // Claude Code 只对 messages 走 SSE；其他路径（如 /v1/models）保持缓冲
    return path == "/v1/messages" || path.find("/v1/messages") != std::string::npos;
}

std::string buildUpstreamUrl(const std::string& baseUrl,
                             const std::string& pathOrUrl) {
    const std::string p = trim(pathOrUrl);
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

    // 2. 注入 API key（覆盖客户端传来的 x-api-key）
    if (!profile.apiKeyPlain.empty()) {
        upstream.headers["x-api-key"] = profile.apiKeyPlain;
    }
    if (upstream.headers.find("anthropic-version") == upstream.headers.end()) {
        upstream.headers["anthropic-version"] = "2023-06-01";
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
                   const std::string& contentType) {
    if (!upstream) {
        out.setStatus(502);
        out.setHeader("Content-Type", "application/json");
        out.write("{\"error\":\"upstream_unreachable\",\"message\":\"null stream\"}");
        out.end();
        return;
    }

    // provider 由 cpp-httplib 反复调用，每轮拉一个 chunk 写给客户端
    auto provider =
        [upstream](platform::http::ResponseWriter::ChunkWriter writeChunk,
                   platform::http::ResponseWriter::IsWritableFn isWritable) -> bool {
        // 客户端已断开 → 立即停止上游（spec §10.4 取消语义）
        if (isWritable && !isWritable()) {
            LOG_INFO("客户端断开，取消上游流式请求");
            upstream->cancel();
            return false;
        }

        std::string chunk;
        std::string err;
        const auto r = upstream->readChunk(chunk, err);

        if (r == platform::http::ChunkResult::End) {
            // 上游自然结束：不写终止 chunk，让 cpp-httplib 发 0\r\n\r\n
            return false;
        }
        if (r == platform::http::ChunkResult::Error) {
            LOG_WARN("上游流式错误: " + err);
            upstream->cancel();
            return false;
        }
        if (chunk.empty()) {
            return true;  // 空 chunk：继续
        }
        if (!writeChunk(chunk.data(), chunk.size())) {
            LOG_INFO("写客户端失败（对端断开），取消上游");
            upstream->cancel();
            return false;
        }
        return true;
    };

    // releaser：响应彻底结束时触发（含客户端断开），确保上游被取消 + 线程回收
    auto releaser = [upstream](bool /*success*/) {
        upstream->cancel();
    };

    out.setChunkProvider(contentType, provider, releaser);
}

} // namespace module::claude
