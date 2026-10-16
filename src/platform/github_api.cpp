// src/platform/github_api.cpp
#include "platform/github_api.h"

#include <openssl/evp.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <thread>

#include "core/logging.h"
#include "core/str_util.h"
#include "platform/http_headers.h"
#include "platform/json.h"

namespace platform::github {

namespace pj = platform::json;

namespace {

constexpr const char* kApiBase = "https://api.github.com";
constexpr const char* kAcceptJson = "application/vnd.github+json";
// >1MB 的文件 Contents API 不回 base64 正文（encoding="none"），改用 raw 直取原文
constexpr const char* kAcceptRaw = "application/vnd.github.raw";
constexpr const char* kApiVersion = "2022-11-28";
constexpr const char* kProbeContent = "devdesk connectivity test";
constexpr size_t kMaxLog = 50;
constexpr size_t kBodyTailBytes = 512;
constexpr int kMaxRetryAfterMs = 60000;   // Retry-After 上限 60s
constexpr long kRequestTimeoutMs = 30000;

// base64：复用 OpenSSL EVP（不自造、不引第三方）。EVP_DecodeBlock 的返回长度含填充字节，
// 必须按 '=' 个数回退；GitHub 的 content 字段每 60 字符插 '\n'，解码前须剔除空白。
std::string base64Encode(const std::string& raw) {
    if (raw.empty()) return "";
    std::string out(4 * ((raw.size() + 2) / 3) + 1, '\0');
    const int n = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(&out[0]),
        reinterpret_cast<const unsigned char*>(raw.data()),
        static_cast<int>(raw.size()));
    if (n <= 0) return "";
    out.resize(static_cast<size_t>(n));
    return out;
}

core::Result<std::string> base64Decode(const std::string& in) {
    std::string clean;
    clean.reserve(in.size());
    for (const char c : in) {
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        clean.push_back(c);
    }
    if (clean.empty()) return core::Result<std::string>::ok("");
    if (clean.size() % 4 != 0) {
        return core::Result<std::string>::fail(core::ErrorCode::ValidationError,
                                               "base64 长度不是 4 的倍数");
    }
    std::string out((clean.size() / 4) * 3, '\0');
    const int n = EVP_DecodeBlock(
        reinterpret_cast<unsigned char*>(&out[0]),
        reinterpret_cast<const unsigned char*>(clean.data()),
        static_cast<int>(clean.size()));
    if (n < 0) {
        return core::Result<std::string>::fail(core::ErrorCode::ValidationError,
                                               "base64 解码失败（非法字符）");
    }
    size_t pad = 0;
    if (clean.back() == '=') ++pad;
    if (clean.size() >= 2 && clean[clean.size() - 2] == '=') ++pad;
    out.resize(static_cast<size_t>(n) - pad);
    return core::Result<std::string>::ok(std::move(out));
}

std::string bodyTail(const std::string& body, size_t maxBytes) {
    if (body.size() <= maxBytes) return body;
    return body.substr(body.size() - maxBytes);
}

int parseIntOr(const char* text, int fallback) {
    if (!text || !*text) return fallback;
    char* end = nullptr;
    const long v = std::strtol(text, &end, 10);
    if (end == text) return fallback;
    return static_cast<int>(v);
}

// 重试表（HTTP 侧）：5xx / 429 恒重试；403 仅在 x-ratelimit-remaining: 0 时按限流重试
// （普通 403 是无权限，不重试）；401/404/409/422 不重试。
// 传输侧（连接失败/超时/断开/DNS 解析/TLS 握手失败）在 doRequest 里按 networkError 整体重试：
// platform::http::send 把所有 CURLcode 传输错误统一折成 Result::fail(NetworkUnreachable)，
// 到这里已不含本地错误（本地错误在发送前被 validateConfig 拦下），整体重试即等价于按 CURLcode 逐码分类。
bool isRetryableStatus(int status, const std::map<std::string, std::string>& headers) {
    if (status >= 500 && status <= 599) return true;
    if (status == 429) return true;
    if (status == 403) {
        const char* remaining = platform::http::findHeaderCI(headers, "x-ratelimit-remaining");
        return remaining && parseIntOr(remaining, -1) == 0;
    }
    return false;
}

// Retry-After（秒）→ 毫秒，上限 60s；缺失或 HTTP-date 形态返回 -1（改用默认退避）。
int retryAfterMs(const std::map<std::string, std::string>& headers) {
    const char* v = platform::http::findHeaderCI(headers, "retry-after");
    if (!v || !*v) return -1;
    char* end = nullptr;
    const long seconds = std::strtol(v, &end, 10);
    if (end == v || seconds < 0) return -1;
    const long ms = seconds * 1000;
    return static_cast<int>(ms > kMaxRetryAfterMs ? kMaxRetryAfterMs : ms);
}

core::ErrorCode mapStatusToError(int status) {
    switch (status) {
        case 401: return core::ErrorCode::InvalidApiKey;
        case 404: return core::ErrorCode::NotFound;
        case 409: return core::ErrorCode::Conflict;
        case 422: return core::ErrorCode::ValidationError;
        default:  return core::ErrorCode::HttpError;
    }
}

std::string httpStatusText(int status) {
    switch (status) {
        case 401: return "认证失败：令牌无效或已过期，请检查/更新 PAT（HTTP 401）";
        case 403: return "无权限（HTTP 403）";
        case 404: return "未找到（HTTP 404）";
        case 409: return "冲突：远端 sha 与本地不一致（HTTP 409）";
        case 422: return "请求校验失败（HTTP 422）";
        case 429: return "触发限流（HTTP 429）";
        default:  return "HTTP " + std::to_string(status);
    }
}

std::string jsonStringField(const pj::JsonValue& v, const char* key) {
    if (!v.isObject()) return "";
    const pj::JsonValue f = v.asObject().get(key);
    return f.isString() ? f.asString() : "";
}

bool jsonBoolField(const pj::JsonValue& v, const char* key, bool fallback) {
    if (!v.isObject()) return fallback;
    const pj::JsonValue f = v.asObject().get(key);
    return f.isBool() ? f.asBool() : fallback;
}

bool isUrlUnreserved(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           c == '-' || c == '_' || c == '.' || c == '~';
}

// 路径编码：笔记文件名是用户数据（中文 / 空格 / '#' '?' 都会出现），不转义 curl 直接拒发。
// '/' 是路径段分隔符，保留。
std::string percentEncodePath(const std::string& path) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(path.size() * 2);
    for (const char ch : path) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (isUrlUnreserved(c) || c == '/') {
            out.push_back(ch);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0xF];
        }
    }
    return out;
}

