#include "mcp_tools_builtin.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/logging.h"
#include "core/str_util.h"
#include "html_to_markdown.h"
#include "mcp_protocol.h"
#include "platform/json.h"
#include "platform/utf8.h"

namespace module::mcp {

namespace {

#ifdef DEVDESK_VERSION
constexpr const char* kDevDeskVersion = DEVDESK_VERSION;
#else
constexpr const char* kDevDeskVersion = "dev";
#endif

#ifdef DEVDESK_BUILD_TYPE
constexpr const char* kDevDeskBuildType = DEVDESK_BUILD_TYPE;
#else
constexpr const char* kDevDeskBuildType = "unknown";
#endif

#ifdef DEVDESK_PLATFORM
constexpr const char* kDevDeskPlatform = DEVDESK_PLATFORM;
#else
constexpr const char* kDevDeskPlatform = "unknown";
#endif

// devdesk_server_info：无参数，返回固定顺序的 Markdown 列表。工具清单来自构造时
// 注入的注册表引用（模块隔离：不读其它模块数据、不读全局注册表）。
class ServerInfoTool final : public core::mcp::IMcpTool {
public:
    explicit ServerInfoTool(core::mcp::McpToolRegistry& registry)
        : registry_(registry) {}

    std::string name() const override { return "devdesk_server_info"; }

    std::string description() const override {
        return "查询 DevDesk 版本、构建类型、平台、支持的 MCP 协议版本与已注册工具列表";
    }

    std::string inputSchemaJson() const override {
        return R"({"type":"object","properties":{}})";
    }

