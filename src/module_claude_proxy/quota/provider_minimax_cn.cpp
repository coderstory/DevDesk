// src/module_claude_proxy/quota/provider_minimax_cn.cpp
// MiniMax 中国站余量 provider。端点写死 api.minimaxi.com（profile 的 base_url 是 api.minimax.cn 也照样打它，同一账号体系与 Key）。
// base_resp 存在但 status_code 缺失/非整数 → 算业务错误（上游 unwrap_or(-1) + != 0）。
// 上游给剩余百分比 → 反转成「已用」再裁剪（见 quota_percent.h）；周桶按 current_weekly_status == 1 门控（无周限额套餐该字段为 3，画出来是假周条）。
#include "provider_minimax_cn.h"

#include "quota_percent.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "platform/json.h"

namespace module::claude::quota {
namespace {

// 余量端点只在 api.minimaxi.com / api.minimax.io 有公开文档，故写死、不读 cfg.baseUrl。
constexpr const char* kRemainsUrl =
    "https://api.minimaxi.com/v1/api/openplatform/coding_plan/remains";
constexpr long kTimeoutMs = 15000;

// 探测命中的 host（标签边界匹配，防 api.minimax.cn.example.com 伪造）；国际站 api.minimax.io 是另一个 provider。
constexpr const char* kMatchHosts[] = {"api.minimaxi.com", "api.minimax.cn"};

std::string urlHost(const std::string& urlOrHost) {
    std::string s = urlOrHost;
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());