// 值编码（query 值 / 单个路径值）：'/' 也转义——branch 名 "feature/x" 不转义会改变 refs 路径结构。
std::string percentEncodeValue(const std::string& value) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 2);
    for (const char ch : value) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (isUrlUnreserved(c)) {
            out.push_back(ch);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0xF];
        }
    }
    return out;
}

bool isSensitiveQueryKey(const std::string& key) {
    const std::string k = core::str::toLowerAscii(key);
    static const char* const kKeys[] = {
        "token", "access_token", "api_key", "apikey", "key", "password", "pwd",
        "secret", "auth", "authorization", "bearer", "credential", "credentials",
        "pat", "private_token",
    };
    for (const char* kk : kKeys) {
        if (k == kk) return true;
    }
    return false;
}

// 兜底：把已知 GitHub token 前缀后面的凭据体替换成 ***（token 可能出现在路径 / 查询值里）
void scrubTokenPrefixes(std::string& text) {
    static const char* const kPrefixes[] = {"github_pat_", "ghp_", "gho_", "ghu_", "ghs_", "ghr_"};
    for (const char* prefix : kPrefixes) {
        const size_t plen = std::strlen(prefix);
        size_t pos = 0;
        while ((pos = text.find(prefix, pos)) != std::string::npos) {
            size_t end = pos + plen;
            while (end < text.size()) {
                const unsigned char c = static_cast<unsigned char>(text[end]);
                const bool word = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                                  (c >= 'a' && c <= 'z') || c == '_';
                if (!word) break;
                ++end;
            }
            text.replace(pos, end - pos, std::string(prefix) + "***");
            pos += plen + 3;
        }
    }
}

std::string maskQuery(std::string out) {
    const size_t qpos = out.find('?');
    if (qpos == std::string::npos) return out;
    std::string prefix = out.substr(0, qpos);
    std::string query = out.substr(qpos + 1);
    std::string fragment;
    if (const size_t hpos = query.find('#'); hpos != std::string::npos) {
        fragment = query.substr(hpos);
        query = query.substr(0, hpos);
    }
    std::string rebuilt;
    size_t start = 0;
    while (true) {
        const size_t amp = query.find('&', start);
        std::string pair = query.substr(start, amp == std::string::npos ? std::string::npos
                                                                       : amp - start);
        if (const size_t eq = pair.find('='); eq != std::string::npos) {
            const std::string key = pair.substr(0, eq);
            if (isSensitiveQueryKey(key)) pair = key + "=***";
        } else if (!pair.empty() && isSensitiveQueryKey(pair)) {
            pair = "***";
        }
        if (!rebuilt.empty()) rebuilt += "&";
        rebuilt += pair;
        if (amp == std::string::npos) break;
        start = amp + 1;
    }
    return prefix + "?" + rebuilt + fragment;
}

} // namespace

