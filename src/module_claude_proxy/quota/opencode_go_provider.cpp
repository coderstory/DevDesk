// src/module_claude_proxy/quota/opencode_go_provider.cpp
// G1：OpenCode Go 余量 provider
// 移植自 cc-switch src-tauri/src/services/coding_plan.rs（bfaaba16）：
//   detect_provider:47-50 / parse_opencode_go_tiers:731-762 / query_opencode_go:764-833
//   （对应的 cc-switch 自测 :1507-1586 是 fixture 来源）
//
// 上游端点（第一方但**未文档化**的路由，2026-08-11 上线当天就改过一次形态）：
//   GET https://opencode.ai/zen/go/v1/usage
//   Authorization: Bearer <workspace api key>   ← 与推理侧只认 x-api-key 相反，不可互换
//   200 → {"usage":{"rolling"|"weekly"|"monthly":
//                    {"status":"ok"|"rate-limited","percent":0-100,"resetsAt":ISO8601}}}
//
// 三条从 cc-switch 记录的真实坑里照搬的判定（各自都有对应用例）：
//   1. 窗口映射**只按上游 key 名**（rolling→five_hour / weekly→weekly /
//      monthly→monthly），绝不按 resetsAt 排序猜周期——周期末尾周桶会比 5h
//      桶更早重置，排序猜必错。
//   2. percent == 0 时上游给的 resetsAt 是「now + 窗口时长」的占位值
//      （整窗早已清零）→ 丢弃重置时间，但**窗口本身保留**。
//   3. 403（key 有效但该 workspace 没有 Go 订阅）与 401（key 无效）分开；
//      三窗口一个都解析不出来 = 响应形态不认识（Shape），**不是**渲染空卡。
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
// cc-switch 用 15s（coding_plan.rs:773）。余量是后台刷新，不该占着连接 30s
//（platform::http::Request 的默认值）。
constexpr long kUsageTimeoutMs = 15000;

// 上游 key → 规范键（数组序 = 展示序）。逐字对齐 coding_plan.rs:732-736 的 WINDOWS。
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