    const std::size_t scheme = s.find("://");
    if (scheme != std::string::npos) s = s.substr(scheme + 3);
    const std::size_t pathEnd = s.find_first_of("/?#");
    if (pathEnd != std::string::npos) s = s.substr(0, pathEnd);
    const std::size_t at = s.rfind('@');  // userinfo
    if (at != std::string::npos) s = s.substr(at + 1);
    if (!s.empty() && s.front() == '[') {  // IPv6 字面量 [::1]:443
        const std::size_t close = s.find(']');
        s = (close == std::string::npos) ? s.substr(1) : s.substr(1, close - 1);
    } else {
        const std::size_t colon = s.find(':');
        if (colon != std::string::npos) s = s.substr(0, colon);
    }
    while (!s.empty() && s.back() == '.') s.pop_back();
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

// host 是候选之一或候选的子域（标签边界），非裸子串：evilapi.minimax.cn / api.minimax.cn.example.com 不命中。
bool hostMatchesAny(const std::string& urlOrHost) {
    const std::string host = urlHost(urlOrHost);
    if (host.empty()) return false;
    for (const char* candidate : kMatchHosts) {
        const std::string c = candidate;
        if (host == c) return true;
        if (host.size() > c.size() + 1 &&
            host[host.size() - c.size() - 1] == '.' &&
            host.compare(host.size() - c.size(), c.size(), c) == 0) {
            return true;  // 子域：<label>.api.minimax.cn
        }
    }
    return false;
}

// epoch 毫秒 → ISO8601 UTC。两处刻意差异：① 秒精度（上游 rfc3339 带小数秒，resetsAt 只服务倒计时）；② 超出 0000-9999 年 → 空串。
std::string millisToIso8601(int64_t ms) {
    constexpr int64_t kMinMs = -62135596800000LL;  // 0000-01-01T00:00:00.000Z
    constexpr int64_t kMaxMs = 253402300799999LL;  // 9999-12-31T23:59:59.999Z
    if (ms < kMinMs || ms > kMaxMs) return {};

    int64_t secs = ms / 1000;
    if (ms < 0 && ms % 1000 != 0) --secs;  // C++ 整除向零取整 → 负数补 1（地板除法）
    int64_t days = secs / 86400;
    int64_t rem = secs % 86400;
    if (rem < 0) {
        rem += 86400;
        --days;
    }
    const int64_t hh = rem / 3600;
    const int64_t mi = (rem % 3600) / 60;
    const int64_t ss = rem % 60;

    // civil_from_days（Howard Hinnant，与 core/time.h daysFromCivil 互逆）
    const int64_t z = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t y = yoe + era * 400;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int64_t dd = doy - (153 * mp + 2) / 5 + 1;
    const int64_t mo = mp < 10 ? mp + 3 : mp - 9;
    const int64_t yy = y + (mo <= 2 ? 1 : 0);

    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04lld-%02lld-%02lldT%02lld:%02lld:%02lldZ",
                  static_cast<long long>(yy), static_cast<long long>(mo),
                  static_cast<long long>(dd), static_cast<long long>(hh),
                  static_cast<long long>(mi), static_cast<long long>(ss));
    return std::string(buf);
}

// JSON 数值 → double。JsonValue 把 Int / Double 都归 T_Number 但内部只填一处（Int 分支 asDouble() 为 0），故按仓库惯用法回退。语义对应上游 as_f64()（整数/小数都收，字符串不收）。
bool jsonNumber(const platform::json::JsonValue& v, double& out) {
    if (!v.isNumber()) return false;
    const double d = v.asDouble();
    out = (d != 0.0) ? d : static_cast<double>(v.asInt());
    return true;
}

// JSON 数值 → int64（要求整数值，对应上游 as_i64()）。唯一放宽：写成 1.0 的整数值也认，带小数部一律不认（放宽方向不会把「未激活」误判成激活）。
bool jsonInteger(const platform::json::JsonValue& v, int64_t& out) {
    if (!v.isNumber()) return false;
    const double d = v.asDouble();
    if (d == 0.0) {
        out = v.asInt();
        return true;
    }
    if (d < -9.0e18 || d > 9.0e18) return false;
    if (static_cast<double>(static_cast<int64_t>(d)) != d) return false;
    out = static_cast<int64_t>(d);
    return true;
}

QuotaResult makeError(QuotaError error, std::string message) {
    QuotaResult r;
    r.error = error;
    r.errorMessage = std::move(message);
    return r;
}

// 响应体 → QuotaResult（纯函数，无网络，便于单测）。
QuotaResult parseRemainsBody(const platform::json::JsonValue& body) {
    if (!body.isObject()) {
        return makeError(QuotaError::Shape, "响应不是 JSON 对象");
    }
    const platform::json::JsonObject root = body.asObject();

    // ① 业务错误在 base_resp.{status_code,status_msg}——HTTP 200 也可能是失败。status_code 缺失/非整数按 -1 处理（同上游 unwrap_or(-1)），非 0 即失败。
    if (root.has("base_resp")) {
        const platform::json::JsonValue baseResp = root.get("base_resp");
        const platform::json::JsonObject baseObj =
            baseResp.isObject() ? baseResp.asObject() : platform::json::JsonObject();
        int64_t code = -1;  // 缺失/非整数默认 -1
        int64_t parsedCode = -1;
        if (jsonInteger(baseObj.get("status_code"), parsedCode)) code = parsedCode;
        if (code != 0) {
            std::string msg = "Unknown error";
            const platform::json::JsonValue statusMsg = baseObj.get("status_msg");
            if (statusMsg.isString() && !statusMsg.asString().empty()) {
                msg = statusMsg.asString();
            }
            // 上游不细分错误码，主因是套餐过期/额度用尽（改密钥没用）→ 归 Entitlement，不引导成网络重试。
            return makeError(QuotaError::Entitlement,
                             "MiniMax 接口业务错误（code " + std::to_string(code) +
                                 "）：" + msg +
                                 "——可能套餐已过期或额度已用尽，可到 MiniMax 控制台确认");
        }
    }

    const platform::json::JsonValue remains = root.get("model_remains");
    if (!remains.isArray()) {
        return makeError(QuotaError::Shape, "响应缺少 model_remains 数组");
    }
    bool foundGeneral = false;
    platform::json::JsonObject general;
    for (int i = 0; i < remains.size(); ++i) {
        const platform::json::JsonValue item = remains[i];
        if (!item.isObject()) continue;  // 上游对非对象元素静默跳过
        const platform::json::JsonObject obj = item.asObject();
        const platform::json::JsonValue name = obj.get("model_name");
        if (name.isString() && name.asString() == "general") {
            general = obj;
            foundGeneral = true;
            break;
        }
    }
    if (!foundGeneral) {
        return makeError(QuotaError::Shape, "model_remains 里没有 general（编程套餐）条目");
    }

    std::vector<QuotaWindow> windows;

    // ③ 五小时窗口：上游给剩余百分比，统一模型要已用 → 反转。key 用规范名 "five_hour"。
    double remain = 0.0;
    if (jsonNumber(general.get("current_interval_remaining_percent"), remain)) {
        QuotaWindow w;
        w.key = "five_hour";
        w.usedPercent = clampUsedPercent(100.0 - remain);  // 反转：本 provider 唯一的符号级错误源
        int64_t endMs = 0;
        if (jsonInteger(general.get("end_time"), endMs)) w.resetsAt = millisToIso8601(endMs);
        windows.push_back(std::move(w));
    }

    // ④ 周窗口：current_weekly_status == 1 才算激活（无周限额套餐该字段为 3、remaining 恒 100 → 会画出假周条）。
    int64_t weeklyStatus = 0;
    if (jsonInteger(general.get("current_weekly_status"), weeklyStatus) && weeklyStatus == 1) {
        if (jsonNumber(general.get("current_weekly_remaining_percent"), remain)) {
            QuotaWindow w;
            w.key = "weekly";
            w.usedPercent = clampUsedPercent(100.0 - remain);
            int64_t endMs = 0;
            if (jsonInteger(general.get("weekly_end_time"), endMs)) {
                w.resetsAt = millisToIso8601(endMs);
            }
            windows.push_back(std::move(w));
        }
    }

    // ⑤ 无月窗口（上游不提供月配额）。字段全缺 → Shape 而非「成功的空卡」，禁止把「没拿到数据」渲染成 0%/100% 假象。
    if (windows.empty()) {
        return makeError(QuotaError::Shape, "general 条目里没有可用的余量字段");
    }

    QuotaResult r;
    r.kind = QuotaKind::Tiered;
    r.windows = std::move(windows);
    return r;
}

class MinimaxCnProvider final : public QuotaProvider {
public:
    std::string id() const override { return "minimax_cn"; }
    std::string displayName() const override { return "MiniMax 中国站"; }
    // 空 fields = 沿用 profile 的 api key。
    std::vector<ProviderField> fields() const override { return {}; }