// ---- 构造与注入点 ----

GithubApi::GithubApi(std::string token, std::string owner, std::string repo, RetryPolicy policy)
    : token_(std::move(token)),
      owner_(std::move(owner)),
      repo_(std::move(repo)),
      policy_(std::move(policy)),
      sender_(platform::http::send) {}

void GithubApi::setSender(Sender sender) {
    sender_ = sender ? std::move(sender) : Sender(platform::http::send);
}

core::Result<void> GithubApi::validateConfig() const {
    if (token_.empty()) {
        return core::Result<void>::fail(core::ErrorCode::InvalidApiKey,
                                        "未配置访问令牌（PAT）");
    }
    // owner/repo 字符白名单（GitHub 规则）：owner 仅字母数字与连字符（1-39，首尾
    // 不可连字符）；repo 另允许 `_` 与 `.`。手误（空格/中文/整段 URL 误填）若放行
    // 会构造出畸形 URL，被 http 层折叠成 networkError 而白耗 3 次退避重试后误报
    // 「网络错误」——这里发送前拦成 ValidationError，零请求
    const auto isOwnerChar = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '-';
    };
    if (owner_.empty() || owner_.size() > 39 || owner_.front() == '-' ||
        owner_.back() == '-' ||
        !std::all_of(owner_.begin(), owner_.end(), isOwnerChar)) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError,
                                        "仓库归属（owner）含不合法字符或超长：" + owner_);
    }
    const auto isRepoChar = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ||
               c == '.';
    };
    if (repo_.empty() ||
        !std::all_of(repo_.begin(), repo_.end(), isRepoChar)) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError,
                                        "仓库名（repo）含不合法字符：" + repo_);
    }
    return core::Result<void>::ok();
}

std::function<void(int)>& GithubApi::sleepFn() {
    static std::function<void(int)> fn;
    return fn;
}

void GithubApi::setSleepFunction(std::function<void(int)> fn) { sleepFn() = std::move(fn); }

void GithubApi::sleepMs(int ms) {
    if (ms <= 0) return;
    if (auto& fn = sleepFn(); fn) {
        fn(ms);
    } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
}

// ---- 日志 ----

void GithubApi::appendLog(const ApiLogEntry& entry) {
    std::lock_guard<std::mutex> lock(logMutex_);
    log_.push_back(entry);
    while (log_.size() > kMaxLog) log_.pop_front();
}

std::vector<ApiLogEntry> GithubApi::recentLog() const {
    std::lock_guard<std::mutex> lock(logMutex_);
    return std::vector<ApiLogEntry>(log_.begin(), log_.end());
}

int GithubApi::backoffFor(int attemptIndex) const {
    if (policy_.backoffMs.empty()) return 1000;
    const size_t idx = std::min(static_cast<size_t>(attemptIndex),
                                policy_.backoffMs.size() - 1);
    return policy_.backoffMs[idx];
}

std::string GithubApi::contentsUrl(const std::string& path) const {
    return std::string(kApiBase) + "/repos/" + owner_ + "/" + repo_ + "/contents/" +
           percentEncodePath(path);
}

// ---- 请求（重试 + 退避 + 日志）----

GithubApi::Attempt GithubApi::doRequest(const std::string& method, const std::string& url,
                                        const std::string& body, const std::string& accept) {
    // 本地错误在发送前拦下：不发请求、不重试、不进日志（无请求可审计）
    if (const core::Result<void> cfg = validateConfig(); cfg.isFail()) {
        Attempt local;
        local.localError = true;
        local.errorCode = cfg.errorCode();
        local.errorMessage = cfg.errorMessage();
        return local;
    }

    const int attempts = policy_.attempts > 0 ? policy_.attempts : 1;
    Attempt last;

    for (int i = 0; i < attempts; ++i) {
        platform::http::Request req;
        req.method = method;
        req.url = url;
        req.timeoutMs = kRequestTimeoutMs;
        // token 只进 header：绝不拼进 URL / argv / 日志
        req.headers["Authorization"] = "Bearer " + token_;
        req.headers["Accept"] = accept;
        req.headers["X-GitHub-Api-Version"] = kApiVersion;
        req.headers["User-Agent"] = "DevDesk";
        if (!body.empty() || method == "PUT" || method == "DELETE" || method == "POST") {
            req.headers["Content-Type"] = "application/json";
            req.body = body;
        }

        const auto t0 = std::chrono::steady_clock::now();
        core::Result<platform::http::Response> sent = sender_(req);
        const int durationMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count());

        ApiLogEntry entry;
        entry.method = method;
        entry.urlMasked = maskUrl(url);
        entry.durationMs = durationMs;

        if (sent.isFail() || sent.value().networkError) {
            const std::string msg = sent.isFail() ? sent.errorMessage()
                                                  : sent.value().errorMessage;
            entry.networkError = true;
            entry.errorMessage = msg;
            appendLog(entry);

            last = Attempt{};
            last.networkError = true;
            last.errorMessage = msg;
            if (i + 1 < attempts) {
                sleepMs(backoffFor(i));
                continue;
            }
            return last;
        }

        platform::http::Response resp = sent.value();
        entry.statusCode = resp.statusCode;
        entry.bodyTail = bodyTail(resp.body, kBodyTailBytes);
        appendLog(entry);

        last = Attempt{};
        last.resp = std::move(resp);
        if (isRetryableStatus(last.resp.statusCode, last.resp.headers) && i + 1 < attempts) {
            int delay = retryAfterMs(last.resp.headers);
            if (delay < 0) delay = backoffFor(i);
            sleepMs(delay);
            continue;
        }
        return last;
    }
    return last;
}

