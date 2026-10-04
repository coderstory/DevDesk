// src/module_claude_proxy/quota/deepseek_provider.cpp
// DeepSeek 余额 provider —— Balance 型：只有余额，无窗口 / 无 used / 无 reset。
// GET https://api.deepseek.com/user/balance，Bearer，超时 15s；2xx 解析 balance_infos[0].total_balance / currency。
// 恒返回 QuotaResult、不抛异常：传输失败 → Transient，401 → Auth，403 / is_available=false → Entitlement，
// 结构不认识 → Shape，空 key → NotConfigured。
#include "deepseek_provider.h"

#include <cctype>
#include <cstdlib>
#include <string>
#include <utility>

#include "platform/json.h"

namespace module::claude::quota {
namespace {

// 余额接口固定域名，不从 cfg.baseUrl 派生：余量接口域名由 provider 自己写死，baseUrl 只是事实记录。
constexpr const char* kBalanceUrl = "https://api.deepseek.com/user/balance";
constexpr long kTimeoutMs = 15000;
constexpr const char* kDeepSeekDomain = "api.deepseek.com";

QuotaResult failure(QuotaError error, std::string message) {
    QuotaResult r;
    r.kind = QuotaKind::Balance;  // provider 的静态形态：失败态也归 Balance 型
    r.error = error;
    r.errorMessage = std::move(message);
    return r;
}

// URL → host（剥 scheme / userinfo / 端口 / path，小写化）。
std::string hostOf(const std::string& url) {
    std::string s = url;
    const std::size_t scheme = s.find("://");
    if (scheme != std::string::npos) s = s.substr(scheme + 3);
    const std::size_t authorityEnd = s.find_first_of("/?#");
    if (authorityEnd != std::string::npos) s = s.substr(0, authorityEnd);
    const std::size_t at = s.rfind('@');
    if (at != std::string::npos) s = s.substr(at + 1);
    const std::size_t colon = s.find(':');
    if (colon != std::string::npos) s = s.substr(0, colon);
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

// host 标签边界匹配：host == domain 或以 "." + domain 结尾，防 api.deepseek.com.evil.com 伪造。
bool hostMatches(const std::string& host, const std::string& domain) {
    if (host == domain) return true;
    if (host.size() <= domain.size() + 1) return false;
    if (host.compare(host.size() - domain.size(), domain.size(), domain) != 0) return false;
    return host[host.size() - domain.size() - 1] == '.';
}

// JSON number 与字符串数字都收（上游返回字符串小数）。strtod 不抛异常，要求整串是数字（"12abc" 判不认识）。
bool numberField(const platform::json::JsonObject& obj, const char* key, double& out) {
    const platform::json::JsonValue value = obj.get(key);
    if (value.isNumber()) {
        out = value.asDouble();
        return true;
    }
    if (!value.isString()) return false;
    const std::string text = value.asString();
    if (text.empty()) return false;
    const char* begin = text.c_str();
    char* end = nullptr;
    const double parsed = std::strtod(begin, &end);
    if (end == begin) return false;  // 一个数字字符都没有
    while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') ++end;
    if (*end != '\0') return false;  // 尾随垃圾（"12abc"）→ 不认识
    out = parsed;
    return true;
}

class DeepSeekProvider final : public QuotaProvider {
public:
    std::string id() const override { return "deepseek"; }
    std::string displayName() const override { return "DeepSeek"; }
    // 只吃 (base_url, api_key) 两个入参 → 无额外字段，密钥沿用 profile 凭据。
    std::vector<ProviderField> fields() const override { return {}; }

    bool matchesBaseUrl(const std::string& baseUrl) const override {
        return hostMatches(hostOf(baseUrl), kDeepSeekDomain);
    }

    QuotaResult fetch(const ProviderConfig& cfg, const HttpSendFn& http) const override {
        // 空缝不许 std::bad_function_call。
        if (!http) return failure(QuotaError::Transient, "HTTP 发送缝未注入");
        // 空 key 是确定性失败且不发请求，归 NotConfigured（引导去配置，不画 0% 条）。
        if (cfg.apiKey.empty()) return failure(QuotaError::NotConfigured, "未配置 API 密钥");

        platform::http::Request req;
        req.method = "GET";
        req.url = kBalanceUrl;
        req.timeoutMs = kTimeoutMs;
        req.headers["Authorization"] = "Bearer " + cfg.apiKey;
        req.headers["Accept"] = "application/json";

        const core::Result<platform::http::Response> sent = http(req);
        if (sent.isFail()) {
            // 传输失败 → Transient（可重试，保留上次成功值 + 打「可能过期」角标）。
            return failure(QuotaError::Transient, "网络请求失败：" + sent.errorMessage());
        }
        const platform::http::Response& resp = sent.value();
        if (resp.networkError) {
            return failure(QuotaError::Transient, "网络请求失败：" + resp.errorMessage);
        }
        // 状态码按「调用方该做什么」分类：401 → Auth；403 → Entitlement（改密钥没用）；408/429/5xx → Transient；
        // 其余 4xx → Shape（确定性失败，不是返回空卡）
        if (resp.statusCode == 401) {
            return failure(QuotaError::Auth, "鉴权失败（HTTP 401）：请检查 API 密钥");
        }
        if (resp.statusCode == 403) {
            return failure(QuotaError::Entitlement,
                           "无权访问（HTTP 403）：该账号没有此产品的权限");
        }
        if (resp.statusCode == 408 || resp.statusCode == 429 || resp.statusCode >= 500) {
            return failure(QuotaError::Transient,
                           "上游暂时不可用（HTTP " + std::to_string(resp.statusCode) + "）");
        }
        if (resp.statusCode < 200 || resp.statusCode >= 300) {
            return failure(QuotaError::Shape,
                           "上游返回异常状态（HTTP " + std::to_string(resp.statusCode) + "）");
        }

        // 先拿完整响应体再解析：读体失败是瞬时（上面已覆盖），解析失败才确定性。
        const core::Result<platform::json::JsonValue> parsed = platform::json::parse(resp.body);
        if (parsed.isFail()) return failure(QuotaError::Shape, "响应不是合法 JSON");
        const platform::json::JsonValue& body = parsed.value();
        if (!body.isObject()) return failure(QuotaError::Shape, "响应不是 JSON 对象");
        const platform::json::JsonObject root = body.asObject();

        // is_available 缺失 / 非 bool → 按 true（无明确证据不判欠费）；只有显式 false 才 Entitlement。
        const platform::json::JsonValue available = root.get("is_available");
        if (available.isBool() && !available.asBool()) {
            // Insufficient balance：改密钥没用，得充值。
            return failure(QuotaError::Entitlement,
                           "余额不足，请充值（上游标记 Insufficient balance）");
        }

        // balance_infos 必须为非空数组，否则按 Shape 报错（不静默返回空卡）。
        const platform::json::JsonValue infos = root.get("balance_infos");
        if (!infos.isArray() || infos.size() <= 0) {
            return failure(QuotaError::Shape, "响应缺少 balance_infos 数组（或为空）");
        }
        const platform::json::JsonValue first = infos[0];
        if (!first.isObject()) return failure(QuotaError::Shape, "balance_infos[0] 不是对象");
        const platform::json::JsonObject entry = first.asObject();

        double total = 0.0;
        if (!numberField(entry, "total_balance", total)) {
            return failure(QuotaError::Shape, "balance_infos[0] 缺少可解析的 total_balance");
        }
        const platform::json::JsonValue currency = entry.get("currency");
        if (!currency.isString() || currency.asString().empty()) {
            return failure(QuotaError::Shape, "balance_infos[0] 缺少 currency");
        }

        // 多币种取数组首条（主币种在首条，顺序稳定）；planName 留空（上游没给套餐名）。
        QuotaResult out;
        out.kind = QuotaKind::Balance;
        out.balanceRemaining = total;
        out.currency = currency.asString();
        return out;
    }
};

}  // namespace

QuotaProvider* createDeepSeekProvider() {
    static DeepSeekProvider instance;
    return &instance;
}

}  // namespace module::claude::quota