    bool matchesBaseUrl(const std::string& baseUrl) const override {
        return hostMatchesAny(baseUrl);
    }

    QuotaResult fetch(const ProviderConfig& cfg, const HttpSendFn& http) const override {
        if (cfg.apiKey.empty()) {
            // 配置不完整（缺字段）→ NotConfigured：不打网络，UI 引导去配置
            return makeError(QuotaError::NotConfigured,
                             "未配置 API 密钥：请在该 profile 里填入 MiniMax 的 API Key");
        }
        if (!http) {
            // 空缝必须返回 Transient，不许直接调用导致 std::bad_function_call。
            return makeError(QuotaError::Transient, "HTTP 发送器未注入（内部错误，可重试）");
        }

        platform::http::Request req;
        req.method = "GET";
        req.url = kRemainsUrl;
        req.headers["Authorization"] = "Bearer " + cfg.apiKey;
        req.headers["Content-Type"] = "application/json";
        req.timeoutMs = kTimeoutMs;

        const core::Result<platform::http::Response> resp = http(req);
        if (resp.isFail()) {
            return makeError(QuotaError::Transient, "网络错误：" + resp.errorMessage());
        }
        const platform::http::Response& response = resp.value();
        // 传输层失败的第二形态（Result::fail 之外）：ok(Response{statusCode=0, networkError=true})，不拦会显示不存在的「HTTP 0」。
        if (response.networkError) {
            return makeError(QuotaError::Transient,
                             "网络请求失败：" + response.errorMessage);
        }

        // 401 / 403 → Auth：上游把两者一起当「密钥无效」，不做 403 = 无权限的细分。
        if (response.statusCode == 401 || response.statusCode == 403) {
            return makeError(QuotaError::Auth, "API 密钥无效或已过期（HTTP " +
                                                   std::to_string(response.statusCode) + "）");
        }
        // 其它非 2xx（含 429/5xx）→ Transient：可重试，四个 provider 口径一致。
        if (response.statusCode < 200 || response.statusCode >= 300) {
            return makeError(QuotaError::Transient,
                             "接口返回 HTTP " + std::to_string(response.statusCode));
        }

        const core::Result<platform::json::JsonValue> parsed =
            platform::json::parse(response.body);
        if (parsed.isFail()) {
            // 拿到完整响应体后解析失败 = 确定性错误
            return makeError(QuotaError::Shape, "响应不是合法 JSON：" + parsed.errorMessage());
        }
        return parseRemainsBody(parsed.value());
    }
};

}  // namespace

QuotaProvider* createMinimaxCn() {
    static MinimaxCnProvider instance;  // 进程内单例（无状态，调用方只借不还）
    return &instance;
}

}  // namespace module::claude::quota