// ---- 端点 ----

core::Result<RepoInfo> GithubApi::getRepo() {
    const std::string url = std::string(kApiBase) + "/repos/" + owner_ + "/" + repo_;
    const Attempt a = doRequest("GET", url, "", kAcceptJson);
    if (a.localError) {
        return core::Result<RepoInfo>::fail(a.errorCode, a.errorMessage);
    }
    if (a.networkError) {
        return core::Result<RepoInfo>::fail(core::ErrorCode::NetworkUnreachable,
                                            "网络错误：" + a.errorMessage);
    }
    if (a.resp.statusCode != 200) {
        return core::Result<RepoInfo>::fail(mapStatusToError(a.resp.statusCode),
                                            httpStatusText(a.resp.statusCode));
    }
    const core::Result<pj::JsonValue> parsed = pj::parse(a.resp.body);
    if (parsed.isFail()) {
        return core::Result<RepoInfo>::fail(core::ErrorCode::JsonParseError,
                                            "解析仓库信息失败：" + parsed.errorMessage());
    }
    const pj::JsonValue v = parsed.value();
    RepoInfo info;
    info.fullName = jsonStringField(v, "full_name");
    info.defaultBranch = jsonStringField(v, "default_branch");
    info.isPrivate = jsonBoolField(v, "private", false);
    if (v.isObject()) {
        info.canPush = jsonBoolField(v.asObject().get("permissions"), "push", false);
    }
    if (info.fullName.empty()) info.fullName = owner_ + "/" + repo_;
    return core::Result<RepoInfo>::ok(std::move(info));
}

core::Result<std::string> GithubApi::getAuthenticatedUser() {
    const std::string url = std::string(kApiBase) + "/user";
    const Attempt a = doRequest("GET", url, "", kAcceptJson);
    if (a.localError) {
        return core::Result<std::string>::fail(a.errorCode, a.errorMessage);
    }
    if (a.networkError) {
        return core::Result<std::string>::fail(core::ErrorCode::NetworkUnreachable,
                                               "网络错误：" + a.errorMessage);
    }
    if (a.resp.statusCode != 200) {
        return core::Result<std::string>::fail(mapStatusToError(a.resp.statusCode),
                                               httpStatusText(a.resp.statusCode));
    }
    const core::Result<pj::JsonValue> parsed = pj::parse(a.resp.body);
    if (parsed.isFail()) {
        return core::Result<std::string>::fail(core::ErrorCode::JsonParseError,
                                               "解析用户信息失败：" + parsed.errorMessage());
    }
    const std::string login = jsonStringField(parsed.value(), "login");
    if (login.empty()) {
        return core::Result<std::string>::fail(core::ErrorCode::JsonParseError,
                                               "响应缺少 login 字段");
    }
    return core::Result<std::string>::ok(login);
}

