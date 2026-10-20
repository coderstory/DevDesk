// NetFetchTool 实现：scheme 校验 → platform::http GET → 32KB 预算截断 → JSON 字符串回喂。
//
// 截断口径：上限与 agent_loop.cpp 的 kMaxToolResultBytes 同值（32KB）。contentJson
// 用 JSON 字符串承载——AgentLoop::toolResultText 对 JSON 字符串原样透传，故本工具在
// 源头把「提示前缀 + 正文 + 截断尾注」整体控制在预算内，loop 侧不再二切，LLM 拿到的
// 就是完整可读文本。
#include "net_fetch_tool.h"

#include "platform/json.h"
#include "platform/utf8.h"

#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace module::agent {

namespace {

using core::Result;
using core::ToolArgs;
using core::ToolResult;
namespace pj = platform::json;

// 单条响应文本硬上限（字节）：与 agent_loop.cpp 的 kMaxToolResultBytes 对齐
constexpr size_t kMaxTextBytes = 32u * 1024u;

std::string toLower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

// 大小写不敏感前缀判定（URL scheme、Content-Type 头均不保证大小写）
bool startsWithCI(std::string_view text, std::string_view prefix) {
    if (text.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(text[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

// 大小写不敏感查响应头（http_client 的 headerCallback 保留服务器原始大小写）
std::string findHeaderCI(const std::map<std::string, std::string>& headers,
                         const std::string& key) {
    const std::string lowerKey = toLower(key);
    for (const auto& [k, v] : headers) {
        if (toLower(k) == lowerKey) return v;
    }
    return {};
}

// 文本家族判定：text/* 与 json/xml/javascript/yaml/csv 等事实上的文本类型；
// 其余（image/*、application/octet-stream、application/pdf…）按二进制处理。
// 缺 Content-Type 按文本宽限（相当多服务器不回该头）。
bool isTextualContentType(const std::string& contentType) {
    const std::string ct = toLower(contentType);
    if (ct.empty()) return true;
    if (startsWithCI(ct, "text/")) return true;
    for (std::string_view kw : {"json", "xml", "javascript", "yaml", "csv",
                                "urlencoded"}) {
        if (ct.find(kw) != std::string::npos) return true;
    }
    return false;
}

// 截断点回退到 UTF-8 字符边界：落在多字节序列中间会拼出半个字（与 agent_loop 同款）
size_t utf8BoundaryBack(const std::string& text, size_t cut) {
    while (cut > 0 &&
           (static_cast<unsigned char>(text[cut]) & 0xC0u) == 0x80u) {
        --cut;
    }
    return cut;
}

// curl 错误串是英文（curl_easy_strerror），映射成可读中文分类；原文保留在错误文本里
std::string classifyNetworkError(const std::string& raw) {
    const std::string lower = toLower(raw);
    if (lower.find("timeout") != std::string::npos ||
        lower.find("timed out") != std::string::npos) {
        return "请求超时";
    }
    if (lower.find("resolve") != std::string::npos) {
        return "域名解析失败（DNS）";
    }
    if (lower.find("connect") != std::string::npos) {
        return "连接失败";
    }
    if (lower.find("ssl") != std::string::npos) {
        return "TLS/SSL 错误";
    }
    return "网络错误";
}

// contentJson = JSON 字符串（toolResultText 对 JSON 字符串原样透传给 LLM）
Result<ToolResult> textResult(bool isError, const std::string& text) {
    auto encoded = pj::stringify(pj::makeString(text));
    if (encoded.isFail()) {
        return Result<ToolResult>::fail(
            core::ErrorCode::Internal,
            "抓取结果序列化失败: " + encoded.errorMessage());
    }
    return Result<ToolResult>::ok(ToolResult{isError, encoded.value()});
}

} // namespace

NetFetchTool::NetFetchTool(NetFetcher fetcher, long timeoutMs)
    : fetcher_(std::move(fetcher)),
      timeoutMs_(timeoutMs > 0 ? timeoutMs : kNetFetchDefaultTimeoutMs) {}

core::Result<platform::http::Response> defaultNetFetcher(
    const platform::http::Request& request) {
    return platform::http::send(request);
}

std::string NetFetchTool::schemaJson() const {
    return R"({"name":"net.fetch","description":"联网抓取：GET 一个 http/https URL，返回响应文本（超 32KB 截断）。外部请求，仅 GET，10 秒超时","input_schema":{"type":"object","properties":{"url":{"type":"string","description":"完整 URL，仅支持 http/https"}},"required":["url"]}})";
}

Result<ToolResult> NetFetchTool::execute(const ToolArgs& args) {
    // 参数解析：{"url":"..."}；非对象 / 缺 url / 空串都给可读错误
    std::string url;
    if (!args.json.empty()) {
        auto parsed = pj::parse(args.json);
        if (parsed.isFail() || !parsed.value().isObject()) {
            return textResult(
                true, "net.fetch 参数错误：需要 JSON 对象 {\"url\": \"...\"}");
        }
        auto urlValue = parsed.value().asObject().get("url");
        if (urlValue.isString()) url = urlValue.asString();
    }
    if (url.empty()) {
        return textResult(true,
                          "net.fetch 参数错误：缺少必填参数 url（http/https "
                          "完整地址）");
    }

    // scheme 白名单：仅 http/https，其余直接拒绝（不发起任何网络请求）
    if (!startsWithCI(url, "http://") && !startsWithCI(url, "https://")) {
        return textResult(true,
                          "net.fetch 参数错误：仅支持 http/https，收到 " + url);
    }

    platform::http::Request req;
    req.method = "GET";
    req.url = url;
    req.timeoutMs = timeoutMs_;

    auto sent = fetcher_(req);
    if (sent.isFail()) {
        const std::string raw =
            sent.errorMessage().empty() ? "未知网络错误" : sent.errorMessage();
        return textResult(true, "net.fetch 失败：" + classifyNetworkError(raw) +
                                    "（" + raw + "），URL: " + url);
    }
    const platform::http::Response& resp = sent.value();

    // 非 2xx → 可读错误（含状态码与响应片段作原因线索）
    if (resp.statusCode < 200 || resp.statusCode >= 300) {
        std::string text = "net.fetch 失败：HTTP 状态码 " +
                           std::to_string(resp.statusCode) + "（非 2xx），URL: " +
                           url;
        // 重定向特殊注记：curl 未跟随（FOLLOWLOCATION 未开），把 Location 指给
        // 调用方，agent 可自行换 URL 重抓
        const std::string location = findHeaderCI(resp.headers, "Location");
        if (resp.statusCode >= 300 && resp.statusCode < 400 && !location.empty()) {
            text += "，重定向未自动跟随，Location: " + location;
        }
        if (!resp.body.empty()) {
            // 片段先清洗再拼接（串内非法 UTF-8 会卡死 loop 侧的 parse 自检路径）
            text += "，响应片段: " +
                    platform::utf8::sanitize(resp.body.substr(0, 200));
        }
        return textResult(true, text);
    }

    // 2xx：先清洗后截断（notification.cpp 同款口径）——二进制/坏编码字节换 U+FFFD，
    // 保证 contentJson 始终是可被 loop 侧严格 parse 的良构 JSON 字符串
    const std::string body = platform::utf8::sanitize(resp.body);

    // 非文本家族 → 前缀提示 + 同 32KB 预算截断（剩余字节对 LLM 无意义）
    const std::string contentType = findHeaderCI(resp.headers, "Content-Type");
    if (!isTextualContentType(contentType)) {
        // 截断与否先按「较长前缀」的预算判定（保守），再定前缀文案——小体积二进制
        // 不该谎称「已截断」
        const std::string truncPrefix = "二进制/非文本，已截断\n";
        const size_t budget = kMaxTextBytes > truncPrefix.size()
                                  ? kMaxTextBytes - truncPrefix.size()
                                  : 0;
        const bool cut = body.size() > budget;
        const std::string prefix = cut ? truncPrefix : "二进制/非文本\n";
        const size_t take =
            cut ? utf8BoundaryBack(body, budget) : body.size();
        return textResult(false, prefix + body.substr(0, take));
    }

    // 文本：超预算时截断 + 尾注（尾注计入预算，最终文本恰好不超 32KB）
    if (body.size() <= kMaxTextBytes) {
        return textResult(false, body);
    }
    const std::string note = "\n[net.fetch: 响应超过 32KB 已截断，原始 " +
                             std::to_string(resp.body.size()) + " 字节]";
    const size_t budget = kMaxTextBytes > note.size() ? kMaxTextBytes - note.size()
                                                      : 0;
    const size_t cut = utf8BoundaryBack(body, budget);
    return textResult(false, body.substr(0, cut) + note);
}

} // namespace module::agent
