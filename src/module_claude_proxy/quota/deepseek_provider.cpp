// src/module_claude_proxy/quota/deepseek_provider.cpp
// DeepSeek 余额 provider（G3）—— **Balance 型**
//
// 移植 cc-switch（bfaaba16）src-tauri/src/services/balance.rs 的 DeepSeek 分支：
//   detect_provider :26-43（deepseek 项：base_url 含 "api.deepseek.com"）
//   query_deepseek  :70-146
//     GET https://api.deepseek.com/user/balance
//     Authorization: Bearer {api_key} / Accept: application/json / 超时 15 s（:78-81）
//     2xx → 解析 { is_available, balance_infos: [{currency, total_balance,
//     granted_balance, topped_up_balance}] }（:105-139）
//     remaining = total_balance（:122）、unit = currency 原样（:129）、
//     is_available == false → "Insufficient balance"（:131-135）
//
// DeepSeek **只有余额**：无时间窗口、无 used、无 reset（cc-switch 未实现套餐 / 速率
// 限制）→ QuotaResult.windows 恒空（契约 quota_provider.h:66-68「Balance 的 windows
// 必须为空，不许为凑数硬造一个窗口」）。
//
// 错误语义按契约 quota_provider.h:11-18 / :44-52：恒返回 QuotaResult、不抛异常、
// 错误只走 error/errorMessage。cc-switch 的「瞬时 vs 确定性」两分类（:4-10）在
// DevDesk 拆成五态：传输失败 → Transient；401 → Auth；403 / is_available=false →
// Entitlement；结构不认识 → Shape；空 key → NotConfigured。
#include "deepseek_provider.h"

#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <utility>

#include "platform/json.h"

namespace module::claude::quota {
namespace {

// 余额接口固定域名：cc-switch :78 的字面量。**不从 cfg.baseUrl 派生**——契约
// quota_provider.h:97-100：baseUrl 只是「这个 profile 本来连哪儿」的事实记录，
// 余量接口用哪个域名由 provider 自己写死。
constexpr const char* kBalanceUrl = "https://api.deepseek.com/user/balance";
constexpr long kTimeoutMs = 15000;  // cc-switch :81（Duration::from_secs(15)）
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

// host 的**标签边界**匹配：host == domain，或 host 以 "." + domain 结尾。
// cc-switch :28 用的是裸子串 `url.contains("api.deepseek.com")`，会把
// `api.deepseek.com.evil.com` / `evil.com/?next=api.deepseek.com` 也算命中；契约
// quota_provider.h:135-139 明确要求标签边界（防 api.foo.com.evil.com 伪造）。
bool hostMatches(const std::string& host, const std::string& domain) {
    if (host == domain) return true;
    if (host.size() <= domain.size() + 1) return false;
    if (host.compare(host.size() - domain.size(), domain.size(), domain) != 0) return false;
    return host[host.size() - domain.size() - 1] == '.';
}

// cc-switch :415-420 parse_f64_field：JSON number 与字符串数字两种形态都收
//（DeepSeek 实际返回的是字符串小数，如 "110.00"）。strtod 不抛异常（项目铁律
//「不抛 C++ 异常」），并要求整串都是数字（"12abc" / "" 判结构不认识）。
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
    // cc-switch 的 get_balance 只吃 (base_url, api_key) 两个入参（:426）→ 无额外
    // 字段，密钥沿用 profile 凭据（G6 弹窗据此不渲染额外输入框）
    std::vector<ProviderField> fields() const override { return {}; }

    bool matchesBaseUrl(const std::string& baseUrl) const override {
        return hostMatches(hostOf(baseUrl), kDeepSeekDomain);
    }