core::Result<FileInfo> GithubApi::getFile(const std::string& path, const std::string& ref) {
    std::string url = contentsUrl(path);
    if (!ref.empty()) url += "?ref=" + percentEncodeValue(ref);

    const Attempt a = doRequest("GET", url, "", kAcceptJson);
    if (a.localError) {
        return core::Result<FileInfo>::fail(a.errorCode, a.errorMessage);
    }
    if (a.networkError) {
        return core::Result<FileInfo>::fail(core::ErrorCode::NetworkUnreachable,
                                            "网络错误：" + a.errorMessage);
    }
    if (a.resp.statusCode == 404) {
        FileInfo missing;   // 不存在不是错误：exists=false 交给调用方决策
        return core::Result<FileInfo>::ok(std::move(missing));
    }
    if (a.resp.statusCode != 200) {
        return core::Result<FileInfo>::fail(mapStatusToError(a.resp.statusCode),
                                            httpStatusText(a.resp.statusCode));
    }

    const core::Result<pj::JsonValue> parsed = pj::parse(a.resp.body);
    if (parsed.isFail()) {
        return core::Result<FileInfo>::fail(core::ErrorCode::JsonParseError,
                                            "解析文件信息失败：" + parsed.errorMessage());
    }
    const pj::JsonValue v = parsed.value();
    FileInfo info;
    info.exists = true;
    info.sha = jsonStringField(v, "sha");

    const std::string encoding = jsonStringField(v, "encoding");
    const std::string content = jsonStringField(v, "content");
    if (!content.empty() && encoding != "none") {
        const core::Result<std::string> decoded = base64Decode(content);
        if (decoded.isFail()) {
            return core::Result<FileInfo>::fail(core::ErrorCode::JsonParseError,
                                                "base64 解码失败：" + decoded.errorMessage());
        }
        info.content = decoded.value();
        return core::Result<FileInfo>::ok(std::move(info));
    }

    // 大文件（>1MB）或 encoding="none"：用 raw 直取原文（sha 仍取上面的元数据）
    const Attempt raw = doRequest("GET", url, "", kAcceptRaw);
    if (raw.networkError) {
        return core::Result<FileInfo>::fail(core::ErrorCode::NetworkUnreachable,
                                            "网络错误：" + raw.errorMessage);
    }
    if (raw.resp.statusCode != 200) {
        return core::Result<FileInfo>::fail(mapStatusToError(raw.resp.statusCode),
                                            httpStatusText(raw.resp.statusCode));
    }
    info.content = raw.resp.body;
    return core::Result<FileInfo>::ok(std::move(info));
}

core::Result<std::vector<TreeEntry>> GithubApi::listTree(const std::string& branch) {
    // branch 按值编码：可含 '/'（feature/x），不转义会改变 refs 路径层级
    const std::string url = std::string(kApiBase) + "/repos/" + owner_ + "/" + repo_ +
                            "/git/trees/" + percentEncodeValue(branch) + "?recursive=1";
    const Attempt a = doRequest("GET", url, "", kAcceptJson);
    if (a.localError) {
        return core::Result<std::vector<TreeEntry>>::fail(a.errorCode, a.errorMessage);
    }
    if (a.networkError) {
        return core::Result<std::vector<TreeEntry>>::fail(core::ErrorCode::NetworkUnreachable,
                                                          "网络错误：" + a.errorMessage);
    }
    if (a.resp.statusCode != 200) {
        return core::Result<std::vector<TreeEntry>>::fail(mapStatusToError(a.resp.statusCode),
                                                          httpStatusText(a.resp.statusCode));
    }
    const core::Result<pj::JsonValue> parsed = pj::parse(a.resp.body);
    if (parsed.isFail() || !parsed.value().isObject()) {
        return core::Result<std::vector<TreeEntry>>::fail(core::ErrorCode::JsonParseError,
                                                          "解析文件树失败");
    }
    const pj::JsonValue root = parsed.value();
    if (jsonBoolField(root, "truncated", false)) {
        return core::Result<std::vector<TreeEntry>>::fail(
            core::ErrorCode::HttpError,
            "远端文件树过大被截断（truncated=true），暂不支持分页补取");
    }
    const pj::JsonValue tree = root.asObject().get("tree");
    if (!tree.isArray()) {
        return core::Result<std::vector<TreeEntry>>::fail(core::ErrorCode::JsonParseError,
                                                          "文件树响应缺少 tree 数组");
    }
    std::vector<TreeEntry> out;
    const int n = tree.size();
    out.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const pj::JsonValue item = tree[i];
        if (!item.isObject()) continue;
        if (jsonStringField(item, "type") != "blob") continue;   // 目录/子模块条目跳过
        TreeEntry e;
        e.path = jsonStringField(item, "path");
        e.sha = jsonStringField(item, "sha");
        if (e.path.empty() || e.sha.empty()) continue;
        const pj::JsonValue size = item.asObject().get("size");
        if (size.isNumber()) e.size = static_cast<uint64_t>(size.asInt());
        out.push_back(std::move(e));
    }
    return core::Result<std::vector<TreeEntry>>::ok(std::move(out));
}

