// src/module_claude_proxy/upstream_dialect.cpp
// 上游方言实现：鉴权头 / 必需会话头 / UA 策略 / 模型名归一
#include "upstream_dialect.h"

#include "claude_service.h"  // claudeUpstreamSessionId()
#include "core/logging.h"
#include "json_scan.h"
#include "platform/json.h"

#include <algorithm>
#include <cctype>
#include <cstddef>

namespace module::claude {

namespace {

// ==================== 头部键大小写不敏感操作 ====================
// HTTP 头名大小写不敏感，而 headers 是 std::map<std::string,...>（大小写敏感）：
// 同名不同大小写在 map 里会并存 → 上游收到重复头（流式路径曾踩过：
// 客户端的小写 authorization 与注入的 Authorization 并存）。一律先清干净。

std::string toLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

void eraseHeaderCI(std::map<std::string, std::string>& headers,
                   const std::string& keyLower) {
    for (auto it = headers.begin(); it != headers.end();) {
        if (toLowerAscii(it->first) == keyLower) {
            it = headers.erase(it);
        } else {
            ++it;
        }
    }
}

bool hasHeaderCI(const std::map<std::string, std::string>& headers,
                 const std::string& keyLower) {
    for (const auto& kv : headers) {
        if (toLowerAscii(kv.first) == keyLower) return true;
    }
    return false;
}

// 覆盖写入一个头：先清掉同名的大小写变体，再以 canonicalName 的拼写写入
void putHeader(std::map<std::string, std::string>& headers,
               const std::string& canonicalName, const std::string& value) {
    eraseHeaderCI(headers, toLowerAscii(canonicalName));
    headers[canonicalName] = value;
}

// ==================== URL host 解析 ====================

// 取 URL 的 host（小写，去 scheme / userinfo / port / path）；解析不出返回空串
std::string hostOf(const std::string& url) {
    size_t begin = 0;
    const size_t scheme = url.find("://");
    if (scheme != std::string::npos) begin = scheme + 3;
    size_t end = url.find_first_of("/?#", begin);
    if (end == std::string::npos) end = url.size();

    std::string authority = url.substr(begin, end - begin);
    const size_t at = authority.find('@');       // 去掉 userinfo
    if (at != std::string::npos) authority.erase(0, at + 1);
    const size_t colon = authority.find(':');    // 去掉端口
    if (colon != std::string::npos) authority.erase(colon);
    return toLowerAscii(authority);
}

// host 是 opencode.ai 本身或其子域（zen.opencode.ai）；notopencode.ai 不算
bool isOpencodeHost(const std::string& host) {
    const std::string suffix = "opencode.ai";
    if (host == suffix) return true;
    if (host.size() <= suffix.size() + 1) return false;
    return host.compare(host.size() - suffix.size() - 1, suffix.size() + 1,
                        "." + suffix) == 0;
}

// ==================== 模型名标记 ====================

constexpr const char* kOneMMarker = "[1M]";
constexpr size_t kOneMMarkerLen = 4;

std::string trimTrailingWs(const std::string& s) {
    size_t e = s.size();
    while (e > 0 && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(0, e);
}

// ==================== 顶层 JSON 字段定位（只读扫描） ====================
// 目标：拿到顶层 "model" 值的**原始字节区间**，从而只替换这一小段，
// 其余字节原样发给上游。不用「解析整个 body 再重新序列化」的理由：
// platform::json 的序列化按 std::map 输出（键序变字母序）且浮点走 %.17g
// （0.7 → 0.69999999999999996）——对透明代理来说是无谓的扰动。这里只做
// 值区间的定点替换，不做全量重写。
// 通用扫描器在 json_scan.h（CPX-11 与 streaming_proxy 的重复实现合并），
// 本文件只保留「值必须是字符串」这一层判定。

// 在顶层对象里找字符串字段 key，回填其值（含引号）的字节区间 [begin, end)；
// 非顶层同名键 / 值不是字符串 / 结构非法 → false
bool findTopLevelStringSpan(const std::string& s, const std::string& key,
                            size_t& begin, size_t& end) {
    if (!json_scan::findTopLevelValueSpan(s, key, begin, end)) return false;
    return begin < s.size() && s[begin] == '"';
}

} // namespace

// ==================== UpstreamDialect ====================

UpstreamDialect UpstreamDialect::anthropicDefault() {
    return UpstreamDialect{Kind::AnthropicDefault};
}

UpstreamDialect UpstreamDialect::opencodeZen() {
    return UpstreamDialect{Kind::OpencodeZen};
}

UpstreamDialect UpstreamDialect::forBaseUrl(const std::string& baseUrl,
                                            const std::string& overrideText) {
    // 显式覆盖优先（审计 CPX-13）：host 推断对自建网关 / 内网镜像无效
    //（后缀像 opencode.ai 的镜像被判成 zen，或反过来），profile 可钉住方言。
    // 未覆盖（空串 / 未知值）→ 原样走 host 推断，行为与从前逐项一致
    if (const auto forced = dialectKindFromText(overrideText)) {
        return UpstreamDialect{*forced};
    }
    return isOpencodeHost(hostOf(baseUrl)) ? opencodeZen() : anthropicDefault();
}

bool UpstreamDialect::stripModelSuffix() const {
    // 只有明确不认该标记的网关才剥；默认方言保持透传（既有的 bailian
    // 网关带 [1M] 能正常返回，剥了反而可能改行为）
    return kind == Kind::OpencodeZen;
}

void UpstreamDialect::applyHeaders(std::map<std::string, std::string>& headers,
                                   const std::string& apiKey,
                                   AuthField authField) const {
    // 1. 鉴权：客户端的凭据一律丢弃（代理只用 profile 的 key，客户端那份
    //    打到这个网关本来就无效），再按认证字段注入
    eraseHeaderCI(headers, "x-api-key");
    eraseHeaderCI(headers, "authorization");
    if (!apiKey.empty()) {
        if (authField != AuthField::Bearer) {
            headers["x-api-key"] = apiKey;
        }
        if (authField != AuthField::ApiKey) {
            // 兼容期望 Bearer 的 Anthropic-compatible 网关（bailian 等）；
            // 真 Anthropic API 收 x-api-key、忽略 Bearer
            headers["Authorization"] = "Bearer " + apiKey;
        }
    }

    // 2. User-Agent：无条件覆盖（理由见 kProxyUserAgent 注释）
    putHeader(headers, "User-Agent", kProxyUserAgent);

    // 3. 上游契约头：客户端没带才补（带了说明客户端更清楚要哪个版本）
    if (!hasHeaderCI(headers, "anthropic-version")) {
        headers["anthropic-version"] = "2023-06-01";
    }

    // 4. 方言专属头
    if (kind == Kind::OpencodeZen &&
        !hasHeaderCI(headers, "x-opencode-session")) {
        headers["x-opencode-session"] = claudeUpstreamSessionId();
    }
}

bool UpstreamDialect::normalizeBody(std::string& body) const {
    if (!stripModelSuffix()) return false;
    return normalizeModelInBody(body);
}

// ==================== 方言覆盖文本（CPX-13） ====================

std::optional<UpstreamDialect::Kind> dialectKindFromText(const std::string& text) {
    if (text == "anthropic_default") {
        return UpstreamDialect::Kind::AnthropicDefault;
    }
    if (text == "opencode_zen") return UpstreamDialect::Kind::OpencodeZen;
    return std::nullopt;   // 空串 / 未知 / 拼写错误 → 未覆盖（读侧兜底）
}

const char* dialectKindToText(UpstreamDialect::Kind kind) {
    switch (kind) {
        case UpstreamDialect::Kind::OpencodeZen: return "opencode_zen";
        case UpstreamDialect::Kind::AnthropicDefault: break;
    }
    return "anthropic_default";
}

// ==================== 模型名归一 ====================

std::string stripOneMSuffix(const std::string& model) {
    const std::string trimmed = trimTrailingWs(model);
    if (trimmed.size() < kOneMMarkerLen) return model;

    const size_t pos = trimmed.size() - kOneMMarkerLen;
    for (size_t k = 0; k < kOneMMarkerLen; ++k) {
        if (std::toupper(static_cast<unsigned char>(trimmed[pos + k])) !=
            static_cast<unsigned char>(kOneMMarker[k])) {
            return model;  // 未命中：原样返回（含尾部空白）
        }
    }
    return trimTrailingWs(trimmed.substr(0, pos));
}

bool normalizeModelInBody(std::string& body) {
    if (body.empty()) return false;

    // 先定位顶层 "model" 值的字节区间（不命中就直接返回，body 零扰动）
    size_t begin = 0;
    size_t end = 0;
    if (!findTopLevelStringSpan(body, "model", begin, end)) return false;

    // 真值走 platform::json（解码语义统一），字节区间由上面的扫描给出
    auto parsed = platform::json::parse(body);
    if (parsed.isFail() || !parsed.value().isObject()) return false;
    const platform::json::JsonValue modelVal =
        parsed.value().asObject().get("model");
    if (!modelVal.isString()) return false;

    const std::string model = modelVal.asString();
    const std::string stripped = stripOneMSuffix(model);
    if (stripped == model) return false;

    auto encoded = platform::json::stringify(platform::json::makeString(stripped));
    if (encoded.isFail()) return false;
    const std::string& newValue = encoded.value();

    // 一致性护栏：扫描到的原文片段必须就是解析出来的那个值（转义/重复键
    // 等罕见形态下两者可能不一致）——不一致就放弃改写，绝不改坏 body
    auto oldEncoded = platform::json::stringify(platform::json::makeString(model));
    if (oldEncoded.isFail() ||
        body.compare(begin, end - begin, oldEncoded.value()) != 0) {
        LOG_WARN("上游方言：模型名区间与解析值不一致，跳过 [1M] 剥离");
        return false;
    }

    body.replace(begin, end - begin, newValue);
    return true;
}

} // namespace module::claude
