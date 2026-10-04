// src/module_claude_proxy/quota/opencode_go_provider.cpp
// OpenCode Go 余量 provider（上游端点第一方未文档化）。
// GET https://opencode.ai/zen/go/v1/usage，Authorization: Bearer <workspace key>（与推理侧只认 x-api-key 相反，不可互换）；
// 200 → usage.{rolling,weekly,monthly}.percent；窗口只按上游 key 名映射，绝不按 resetsAt 排序猜周期。
// 坑：percent==0 时 resetsAt 是占位值（丢弃重置时间但保留窗口）；403（key 有效但无 Go 订阅）与 401 分开；三窗口全解析不出 = Shape。
#include "opencode_go_provider.h"

#include "quota_percent.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "core/result.h"
#include "platform/json.h"

namespace module::claude::quota {
namespace {

constexpr char kUsageUrl[] = "https://opencode.ai/zen/go/v1/usage";
// 余量是后台刷新，取短超时，不占 Request 默认的 30s 连接。
constexpr long kUsageTimeoutMs = 15000;

// 上游 key → 规范键（数组序 = 展示序）。
struct WindowSpec {
    const char* upstreamKey;
    const char* canonicalKey;
};
constexpr WindowSpec kWindows[] = {
    {"rolling", "five_hour"},
    {"weekly", "weekly"},
    {"monthly", "monthly"},
};

QuotaResult failure(QuotaError error, std::string message) {
    QuotaResult result;
    result.error = error;
    result.errorMessage = std::move(message);
    return result;
}

// 数字与数字字符串都收（上游给过 "percent": "12"）。
bool parsePercent(const platform::json::JsonValue& value, double* out) {
    if (value.isNumber()) {
        // 数字双分支读取：yyjson 把整数归 Int 分支（只填 intVal），只读 asDouble() 会恒返 0，把整数 percent 读成「0% 已用」。故为 0 时回退 asInt；小数原样不截断。
        *out = value.asDouble() != 0.0 ? value.asDouble()
                                       : static_cast<double>(value.asInt());
        return true;
    }
    if (!value.isString()) return false;
    const std::string text = value.asString();
    if (text.empty()) return false;
    const char* begin = text.c_str();
    char* end = nullptr;
    const double parsed = std::strtod(begin, &end);
    if (end == begin || end == nullptr || *end != '\0') return false;
    *out = parsed;
    return true;
}

// resetsAt：ISO8601 字符串原样透传。非字符串一律当「没有重置时间」——仓库无 UTC 格式化设施（core/time.h 只有本地时区），宁可空也不把裸时间戳塞给 UI。
std::string resetTime(const platform::json::JsonValue& value) {
    if (value.isString()) return value.asString();
    return {};
}

// 逐窗口防御解析：缺失 / 非对象 / percent 不可解析 → 跳过该窗口，三窗口全空由调用方判 Shape。
std::vector<QuotaWindow> parseWindows(const platform::json::JsonObject& usage) {
    std::vector<QuotaWindow> windows;
    for (const WindowSpec& spec : kWindows) {
        const platform::json::JsonValue window = usage.get(spec.upstreamKey);
        if (!window.isObject()) continue;
        const platform::json::JsonObject fields = window.asObject();
        double percent = 0.0;
        if (!parsePercent(fields.get("percent"), &percent)) continue;
        QuotaWindow out;
        out.key = spec.canonicalKey;
        out.usedPercent = clampUsedPercent(percent);
        if (percent > 0.0) out.resetsAt = resetTime(fields.get("resetsAt"));
        windows.push_back(std::move(out));
    }
    return windows;
}

std::string toLower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

struct UrlParts {
    std::string host;
    std::string path;  // 已去 query / fragment 与尾部 '/'
};

UrlParts splitUrl(const std::string& rawUrl) {
    std::string rest = toLower(rawUrl);
    const std::size_t scheme = rest.find("://");
    if (scheme != std::string::npos) rest.erase(0, scheme + 3);

    const std::size_t authorityEnd = rest.find_first_of("/?#");
    std::string authority = rest.substr(0, authorityEnd);
    std::string path =
        authorityEnd == std::string::npos ? std::string() : rest.substr(authorityEnd);

    const std::size_t at = authority.rfind('@');  // 去 userinfo
    if (at != std::string::npos) authority.erase(0, at + 1);
    const std::size_t colon = authority.find(':');  // 去端口
    if (colon != std::string::npos) authority.erase(colon);

    const std::size_t query = path.find_first_of("?#");
    if (query != std::string::npos) path.erase(query);
    while (!path.empty() && path.back() == '/') path.pop_back();

    return UrlParts{authority, path};
}

bool isOpencodeAiHost(const std::string& host) {
    constexpr char kDomain[] = "opencode.ai";
    const std::size_t domainLength = sizeof(kDomain) - 1;
    if (host == kDomain) return true;
    // 标签边界：只认「.opencode.ai」结尾，挡 notopencode.ai / opencode.ai.evil.com 伪造域
    return host.size() > domainLength &&
           host.compare(host.size() - domainLength, domainLength, kDomain) == 0 &&
           host[host.size() - domainLength - 1] == '.';
}

class OpenCodeGoProvider final : public QuotaProvider {
public:
    std::string id() const override { return "opencode_go"; }
    std::string displayName() const override { return "OpenCode Go"; }