    core::Result<core::mcp::ToolResult> execute(
        const std::string& /*argumentsJson*/) override {
        std::string text;
        text += "- DevDesk 版本：";
        text += kDevDeskVersion;
        text += "\n- 构建类型：";
        text += kDevDeskBuildType;
        text += "\n- 平台：";
        text += kDevDeskPlatform;
        text += "\n- 支持的 MCP 协议版本：";
        text += kMcpProtocolVersion;
        text += "、";
        text += kMcpProtocolVersionLegacy;

        const std::vector<std::shared_ptr<core::mcp::IMcpTool>> tools =
            registry_.list();
        text += "\n- 已注册工具：";
        text += std::to_string(tools.size());
        text += " 个";
        if (!tools.empty()) {
            text += "（";
            for (std::size_t i = 0; i < tools.size(); ++i) {
                if (i > 0) text += ", ";
                text += tools[i]->name();
            }
            text += "）";
        }

        return core::Result<core::mcp::ToolResult>::ok(
            core::mcp::ToolResult{false, std::move(text)});
    }

private:
    core::mcp::McpToolRegistry& registry_;
};


// ===== B2:web_fetch_markdown(spec §6.1) =====

using platform::json::JsonObject;
using platform::json::JsonValue;

constexpr std::size_t kMaxBodyBytes = 2 * 1024 * 1024;  // 原始响应体上限(spec §6.1-9)
constexpr long long kDefaultMaxChars = 60000;
constexpr long long kMinMaxChars = 1000;
constexpr long long kMaxMaxChars = 200000;
constexpr long long kPerHopTimeoutMs = 10000;

std::string lowerAsciiCopy(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool digitsOnlyStr(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

// 所有工具输出(含错误文本、截断附注)返回前必须 sanitize,保证进 JSON 前良构(spec §6.1-8)
core::mcp::ToolResult okToolResult(std::string text) {
    return core::mcp::ToolResult{false, platform::utf8::sanitize(std::move(text))};
}

core::mcp::ToolResult errToolResult(std::string text) {
    return core::mcp::ToolResult{true, platform::utf8::sanitize(std::move(text))};
}

// ---- URL 解析与重定向目标解析 ----

struct ParsedUrl {
    bool valid = false;
    bool userinfo = false;  // authority 含 '@'：本工具无合法用途，一律拒（防 a@127.0.0.1 绕过）
    std::string scheme;    // 小写
    std::string host;      // 小写;IPv6 无方括号
    bool isV6 = false;
    std::string port;      // 空 = 协议默认端口
    std::string path;      // 以 '/' 起,含 query/fragment
    std::string hostPort;  // host(:port);IPv6 还原方括号
};

ParsedUrl parseUrl(const std::string& url) {
    ParsedUrl out;
    const std::size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos || schemeEnd == 0) return out;
    out.scheme = lowerAsciiCopy(url.substr(0, schemeEnd));
    const std::size_t authorityStart = schemeEnd + 3;
    const std::size_t authorityEnd = url.find_first_of("/?#", authorityStart);
    const std::string authority = url.substr(
        authorityStart,
        (authorityEnd == std::string::npos ? url.size() : authorityEnd) - authorityStart);
    // userinfo 一律拒：libcurl 按 RFC 3986 把 @ 后段当真主机实连,而自研解析把
    // 「a@127.0.0.1」整体当 host → 非字面 IP → 放行,SSRF 绕过。本工具无合法用途。
    if (authority.find('@') != std::string::npos) {
        out.userinfo = true;
        return out;
    }
    out.path = authorityEnd == std::string::npos ? "/" : url.substr(authorityEnd);
    if (out.path.empty() || out.path[0] != '/') out.path = "/" + out.path;
    std::string hostPart;
    if (!authority.empty() && authority[0] == '[') {
        const std::size_t close = authority.find(']');
        if (close == std::string::npos) return out;
        hostPart = authority.substr(1, close - 1);
        out.isV6 = true;
        const std::size_t colon = authority.find(':', close);
        if (colon != std::string::npos) out.port = authority.substr(colon + 1);
    } else {
        const std::size_t colon = authority.rfind(':');
        if (colon != std::string::npos) {
            hostPart = authority.substr(0, colon);
            out.port = authority.substr(colon + 1);
            if (!digitsOnlyStr(out.port)) return out;
        } else {
            hostPart = authority;
        }
    }
    out.host = lowerAsciiCopy(hostPart);
    if (out.host.empty()) return out;
    out.hostPort = out.isV6 ? "[" + out.host + "]" : out.host;
    if (!out.port.empty()) out.hostPort += ":" + out.port;
    out.valid = true;
    return out;
}

// 空串 = 无法解析为合法 http(s) 目标(spec §6.1-4)
std::string resolveRedirect(const ParsedUrl& base, const std::string& location) {
    const std::string loc = core::str::trimAscii(location);
    if (loc.empty()) return "";
    const std::size_t schemeEnd = loc.find("://");
    if (schemeEnd != std::string::npos) {
        const std::string scheme = lowerAsciiCopy(loc.substr(0, schemeEnd));
        return (scheme == "http" || scheme == "https") ? loc : "";
    }
    if (loc.compare(0, 2, "//") == 0) return base.scheme + ":" + loc;
    if (loc[0] == '/') return base.scheme + "://" + base.hostPort + loc;
    if (loc[0] == '#') return "";
    // 相对当前目录解析 + 点段归一化(spec §6.1-4)
    auto splitSegments = [](const std::string& s) {
        std::vector<std::string> parts;
        std::string cur;
        for (const char c : s) {
            if (c == '/') {
                parts.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
        parts.push_back(cur);
        return parts;
    };
    // 相对目录 = base.path 去掉最后一段(文件名)后的目录(spec §6.1-4)
    std::string baseDir = base.path;
    const std::size_t lastSlash = baseDir.find_last_of('/');
    baseDir = lastSlash == std::string::npos ? "/" : baseDir.substr(0, lastSlash + 1);
    std::vector<std::string> stack;
    for (const std::string& s : splitSegments(baseDir)) {
        if (!s.empty()) stack.push_back(s);
    }
    const bool trailingSlash = loc.back() == '/';
    for (const std::string& s : splitSegments(loc)) {
        if (s.empty() || s == ".") continue;
        if (s == "..") {
            if (!stack.empty()) stack.pop_back();
            continue;
        }
        stack.push_back(s);
    }
    std::string path = "/";
    for (std::size_t i = 0; i < stack.size(); ++i) {
        path += stack[i];
        if (i + 1 < stack.size()) path += "/";
    }
    if (trailingSlash && path.back() != '/') path += "/";
    return base.scheme + "://" + base.hostPort + path;
}

bool parseIpv4(const std::string& host, unsigned char out[4]) {
    unsigned cur = 0;
    int digits = 0;
    int idx = 0;
    for (const char c : host) {
        if (c >= '0' && c <= '9') {
            cur = cur * 10 + static_cast<unsigned>(c - '0');
            if (++digits > 3 || cur > 255) return false;
        } else if (c == '.') {
            if (digits == 0 || idx >= 3) return false;
            out[idx++] = static_cast<unsigned char>(cur);
            cur = 0;
            digits = 0;
        } else {
            return false;
        }
    }
    if (digits == 0 || idx != 3) return false;
    out[3] = static_cast<unsigned char>(cur);
    return true;
}

int hexValueForIp(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool hexGroup(const std::string& token, unsigned short& out) {
    if (token.empty() || token.size() > 4) return false;
    unsigned v = 0;
    for (const char c : token) {
        const int d = hexValueForIp(c);
        if (d < 0) return false;
        v = v * 16 + static_cast<unsigned>(d);
    }
    out = static_cast<unsigned short>(v);
    return true;
}

// 一段冒号分隔的组序列(允许末段 IPv4 形式)
bool parseGroupRun(const std::string& s, std::vector<unsigned short>& groups) {
    if (s.empty()) return true;  // "::" 两侧可为空
    std::vector<std::string> tokens;
    std::string cur;
    for (const char c : s) {
        if (c == ':') {
            tokens.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    tokens.push_back(cur);
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i].find('.') != std::string::npos) {
            if (i != tokens.size() - 1) return false;  // IPv4 尾只允许在最后
            unsigned char b[4];
            if (!parseIpv4(tokens[i], b)) return false;
            groups.push_back(static_cast<unsigned short>((b[0] << 8) | b[1]));
            groups.push_back(static_cast<unsigned short>((b[2] << 8) | b[3]));
        } else {
            unsigned short v = 0;
            if (!hexGroup(tokens[i], v)) return false;
            groups.push_back(v);
        }
    }
    return true;
}

bool isBlockedIpv4(const unsigned char b[4]) {
    if (b[0] == 127 || b[0] == 0 || b[0] == 10) return true;  // 环回/全零/10/8
    if (b[0] == 172 && b[1] >= 16 && b[1] <= 31) return true;  // 172.16/12
    if (b[0] == 192 && b[1] == 168) return true;               // 192.168/16
    if (b[0] == 169 && b[1] == 254) return true;               // 169.254/16 链路本地
    if (b[0] >= 224 && b[0] <= 239) return true;               // 224.0.0.0/4 组播(OWASP 点名)
    return false;
}

bool parseIpv6(const std::string& host, unsigned short out[8]) {
    const std::size_t dc = host.find("::");
    if (dc != std::string::npos) {
        if (host.find("::", dc + 1) != std::string::npos) return false;  // 多个 "::"
        std::vector<unsigned short> left;
        std::vector<unsigned short> right;
        if (!parseGroupRun(host.substr(0, dc), left)) return false;
        if (!parseGroupRun(host.substr(dc + 2), right)) return false;
        if (left.size() + right.size() > 7) return false;  // "::" 至少压一组零
        for (std::size_t i = 0; i < 8; ++i) {
            if (i < left.size()) {
                out[i] = left[i];
            } else if (i < 8 - right.size()) {
                out[i] = 0;
            } else {
                out[i] = right[i - (8 - right.size())];
            }
        }
        return true;
    }
    std::vector<unsigned short> groups;
    if (!parseGroupRun(host, groups)) return false;
    if (groups.size() != 8) return false;
    for (std::size_t i = 0; i < 8; ++i) out[i] = groups[i];
    return true;
}

// 空串 = 放行;非空 = 拒绝原因。仅字面地址,DNS 域名放行(重绑定风险 spec §14.2 已接受)。
// host 是否为「数字编码形态」：每段都是纯十进制，或 0x/0X 前缀十六进制（前导 0
// 的八进制段本就是纯数字，一并覆盖）。字母域名（含十六进制词如 deadbeef.ca）不算。
bool isNumericEncodedHost(const std::string& host) {
    if (host.empty() || host.front() == '.' || host.back() == '.') return false;
    std::size_t start = 0;
    while (start <= host.size()) {
        const std::size_t dot = host.find('.', start);
        const std::string seg = host.substr(
            start, (dot == std::string::npos ? host.size() : dot) - start);
        if (seg.empty()) return false;
        const bool hex = seg.size() >= 2 && seg[0] == '0' &&
                         (seg[1] == 'x' || seg[1] == 'X');
        for (std::size_t pos = hex ? 2 : 0; pos < seg.size(); ++pos) {
            const char c = seg[pos];
            const bool ok = (c >= '0' && c <= '9') ||
                            (hex && ((c >= 'a' && c <= 'f') ||
                                     (c >= 'A' && c <= 'F')));
            if (!ok) return false;
        }
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return true;
}

// inet_aton 语义的 IPv4 数字形态解析：1-4 段；段可十进制 / 0x 十六进制 / 前导 0
// 八进制；不足 4 段时末段按剩余字节数计权（"127.1"=127.0.0.1，"2130706433" 与
// "0x7f000001"=127.0.0.1）。溢出 / 段值越界 / 裸 0x / 超 4 段 / 尾点 → false
//（调用方对数字形态解析失败一律 fail-closed，防填充零等变体绕过）。
bool parseIpv4NumericForms(const std::string& host, unsigned char out[4]) {
    unsigned long long parts[4] = {0, 0, 0, 0};
    int partCount = 0;
    std::size_t i = 0;
    const std::size_t n = host.size();
    if (n == 0) return false;
    for (;;) {
        if (partCount >= 4) return false;
        unsigned long long v = 0;
        int base = 10;
        if (i + 1 < n && host[i] == '0' && (host[i + 1] == 'x' || host[i + 1] == 'X')) {
            base = 16;
            i += 2;
            if (i >= n || host[i] == '.') return false;  // 裸 0x
        } else if (i < n && host[i] == '0' && i + 1 < n && host[i + 1] != '.') {
            base = 8;
            ++i;
        }
        bool anyDigit = false;
        while (i < n && host[i] != '.') {
            const char c = host[i];
            int d = -1;
            if (c >= '0' && c <= '9') {
                d = c - '0';
            } else if (base == 16) {
                if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            }
            if (d < 0) return false;
            if (v > 0xFFFFFFFFULL / static_cast<unsigned long long>(base)) return false;
            v = v * static_cast<unsigned long long>(base) +
                static_cast<unsigned long long>(d);
            if (v > 0xFFFFFFFFULL) return false;
            anyDigit = true;
            ++i;
        }
        if (!anyDigit) return false;
        parts[partCount++] = v;
        if (i == n) break;
        ++i;  // 跳过 '.'
        if (i == n) return false;  // 尾点
    }
    unsigned long long addr = 0;
    if (partCount == 4) {
        if (parts[0] > 255 || parts[1] > 255 || parts[2] > 255 || parts[3] > 255) {
            return false;
        }
        addr = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    } else if (partCount == 3) {
        if (parts[0] > 255 || parts[1] > 255 || parts[2] > 0xFFFFFFULL) return false;
        addr = (parts[0] << 24) | (parts[1] << 16) | parts[2];
    } else if (partCount == 2) {
        if (parts[0] > 255 || parts[1] > 0xFFFFFFULL) return false;
        addr = (parts[0] << 24) | parts[1];
    } else {
        addr = parts[0];
    }
    out[0] = static_cast<unsigned char>((addr >> 24) & 0xff);
    out[1] = static_cast<unsigned char>((addr >> 16) & 0xff);
    out[2] = static_cast<unsigned char>((addr >> 8) & 0xff);
    out[3] = static_cast<unsigned char>(addr & 0xff);
    return true;
}

std::string ssrfRejectReason(const std::string& host) {
    if (host == "localhost") return "拒绝访问本机地址（SSRF 防护）: " + host;
    // 尾点 host fail-closed：WHATWG/curl 剥尾点后按字面 IP 实连,而字面解析判
    // 「非字面」落域名放行；公网域名尾点同拒（保守口径,不给绕过留口）
    if (!host.empty() && host.back() == '.') {
        return "非法目标地址（SSRF 防护）: " + host;
    }
    if (host.find(':') != std::string::npos) {
        unsigned short g[8];
        // 解析失败一律 fail-closed：zone-id（[fe80::1%25eth0]）、畸形 v6 等
        // 系统解析器可能仍接受的形态，不得走「放行当域名」分支
        if (!parseIpv6(host, g)) return "无法解析的目标地址（SSRF 防护）: " + host;
        const bool headZero6 = g[0] == 0 && g[1] == 0 && g[2] == 0 &&
                               g[3] == 0 && g[4] == 0 && g[5] == 0;  // ::/96 前缀
        const bool headZero7 = headZero6 && g[6] == 0;
        if (headZero7 && g[7] == 0) return "拒绝访问未指定地址（SSRF 防护）: " + host;
        if (headZero7 && g[7] == 1) return "拒绝访问本机地址（SSRF 防护）: " + host;
        if (headZero6) {
            // ::/96 IPv4-compatible（::127.0.0.1 类，RFC 4291 已废弃但系统解析器
            // 普遍仍接受）：还原 v4 再查表
            const unsigned char b[4] = {static_cast<unsigned char>(g[6] >> 8),
                                        static_cast<unsigned char>(g[6] & 0xff),
                                        static_cast<unsigned char>(g[7] >> 8),
                                        static_cast<unsigned char>(g[7] & 0xff)};
            if (isBlockedIpv4(b)) return "拒绝访问内网/本机地址（SSRF 防护）: " + host;
            return "";  // 全零前缀下 fe80/fc00 检查必不中，直接收敛
        }
        if (g[0] == 0 && g[1] == 0 && g[2] == 0 && g[3] == 0 && g[4] == 0 &&
            g[5] == 0xffff) {  // IPv4 映射地址:还原 v4 再查表([::ffff:127.0.0.1] 类)
            const unsigned char b[4] = {static_cast<unsigned char>(g[6] >> 8),
                                        static_cast<unsigned char>(g[6] & 0xff),
                                        static_cast<unsigned char>(g[7] >> 8),
                                        static_cast<unsigned char>(g[7] & 0xff)};
            if (isBlockedIpv4(b)) return "拒绝访问内网/本机地址（SSRF 防护）: " + host;
        }
        if ((g[0] & 0xFFC0) == 0xFE80) return "拒绝访问链路本地地址（SSRF 防护）: " + host;
        if ((g[0] & 0xFE00) == 0xFC00) return "拒绝访问私有地址（SSRF 防护）: " + host;
        if ((g[0] & 0xFF00) == 0xFF00) return "拒绝访问组播地址（SSRF 防护）: " + host;  // ff00::/8
        return "";
    }
    unsigned char b[4];
    if (parseIpv4(host, b) || parseIpv4NumericForms(host, b)) {
        // 数字编码形态（inet_aton 语义）还原成 v4 查表，不得按域名放行
        if (isBlockedIpv4(b)) return "拒绝访问内网/本机地址（SSRF 防护）: " + host;
        return "";
    }
    if (isNumericEncodedHost(host)) {
        // 长得像数字 IP 但解析失败（越界/超段/裸 0x）：fail-closed
        return "无法解析的目标地址（SSRF 防护）: " + host;
    }
    return "";  // DNS 域名放行(重绑定风险 spec §14.2 已接受)
}

// ---- 响应头/编码/截断 ----

// 响应头按大小写不敏感查表(spec §6.1-6;send 返回头保留服务端原始大小写)
const std::string* findHeaderCi(const std::map<std::string, std::string>& headers,
                                const std::string& lowerKey) {
    const auto it = headers.find(lowerKey);
    if (it != headers.end()) return &it->second;
    for (const auto& kv : headers) {
        if (lowerAsciiCopy(kv.first) == lowerKey) return &kv.second;
    }
    return nullptr;
}

struct ContentTypeParts {
    std::string mainType;  // ';' 前主类型,小写
    std::string charset;   // 归一后(去空白/引号、小写);空 = 未声明
    std::string raw;       // 原始值(报错用)
};

ContentTypeParts parseContentType(const std::map<std::string, std::string>& headers) {
    ContentTypeParts ct;
    const std::string* raw = findHeaderCi(headers, "content-type");
    if (raw == nullptr) return ct;
    ct.raw = *raw;
    const std::size_t semi = raw->find(';');
    ct.mainType = lowerAsciiCopy(core::str::trimAscii(
        semi == std::string::npos ? *raw : raw->substr(0, semi)));
    std::size_t start = semi + 1;
    while (semi != std::string::npos && start < raw->size()) {
        const std::size_t end = raw->find(';', start);
        const std::string param =
            raw->substr(start, (end == std::string::npos ? raw->size() : end) - start);
        const std::size_t eq = param.find('=');
        if (eq != std::string::npos &&
            lowerAsciiCopy(core::str::trimAscii(param.substr(0, eq))) == "charset") {
            std::string v;
            for (const char c : param.substr(eq + 1)) {
                if (c != ' ' && c != '\t' && c != '"' && c != '\'') v += c;  // 去空白与引号
            }
            ct.charset = lowerAsciiCopy(v);
            break;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return ct;
}

// 按码点数截断;truncatedOut = 是否发生截断(spec §6.1-10)。非 2xx 错误前缀的
// 「前 200 字符」同用本函数——按字节截会把 CJK 页面砍到 ~66 字(F3 收口)。
std::string truncateByChars(const std::string& s, const long long maxChars,
                            bool* truncatedOut) {
    long long chars = 0;
    std::size_t i = 0;
    while (i < s.size() && chars < maxChars) {
        const unsigned char lead = static_cast<unsigned char>(s[i]);
        std::size_t len = 1;
        if (lead >= 0xF0) {
            len = 4;
        } else if (lead >= 0xE0) {
            len = 3;
        } else if (lead >= 0xC0) {
            len = 2;
        }
        if (i + len > s.size()) len = 1;  // 容错:sanitize 后不应出现
        ++chars;
        i += len;
    }
    *truncatedOut = i < s.size();
    return s.substr(0, i);
}

// web_fetch_markdown:GET http(s) 网页 → Markdown。scheme/SSRF 字面量逐跳校验后
// 才触网;手动跟随 301/302/303/307/308 ≤ limits_.maxRedirectHops 跳,所有跳共享
// 总时限;非 2xx 报状态码 + 响应体前 200 字符;charset 白名单外拒绝;输出按
// max_chars(钳制 1000..200000)做 UTF-8 边界截断。
class WebFetchMarkdownTool final : public core::mcp::IMcpTool {
public:
    WebFetchMarkdownTool(Fetcher fetcher, FetchLimits limits)
        : fetcher_(std::move(fetcher)), limits_(limits) {}

    std::string name() const override { return "web_fetch_markdown"; }

    std::string description() const override {
        return "抓取 http/https 网页并转换为 Markdown（拒绝本机/内网地址，最多 5 跳重定向，总时限 10 秒）";
    }

    std::string inputSchemaJson() const override {
        return R"({"type":"object","properties":{"url":{"type":"string","description":"http/https 网页地址"},"max_chars":{"type":"integer","description":"输出字符上限，默认 60000，范围 1000..200000"}},"required":["url"]})";
    }

    core::Result<core::mcp::ToolResult> execute(
        const std::string& argumentsJson) override {
        auto parsed = platform::json::parse(argumentsJson);
        if (parsed.isFail() || !parsed.value().isObject()) {
            return core::Result<core::mcp::ToolResult>::ok(
                errToolResult("参数必须是 JSON 对象"));
        }
        const JsonObject args = parsed.value().asObject();
        const JsonValue urlValue = args.get("url");
        if (!urlValue.isString() || urlValue.asString().empty()) {
            return core::Result<core::mcp::ToolResult>::ok(
                errToolResult("缺少 url（必须是非空字符串）"));
        }
        long long maxChars = kDefaultMaxChars;
        const JsonValue maxCharsValue = args.get("max_chars");
        if (maxCharsValue.isNumber()) maxChars = maxCharsValue.asInt();
        if (maxChars < kMinMaxChars) maxChars = kMinMaxChars;
        if (maxChars > kMaxMaxChars) maxChars = kMaxMaxChars;

        // 抓取循环:每跳先做不触网校验(scheme + SSRF 字面量),再受总时限约束
        const auto deadline = std::chrono::steady_clock::now() + limits_.totalBudget;
        platform::http::Response resp;
        int redirectHops = 0;
        std::string current = urlValue.asString();
        for (;;) {
            const ParsedUrl target = parseUrl(current);
            // userinfo 先于 valid 判：parseUrl 对 @ 目标提前返回(valid=false),
            // 若让「仅支持 http/https」抢先,攻击形态被掩盖成普通格式错
            if (target.userinfo) {
                return core::Result<core::mcp::ToolResult>::ok(
                    errToolResult("拒绝包含 userinfo 的目标地址（SSRF 防护）: " + current));
            }
            if (!target.valid ||
                (target.scheme != "http" && target.scheme != "https")) {
                return core::Result<core::mcp::ToolResult>::ok(
                    errToolResult("仅支持 http/https 地址: " + current));
            }
            const std::string ssrf = ssrfRejectReason(target.host);
            if (!ssrf.empty()) {
                return core::Result<core::mcp::ToolResult>::ok(errToolResult(ssrf));
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                return core::Result<core::mcp::ToolResult>::ok(
                    errToolResult("抓取超时：所有重定向跳合计总时限 10 秒已耗尽"));
            }
            platform::http::Request request;
            request.method = "GET";
            request.url = current;
            const long long remainMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
                    .count();
            request.timeoutMs =
                static_cast<long>(std::min<long long>(kPerHopTimeoutMs, remainMs));
            auto fetched = fetcher_(request);
            if (fetched.isFail()) {
                return core::Result<core::mcp::ToolResult>::ok(
                    errToolResult("网络请求失败：" + fetched.errorMessage()));
            }
            resp = fetched.value();
            if (resp.networkError) {
                return core::Result<core::mcp::ToolResult>::ok(
                    errToolResult("网络请求失败：" +
                                  (resp.errorMessage.empty()
                                       ? "HTTP " + std::to_string(resp.statusCode)
                                       : resp.errorMessage)));
            }
            if (resp.statusCode >= 200 && resp.statusCode < 300) break;
            const bool redirectStatus = resp.statusCode == 301 || resp.statusCode == 302 ||
                                        resp.statusCode == 303 || resp.statusCode == 307 ||
                                        resp.statusCode == 308;
            if (!redirectStatus) {
                bool prefixTruncated = false;
                return core::Result<core::mcp::ToolResult>::ok(
                    errToolResult("HTTP " + std::to_string(resp.statusCode) + "：" +
                                  truncateByChars(resp.body, 200,
                                                  &prefixTruncated)));
            }
            if (redirectHops >= limits_.maxRedirectHops) {
                return core::Result<core::mcp::ToolResult>::ok(
                    errToolResult("重定向次数过多（最多 5 跳）"));
            }
            const std::string* location = findHeaderCi(resp.headers, "location");
            if (location == nullptr || location->empty()) {
                return core::Result<core::mcp::ToolResult>::ok(
                    errToolResult("重定向缺少 Location 头"));
            }
            const std::string next = resolveRedirect(target, *location);
            if (next.empty()) {
                return core::Result<core::mcp::ToolResult>::ok(errToolResult(
                    "重定向 Location 无法解析为 http/https 地址: " + *location));
            }
            ++redirectHops;
            current = next;
        }

        // 2 MiB 截断(spec §6.1-9):先截断再转换/返回
        const bool overSize = resp.body.size() > kMaxBodyBytes;
        std::string body = overSize ? resp.body.substr(0, kMaxBodyBytes) : resp.body;
        const ContentTypeParts ct = parseContentType(resp.headers);
        const bool htmlType =
            ct.mainType == "text/html" || ct.mainType == "application/xhtml+xml";
        const bool textType =
            ct.mainType.empty() || ct.mainType == "text/plain" ||
            ct.mainType == "text/markdown" || ct.mainType == "application/json" ||
            ct.mainType == "text/xml" || ct.mainType == "application/xml";
        if (!htmlType && !textType) {
            return core::Result<core::mcp::ToolResult>::ok(
                errToolResult("不支持的内容类型: " + ct.raw));
        }
        if (!ct.charset.empty() && ct.charset != "utf-8" && ct.charset != "utf8" &&
            ct.charset != "us-ascii" && ct.charset != "ascii") {
            return core::Result<core::mcp::ToolResult>::ok(
                errToolResult("不支持非 UTF-8 编码: " + ct.charset));
        }
        body = platform::utf8::sanitize(body);  // 先 sanitize 再转换/返回(spec §6.1-8)
        std::string text;
        if (htmlType) {
            text = htmlToMarkdown(body);
            if (core::str::trimAscii(text).empty()) {
                // 空可见文本非错误(spec §6.1-11)
                return core::Result<core::mcp::ToolResult>::ok(
                    okToolResult("页面未提取到文本（可能是 JS 渲染页面）"));
            }
        } else {
            text = std::move(body);  // 空正文非错误
        }
        if (overSize) text += "（原始响应超过 2 MiB，已截断）";
        bool truncated = false;
        text = truncateByChars(text, maxChars, &truncated);
        if (truncated) {
            text += "（已截断到 " + std::to_string(maxChars) + " 字符）";
        }
        return core::Result<core::mcp::ToolResult>::ok(okToolResult(std::move(text)));
    }

private:
    Fetcher fetcher_;
    FetchLimits limits_;
};

} // namespace

void registerBuiltinMcpTools(core::mcp::McpToolRegistry& registry,
                             Fetcher fetcher, const FetchLimits& limits) {
    auto info = registry.registerTool(std::make_shared<ServerInfoTool>(registry));
    if (info.isFail()) {
        LOG_ERROR("MCP 内置工具注册失败: " + info.errorMessage());
    }
    auto fetch =
        registry.registerTool(std::make_shared<WebFetchMarkdownTool>(fetcher, limits));
    if (fetch.isFail()) {
        LOG_ERROR("MCP 内置工具注册失败: " + fetch.errorMessage());
    }
}

} // namespace module::mcp