core::Result<std::string> GithubApi::putFile(const std::string& path, const std::string& content,
                                             const std::string& message,
                                             const std::string& shaIfMatch,
                                             const std::string& branch) {
    pj::JsonObject body;
    body.set("message", pj::makeString(message));
    body.set("content", pj::makeString(base64Encode(content)));
    // Contents API 的 PUT 用 body 里的 branch 字段定向分支（GET 才用 ?ref= 查询参数）；
    // 空串 = 不带该字段 = 仓库默认分支。
    if (!branch.empty()) body.set("branch", pj::makeString(branch));
    if (!shaIfMatch.empty()) body.set("sha", pj::makeString(shaIfMatch));
    const core::Result<std::string> text = pj::stringify(pj::JsonValue(body));
    if (text.isFail()) {
        return core::Result<std::string>::fail(core::ErrorCode::Internal, "构造请求体失败");
    }

    const Attempt a = doRequest("PUT", contentsUrl(path), text.value(), kAcceptJson);
    if (a.localError) {
        return core::Result<std::string>::fail(a.errorCode, a.errorMessage);
    }
    if (a.networkError) {
        return core::Result<std::string>::fail(core::ErrorCode::NetworkUnreachable,
                                               "网络错误：" + a.errorMessage);
    }
    if (a.resp.statusCode != 200 && a.resp.statusCode != 201) {
        return core::Result<std::string>::fail(mapStatusToError(a.resp.statusCode),
                                               httpStatusText(a.resp.statusCode));
    }
    const core::Result<pj::JsonValue> parsed = pj::parse(a.resp.body);
    if (parsed.isOk()) {
        const pj::JsonValue v = parsed.value();
        if (v.isObject()) {
            return core::Result<std::string>::ok(jsonStringField(v.asObject().get("content"), "sha"));
        }
    }
    return core::Result<std::string>::ok(std::string());
}

core::Result<void> GithubApi::deleteFile(const std::string& path, const std::string& message,
                                         const std::string& shaIfMatch,
                                         const std::string& branch) {
    pj::JsonObject body;
    body.set("message", pj::makeString(message));
    body.set("sha", pj::makeString(shaIfMatch));
    // 空 branch = 不带该字段 = 仓库默认分支（与 putFile 同语义）。
    if (!branch.empty()) body.set("branch", pj::makeString(branch));
    const core::Result<std::string> text = pj::stringify(pj::JsonValue(body));
    if (text.isFail()) {
        return core::Result<void>::fail(core::ErrorCode::Internal, "构造请求体失败");
    }

    const Attempt a = doRequest("DELETE", contentsUrl(path), text.value(), kAcceptJson);
    if (a.localError) {
        return core::Result<void>::fail(a.errorCode, a.errorMessage);
    }
    if (a.networkError) {
        return core::Result<void>::fail(core::ErrorCode::NetworkUnreachable,
                                        "网络错误：" + a.errorMessage);
    }
    if (a.resp.statusCode != 200) {
        return core::Result<void>::fail(mapStatusToError(a.resp.statusCode),
                                        httpStatusText(a.resp.statusCode));
    }
    return core::Result<void>::ok();
}

core::Result<std::string> GithubApi::putFileOnBranch(const std::string& path,
                                                     const std::string& content,
                                                     const std::string& message,
                                                     const std::string& shaIfMatch,
                                                     const std::string& branch) {
    return putFile(path, content, message, shaIfMatch, branch);
}

core::Result<void> GithubApi::createRef(const std::string& branch, const std::string& fromBranch) {
    // 基点分支 HEAD sha：GET /git/ref/heads/{fromBranch}（单数 ref 端点）→ object.sha
    const std::string refUrl = std::string(kApiBase) + "/repos/" + owner_ + "/" + repo_ +
                               "/git/ref/heads/" + percentEncodeValue(fromBranch);
    const Attempt head = doRequest("GET", refUrl, "", kAcceptJson);
    if (head.localError) {
        return core::Result<void>::fail(head.errorCode, head.errorMessage);
    }
    if (head.networkError) {
        return core::Result<void>::fail(core::ErrorCode::NetworkUnreachable,
                                        "网络错误：" + head.errorMessage);
    }
    if (head.resp.statusCode != 200) {
        return core::Result<void>::fail(mapStatusToError(head.resp.statusCode),
                                        "读取基点分支 HEAD 失败：" +
                                            httpStatusText(head.resp.statusCode));
    }
    const core::Result<pj::JsonValue> parsed = pj::parse(head.resp.body);
    if (parsed.isFail()) {
        return core::Result<void>::fail(core::ErrorCode::JsonParseError,
                                        "解析基点分支信息失败：" + parsed.errorMessage());
    }
    const pj::JsonValue object = parsed.value().asObject().get("object");
    const std::string baseSha = jsonStringField(object, "sha");
    if (baseSha.empty()) {
        return core::Result<void>::fail(core::ErrorCode::JsonParseError,
                                        "基点分支响应缺少 object.sha");
    }

    pj::JsonObject body;
    body.set("ref", pj::makeString("refs/heads/" + branch));
    body.set("sha", pj::makeString(baseSha));
    const core::Result<std::string> text = pj::stringify(pj::JsonValue(body));
    if (text.isFail()) {
        return core::Result<void>::fail(core::ErrorCode::Internal, "构造请求体失败");
    }

    const std::string refsUrl = std::string(kApiBase) + "/repos/" + owner_ + "/" + repo_ + "/git/refs";
    const Attempt a = doRequest("POST", refsUrl, text.value(), kAcceptJson);
    if (a.localError) {
        return core::Result<void>::fail(a.errorCode, a.errorMessage);
    }
    if (a.networkError) {
        return core::Result<void>::fail(core::ErrorCode::NetworkUnreachable,
                                        "网络错误：" + a.errorMessage);
    }
    if (a.resp.statusCode != 201) {
        // POST 已被服务端受理但响应丢失 → 重试撞 422 "Reference already exists"：
        // 分支实际已建成，视为成功（对齐「测完不留痕」——若当失败处理会漏删临时分支）
        if (a.resp.statusCode == 422 &&
            a.resp.body.find("already exists") != std::string::npos) {
            return core::Result<void>::ok();
        }
        return core::Result<void>::fail(mapStatusToError(a.resp.statusCode),
                                        httpStatusText(a.resp.statusCode));
    }
    return core::Result<void>::ok();
}