    QuotaResult fetch(const ProviderConfig& cfg, const HttpSendFn& http) const override {
        // 空缝不许 std::bad_function_call（契约 :114-116）
        if (!http) return failure(QuotaError::Transient, "HTTP 发送缝未注入");
        // cc-switch :427-433：空 key 是确定性失败且**不发请求**。DevDesk 归
        // NotConfigured（配置不完整 → G7 引导去配置，不画 0% 条）
        if (cfg.apiKey.empty()) return failure(QuotaError::NotConfigured, "未配置 API 密钥");

        platform::http::Request req;
        req.method = "GET";
        req.url = kBalanceUrl;
        req.timeoutMs = kTimeoutMs;
        req.headers["Authorization"] = "Bearer " + cfg.apiKey;
        req.headers["Accept"] = "application/json";

        const core::Result<platform::http::Response> sent = http(req);
        if (sent.isFail()) {
            // cc-switch :85-88（Err = 瞬时传输失败）→ Transient：可重试，
            // G8 保留上次成功值 + 打「可能过期」角标
            return failure(QuotaError::Transient, "网络请求失败：" + sent.errorMessage());
        }
        const platform::http::Response& resp = sent.value();
        if (resp.networkError) {
            return failure(QuotaError::Transient, "网络请求失败：" + resp.errorMessage);
        }
        // 状态码分类：cc-switch :90-97 只分「鉴权（401/403）/ 其余」两类；契约
        // quota_provider.h:44-52 按「调用方该做什么」拆开——
        //   401 → Auth（改密钥有用）
        //   403 → Entitlement（凭据有效、产品权限没有 → 改密钥没用）
        //   408/429/5xx → Transient（超时 / 限流 / 上游故障，可重试）
        //   其余 4xx → Shape（确定性失败：报错，不是返回空卡）
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

        // 先拿完整响应体再解析：读体失败是瞬时（上面 Result::fail 已覆盖），
        // 解析失败才是确定性（cc-switch :99-108 同样的先后顺序）
        const core::Result<platform::json::JsonValue> parsed = platform::json::parse(resp.body);
        if (parsed.isFail()) return failure(QuotaError::Shape, "响应不是合法 JSON");
        const platform::json::JsonValue& body = parsed.value();
        if (!body.isObject()) return failure(QuotaError::Shape, "响应不是 JSON 对象");
        const platform::json::JsonObject root = body.asObject();

        // is_available 缺失 / 非 bool → 按 true（cc-switch :110-113 unwrap_or(true)：
        // 没有明确证据不判欠费）。**只有显式 false** 才 Entitlement。
        const platform::json::JsonValue available = root.get("is_available");
        if (available.isBool() && !available.asBool()) {
            // cc-switch :131-135 的 "Insufficient balance"：改密钥没用，得充值
            return failure(QuotaError::Entitlement,
                           "余额不足，请充值（上游标记 Insufficient balance）");
        }

        // balance_infos 必须是**非空数组**。cc-switch :116-139 对空数组/缺键是
        // 「静默成功但无数据」；契约 quota_provider.h:46-47 要求这种情况报 Shape
        //（「该报错，不是返回空卡」）——这是有意的本地收紧。
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

        // 多币种取**表序首条**：cc-switch :116-138 把 balance_infos 逐条各成一行、
        // 按数组序展示（不排序、不挑币种、不换算）→ 首行即其展示序第一条（DeepSeek
        // 主币种在首条，顺序稳定）。DevDesk 契约只有单标量，取首条最贴近该语义。
        // cc-switch 另外把 plan_name 也填 currency（:125），那是它模型没有 currency
        // 列的代偿；DevDesk 有 currency 专列 → planName 留空（上游没给套餐名）。
        QuotaResult out;
        out.kind = QuotaKind::Balance;
        out.balanceRemaining = total;        // cc-switch :122 remaining = total_balance
        out.currency = currency.asString();  // cc-switch :129 unit = currency，原样不换算
        return out;
    }
};

}  // namespace

QuotaProvider* createDeepSeekProvider() {
    static DeepSeekProvider instance;  // 进程内单例（provider 无状态，只借不还）
    return &instance;
}

}  // namespace module::claude::quota