    // 沿用 profile 的 API Key 做 Bearer，不吃额外字段。
    std::vector<ProviderField> fields() const override { return {}; }

    bool matchesBaseUrl(const std::string& baseUrl) const override {
        const UrlParts parts = splitUrl(baseUrl);
        if (!isOpencodeAiHost(parts.host)) return false;
        // 两档 base：/zen/go 与 /zen/go/v1；Zen 按量版 /zen/v1 没有用量 API，不命中。
        return parts.path == "/zen/go" || parts.path == "/zen/go/v1";
    }

    QuotaResult fetch(const ProviderConfig& cfg, const HttpSendFn& http) const override {
        // 空缝 → Transient：不许直接调用触发 std::bad_function_call。
        if (!http) {
            return failure(QuotaError::Transient, "余量查询通道未就绪，请稍后重试");
        }

        platform::http::Request request;
        request.method = "GET";
        request.url = kUsageUrl;
        // 只认 Authorization: Bearer（与推理侧 x-api-key 相反，不可互换）；apiKey 永不进日志。
        request.headers["Authorization"] = "Bearer " + cfg.apiKey;
        request.headers["Accept"] = "application/json";
        request.timeoutMs = kUsageTimeoutMs;

        const core::Result<platform::http::Response> sent = http(request);
        if (sent.isFail()) {
            return failure(QuotaError::Transient,
                           "余量接口请求失败：" + sent.errorMessage());
        }
        const platform::http::Response response =
            sent.valueOr(platform::http::Response{});

        // 传输层失败的第二形态（Result::fail 之外）：ok(Response{statusCode=0, networkError=true})。
        // 不拦会落进下面 <200 分支，用户看到不存在的「HTTP 0」，排查被指向 HTTP 而真因在网络层。
        if (response.networkError) {
            return failure(QuotaError::Transient,
                           "网络请求失败：" + response.errorMessage);
        }

        if (response.statusCode == 401) {
            return failure(QuotaError::Auth, "API Key 无效或已过期（HTTP 401）");
        }
        if (response.statusCode == 403) {
            // key 有效但该 workspace 没有 Go 订阅 —— 改密钥没用、得换套餐，故与 401 分开。
            return failure(QuotaError::Entitlement,
                           "API Key 有效，但该工作区没有 OpenCode Go 订阅（HTTP 403）");
        }
        if (response.statusCode < 200 || response.statusCode >= 300) {
            // 5xx / 429 / 其余 4xx 归可重试，不冒充鉴权或订阅结论。
            return failure(QuotaError::Transient,
                           "余量接口返回 HTTP " +
                               std::to_string(response.statusCode));
        }

        const core::Result<platform::json::JsonValue> parsed =
            platform::json::parse(response.body);
        if (parsed.isFail()) {
            return failure(QuotaError::Shape, "余量响应不是合法 JSON");
        }
        const platform::json::JsonValue usage = parsed.value().asObject().get("usage");
        if (!usage.isObject()) {
            // 扁平形态（rollingUsage 等）也不兼容，直接报形态不认识。
            return failure(QuotaError::Shape,
                           "余量响应形态不认识（缺少 usage 对象）");
        }

        std::vector<QuotaWindow> windows = parseWindows(usage.asObject());
        if (windows.empty()) {
            // 报错而非渲染「0% 已用」空卡（那等于说还有 100% 可用）。
            return failure(
                QuotaError::Shape,
                "余量响应形态不认识（未找到 rolling / weekly / monthly 窗口）");
        }

        QuotaResult result;
        result.kind = QuotaKind::Tiered;
        result.windows = std::move(windows);
        return result;
    }
};

}  // namespace

QuotaProvider* createOpenCodeGoProvider() {
    static OpenCodeGoProvider instance;  // 进程内单例
    return &instance;
}

}  // namespace module::claude::quota