core::Result<void> GithubApi::deleteRef(const std::string& branch) {
    // branch 整体按值编码：分支名可含 '/'（feature/x），不转义会改变 refs 路径层级
    const std::string url = std::string(kApiBase) + "/repos/" + owner_ + "/" + repo_ +
                            "/git/refs/heads/" + percentEncodeValue(branch);
    const Attempt a = doRequest("DELETE", url, "", kAcceptJson);
    if (a.localError) {
        return core::Result<void>::fail(a.errorCode, a.errorMessage);
    }
    if (a.networkError) {
        return core::Result<void>::fail(core::ErrorCode::NetworkUnreachable,
                                        "网络错误：" + a.errorMessage);
    }
    if (a.resp.statusCode != 200 && a.resp.statusCode != 204) {
        return core::Result<void>::fail(mapStatusToError(a.resp.statusCode),
                                        httpStatusText(a.resp.statusCode));
    }
    return core::Result<void>::ok();
}

TestReport GithubApi::testConnection(std::function<void(const TestStep&)> onStep) {
    TestReport report;
    const auto emit = [&](TestStep step) {
        if (onStep) onStep(step);
        report.steps.push_back(std::move(step));
    };
    const auto stepStart = [] { return std::chrono::steady_clock::now(); };
    const auto elapsed = [](const std::chrono::steady_clock::time_point& t0) {
        return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count());
    };

    // ① GET /user
    {
        const auto t0 = stepStart();
        const core::Result<std::string> user = getAuthenticatedUser();
        TestStep step;
        step.name = "验证令牌";
        step.durationMs = elapsed(t0);
        step.ok = user.isOk();
        step.detail = user.isOk() ? ("已认证用户：" + user.value()) : user.errorMessage();
        emit(std::move(step));
        if (!user.isOk()) {
            report.ok = false;
            report.summary = "连通性失败：令牌认证未通过（" + user.errorMessage() + "）";
            return report;
        }
    }

    // ② GET /repos/{owner}/{repo}：存在 + canPush；defaultBranch 供第 ③ 步建临时分支用
    RepoInfo repoInfo;
    {
        const auto t0 = stepStart();
        const core::Result<RepoInfo> repo = getRepo();
        TestStep step;
        step.name = "检查仓库";
        step.durationMs = elapsed(t0);
        if (repo.isFail()) {
            step.ok = false;
            step.detail = repo.errorMessage();
        } else if (!repo.value().canPush) {
            step.ok = false;
            step.detail = "仓库 " + repo.value().fullName + " 存在，但令牌无 push 权限";
        } else {
            step.ok = true;
            step.detail = "仓库 " + repo.value().fullName + " 可推送（默认分支 " +
                          repo.value().defaultBranch + "）";
            repoInfo = repo.value();
        }
        emit(std::move(step));
        if (!report.steps.back().ok) {
            report.ok = false;
            report.summary = "连通性失败：仓库检查未通过（" + report.steps.back().detail + "）";
            return report;
        }
    }

    // ③ 读写探针：建临时分支 → 分支上写探针文件 → 删临时分支（测完不留痕）。
    // 删分支失败不算连通性失败（读写已被证明），但 summary 必须提示远端残留与手动清理。
    {
        const auto t0 = stepStart();
        const std::string branch =
            "devdesk-test-" + std::to_string(static_cast<long long>(std::time(nullptr)));
        TestStep step;
        step.name = "读写探针";
        std::string residueHint;

        const core::Result<void> created = createRef(branch, repoInfo.defaultBranch);
        if (created.isFail()) {
            step.ok = false;
            step.detail = "创建临时分支失败：" + created.errorMessage();
            // 兜底删除：POST 可能已被服务端受理但响应丢失（重试耗尽仍报失败）。
            // 分支没建成时删的是 404，无害；只有删除也失败（非 404）才算真残留
            if (const core::Result<void> removed = deleteRef(branch);
                removed.isFail() && removed.errorCode() != core::ErrorCode::NotFound) {
                residueHint = "临时分支 " + branch + " 删除失败（" + removed.errorMessage() +
                              "），远端有残留，请手动删除";
            }
        } else {
            const core::Result<std::string> put =
                putFileOnBranch("devdesk-probe.txt", kProbeContent,
                                "devdesk connectivity test", "", branch);
            if (put.isFail()) {
                step.ok = false;
                step.detail = "写入探针失败：" + put.errorMessage();
            } else {
                step.ok = true;
                step.detail = "已创建并删除临时分支 " + branch + "（含探针文件）";
            }
            // 分支已建就必须尝试删除：探针失败也不能把残留留给用户
            if (const core::Result<void> removed = deleteRef(branch); removed.isFail()) {
                residueHint = "临时分支 " + branch + " 删除失败（" + removed.errorMessage() +
                              "），远端有残留，请手动删除";
            }
        }

        step.durationMs = elapsed(t0);
        const bool ok = step.ok;
        const std::string detail = step.detail;
        emit(std::move(step));
        if (!ok) {
            report.ok = false;
            report.summary = "连通性失败：读写探针未通过（" + detail + "）";
            if (!residueHint.empty()) report.summary += "；" + residueHint;
            return report;
        }
        report.ok = true;
        report.summary = "连通性正常：令牌有效、仓库可推送、读写探针成功";
        if (!residueHint.empty()) report.summary += "（注意：" + residueHint + "）";
        return report;
    }
}