// cc-switch parse_f64（coding_plan.rs:95-99）：数字与**数字字符串**都收
//（上游给过 `"percent": "12"`）。
bool parsePercent(const platform::json::JsonValue& value, double* out) {
    if (value.isNumber()) {
        // 数字双分支读取（照抄 pet_config.cpp:92-97 / todo_view_state.cpp:94-97
        // 同款先例）：yyjson 把 `12` 归 Int 分支（Kind::Int 只填 intVal），
        // 只读 asDouble() 会**恒返 0** —— 整数 percent 会被静默读成「0% 已用」
        //（等于告诉用户「还有 100% 可用」）。故 asDouble 为 0 时回退 asInt；
        // Double 分支（38.7 一类）原样返回，小数不截断。
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

// resetsAt：ISO8601 字符串原样透传。cc-switch 的 extract_reset_time
//（coding_plan.rs:78-92）还兼容数字时间戳（秒 / 毫秒 → ISO8601），那条分支服务
// 的是火山等其它供应商的形态（会话无活跃窗口回 -1）；本端点的契约就是 ISO8601
// 串，仓库又没有 UTC 格式化设施（core/time.h 只有本地时区），故非字符串一律
// 当「没有重置时间」——宁可空，也不把裸时间戳当字符串原样塞给 UI。
std::string resetTime(const platform::json::JsonValue& value) {
    if (value.isString()) return value.asString();
    return {};
}

// 逐窗口防御解析（coding_plan.rs:741-760）：窗口缺失 / 非对象 / percent 缺失或
// 不可解析 → **跳过该窗口**，不整体失败；三窗口全空由调用方判为 Shape。
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

// ── base_url 探测（detect_provider:47-50 的加固版）──
// cc-switch 用整串 `contains("opencode.ai/zen/go")`；裸子串会被 notopencode.ai、
// evil.com/opencode.ai/... 这类伪造 host 命中。这里按 G0 头文件 :135-136 的要求
// 改成 **host 标签边界 + path 精确**两段判定（行为上仍是「/zen/go 与 /zen/go/v1
// 两档命中」，即 cc-switch 自测 :1508-1520 那条）。
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
    // 标签边界：只认「.opencode.ai」结尾 → 挡掉 notopencode.ai /
    // opencode.ai.evil.com 一类伪造域（裸子串包含会把它们全放进来）
    return host.size() > domainLength &&
           host.compare(host.size() - domainLength, domainLength, kDomain) == 0 &&
           host[host.size() - domainLength - 1] == '.';
}

class OpenCodeGoProvider final : public QuotaProvider {
public:
    std::string id() const override { return "opencode_go"; }
    std::string displayName() const override { return "OpenCode Go"; }

    // 沿用 profile 的 API Key（ProviderConfig::apiKey）做 Bearer，不吃额外字段
    //（G6 弹窗因此不渲染额外输入框）。
    std::vector<ProviderField> fields() const override { return {}; }

    bool matchesBaseUrl(const std::string& baseUrl) const override {
        const UrlParts parts = splitUrl(baseUrl);
        if (!isOpencodeAiHost(parts.host)) return false;
        // 两档 base：/zen/go（claude 系直连 /messages）与 /zen/go/v1（codex /
        // opencode / pi 走 Chat）。Zen 按量版 /zen/v1 没有任何用量 / 余额 API
        //（实测 404），刻意不命中。
        return parts.path == "/zen/go" || parts.path == "/zen/go/v1";
    }

    QuotaResult fetch(const ProviderConfig& cfg, const HttpSendFn& http) const override {
        // 空缝 → Transient：不许直接调用触发 std::bad_function_call
        //（项目铁律「不抛 C++ 异常」；契约见 quota_provider.h:113-116）
        if (!http) {
            return failure(QuotaError::Transient, "余量查询通道未就绪，请稍后重试");
        }

        platform::http::Request request;
        request.method = "GET";
        request.url = kUsageUrl;
        // 余量端点只认 Authorization: Bearer —— 与推理侧只认 x-api-key 正好
        // 相反，不可互换（coding_plan.rs:767-771）。apiKey 永不进日志。
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

        // 传输层失败有两种形态，缺一不可（G3/G4 都查这条，G1/G2 原来漏了）：
        //   ① Result::fail —— 已在上面拦下
        //   ② ok(Response{statusCode=0, networkError=true}) —— 超时 / 连接失败 /
        //      读体中断。不查的话 statusCode 0 会落进下面的 <200 分支，用户看到
        //      「余量接口返回 HTTP 0」——一个**不存在的**状态码，把排查方向指向
        //      上游 HTTP 状态，而真因（超时）在网络层，一个字都不出现。
        if (response.networkError) {
            return failure(QuotaError::Transient,
                           "网络请求失败：" + response.errorMessage);
        }

        if (response.statusCode == 401) {
            return failure(QuotaError::Auth, "API Key 无效或已过期（HTTP 401）");
        }
        if (response.statusCode == 403) {
            // Zen 与 Go 共用同一把 workspace key：403 = key 本身有效，但该
            // workspace 没有 Go 订阅 —— 改密钥没用、得换套餐，故与 401 分开
            //（coding_plan.rs:783-789）。
            return failure(QuotaError::Entitlement,
                           "API Key 有效，但该工作区没有 OpenCode Go 订阅（HTTP 403）");
        }
        if (response.statusCode < 200 || response.statusCode >= 300) {
            // 5xx / 429 / 其余 4xx 一律归可重试（G8 保留上次成功值 + 可能过期
            // 角标），不冒充鉴权或订阅结论。
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
            // 含旧扁平形态（rollingUsage / usagePercent / resetInSec，2026-08-11
            // 上线当天仅存在 53 分钟）：不作兼容，直接报形态不认识。
            return failure(QuotaError::Shape,
                           "余量响应形态不认识（缺少 usage 对象）");
        }

        std::vector<QuotaWindow> windows = parseWindows(usage.asObject());
        if (windows.empty()) {
            // 未文档化端点可能再次变形：报错，而不是渲染一张「0% 已用」的空卡
            //（那等于告诉用户「还有 100% 可用」）。
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
    static OpenCodeGoProvider instance;  // 进程内单例（无状态，只借不还）
    return &instance;
}

}  // namespace module::claude::quota