// ---- 静态工具 ----

std::pair<std::string, std::string> GithubApi::parseRepoUrl(const std::string& url) {
    const std::string s = core::str::trimAscii(url);
    if (s.empty()) return {"", ""};

    std::string pathPart;
    if (const size_t scheme = s.find("://"); scheme != std::string::npos) {
        const std::string rest = s.substr(scheme + 3);
        const size_t slash = rest.find('/');
        if (slash == std::string::npos) return {"", ""};
        pathPart = rest.substr(slash + 1);
    } else if (const size_t at = s.find('@'); at != std::string::npos) {
        // scp 形态 git@host:owner/repo 或 user@host/owner/repo
        const std::string rest = s.substr(at + 1);
        if (const size_t colon = rest.find(':'); colon != std::string::npos) {
            pathPart = rest.substr(colon + 1);
        } else if (const size_t slash = rest.find('/'); slash != std::string::npos) {
            pathPart = rest.substr(slash + 1);
        } else {
            return {"", ""};
        }
    } else {
        pathPart = s;   // 裸 "owner/repo"
    }

    if (const size_t cut = pathPart.find_first_of("?#"); cut != std::string::npos) {
        pathPart = pathPart.substr(0, cut);
    }
    while (!pathPart.empty() && pathPart.front() == '/') pathPart.erase(pathPart.begin());
    while (!pathPart.empty() && pathPart.back() == '/') pathPart.pop_back();

    const size_t slash = pathPart.find('/');
    if (slash == std::string::npos) return {"", ""};
    std::string owner = pathPart.substr(0, slash);
    std::string repo = pathPart.substr(slash + 1);
    if (const size_t extra = repo.find('/'); extra != std::string::npos) {
        repo = repo.substr(0, extra);   // owner/repo/tree/main 之类只取仓库名
    }
    if (repo.size() > 4 && repo.compare(repo.size() - 4, 4, ".git") == 0) {
        repo = repo.substr(0, repo.size() - 4);
    }
    return {owner, repo};
}

std::string GithubApi::maskUrl(const std::string& url) {
    std::string out = url;

    // ① userinfo：scheme://[user[:pass]@]host 或无 scheme 的 user@host:path
    if (const size_t scheme = out.find("://"); scheme != std::string::npos) {
        const size_t authStart = scheme + 3;
        const size_t pathStart = out.find_first_of("/?#", authStart);
        const size_t at = out.find('@', authStart);
        if (at != std::string::npos && (pathStart == std::string::npos || at < pathStart)) {
            out.replace(authStart, at - authStart, "***");
        }
    } else if (const size_t at = out.find('@'); at != std::string::npos) {
        const size_t slash = out.find('/');
        if (slash == std::string::npos || at < slash) out.replace(0, at, "***");
    }

    // ② 查询参数里的凭据键
    out = maskQuery(std::move(out));

    // ③ 已知 token 前缀（可能出现在路径 / 查询值里）
    scrubTokenPrefixes(out);
    return out;
}

} // namespace platform::github
