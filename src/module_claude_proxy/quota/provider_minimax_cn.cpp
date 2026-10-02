// src/module_claude_proxy/quota/provider_minimax_cn.cpp
// MiniMax 中国站余量 provider（G2）
// （实施计划 docs/superpowers/plans/2026-10-02-quota-implementation-plan.md §G2；
//   spec docs/superpowers/specs/2026-10-02-provider-quota-and-storage-design.md §3.3）
//
// 业务逻辑逐条移植自 cc-switch `src-tauri/src/services/coding_plan.rs`：
//   detect_provider:38-44        host 标签边界探测（api.minimaxi.com / api.minimax.cn）
//   query_minimax:426-509        旧域端点 + Bearer + base_resp 业务错误（HTTP 200 也可能是失败）
//   parse_minimax_tiers:657-712  general 桶 / 剩余百分比反转 / 周桶 status 门控
//   millis_to_iso8601:69-73      epoch 毫秒 → ISO8601
//   codex_config.rs:103-132      codex_url_host / codex_url_host_matches_any
//
// 刻意**不重新设计**的地方（照抄上游语义，包括看起来奇怪的那几条）：
//   - 端点写死旧域：profile 的 base_url 是 api.minimax.cn 也照样打 api.minimaxi.com
//     （上游注释 :429-435，commit e06ff90f：同一账号体系与 Key）
//   - base_resp **存在**但 status_code 缺失/非整数 → 上游 `unwrap_or(-1)` + `!= 0`
//     → 算业务错误，而不是「当成功继续解析」
//   - 百分比裁剪：上游记的是「渲染层负责显示策略」，但渲染层 G7 尚不存在——
//     那句约定在本仓库当前形态下无人兑现，故裁剪在本 provider 落地
//     （clampUsedPercent，见 quota_percent.h）。本 provider 上游给的是**剩余**%，
//     故先反转成「已用」再裁剪
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

// ── 上游常量（逐条对应 cc-switch）────────────────────────────────
// 额度接口只在 api.minimaxi.com / api.minimax.io 有公开出处；国内新推理域名
// api.minimax.cn 未见该接口文档，沿用旧域名（同一账号体系与 Key）。
// 依据：coding_plan.rs:429-436。这里**写死**，不读 cfg.baseUrl。
constexpr const char* kRemainsUrl =
    "https://api.minimaxi.com/v1/api/openplatform/coding_plan/remains";
constexpr long kTimeoutMs = 15000;  // 同上游 15s（coding_plan.rs:442）

// base_url 探测命中的 host（标签边界匹配，防 api.minimax.cn.example.com 伪造）。
// 依据：coding_plan.rs:38-44 —— codex_url_host_matches_any(base_url,
// ["api.minimaxi.com", "api.minimax.cn"])。国际站 api.minimax.io 是另一个 provider。
constexpr const char* kMatchHosts[] = {"api.minimaxi.com", "api.minimax.cn"};

// ── base_url → host（照 cc-switch codex_config.rs:103-116 codex_url_host）──
// 去 scheme / userinfo / port / path / query，去尾点，转小写；容忍用户手填的
// 松散形态（example.com / https://user@Example.com:8443/v1）。
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

// host **是**候选之一或候选的子域（标签边界），照 codex_url_host_matches_any:123-132。
// 不是裸子串包含：evilapi.minimax.cn / api.minimax.cn.example.com 都不命中。
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

// epoch 毫秒 → ISO8601 UTC "YYYY-MM-DDTHH:MM:SSZ"。
// 依据 cc-switch millis_to_iso8601（coding_plan.rs:69-73，chrono from_timestamp
// → to_rfc3339）。两处刻意差异：① 秒精度（上游 rfc3339 会带小数秒；resetsAt 只
// 服务倒计时展示）；② 超出 0000-9999 年（= 上游 from_timestamp 返回 None）→ 空串。
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

// JSON 数值 → double。JsonValue 把 JSON 整数（Int）与小数（Double）都归为
// T_Number，但内部只填一处（Int 分支只填 intVal → asDouble() 为 0；Double 分支
// 只填 dblVal → asInt() 为 0），故按仓库既有惯用法回退：src/module_todo/
// todo_view_state.cpp:94-98、src/module_pet/pet_config.cpp:92-96。
// 语义对应上游 `as_f64()`（整数 / 小数都收，字符串不收）——coding_plan.rs:675-678。
bool jsonNumber(const platform::json::JsonValue& v, double& out) {
    if (!v.isNumber()) return false;
    const double d = v.asDouble();
    out = (d != 0.0) ? d : static_cast<double>(v.asInt());
    return true;
}

// JSON 数值 → int64（要求整数值）。语义对应上游 `as_i64()`（coding_plan.rs:693：
// `current_weekly_status` 只认整数）。唯一放宽：JSON 写成 `1.0` 的整数值也认
//（上游 as_i64 会返 None），带小数部分的一律不认——放宽方向不会把「未激活」误判成激活。
bool jsonInteger(const platform::json::JsonValue& v, int64_t& out) {
    if (!v.isNumber()) return false;
    const double d = v.asDouble();
    if (d == 0.0) {  // Int 分支（asDouble() 恒 0）；Double 0.0 走同一分支也正确
        out = v.asInt();
        return true;
    }
    if (d < -9.0e18 || d > 9.0e18) return false;  // int64 安全区外
    if (static_cast<double>(static_cast<int64_t>(d)) != d) return false;  // 有小数部分
    out = static_cast<int64_t>(d);
    return true;
}

QuotaResult makeError(QuotaError error, std::string message) {
    QuotaResult r;
    r.error = error;
    r.errorMessage = std::move(message);
    return r;
}

// 响应体 → QuotaResult（纯函数，无网络，便于单测；上游同款拆分见
// `parse_minimax_tiers`，coding_plan.rs:657）。
QuotaResult parseRemainsBody(const platform::json::JsonValue& body) {
    if (!body.isObject()) {
        return makeError(QuotaError::Shape, "响应不是 JSON 对象");
    }
    const platform::json::JsonObject root = body.asObject();

    // ① 业务错误在 base_resp.{status_code,status_msg}——**HTTP 200 也可能是失败**
    //    （coding_plan.rs:481-494）。上游的 status_code 是 `unwrap_or(-1)` 后判
    //    `!= 0`：base_resp 存在但 status_code 缺失 / 非整数 → -1 → 同样算失败。
    if (root.has("base_resp")) {
        const platform::json::JsonValue baseResp = root.get("base_resp");
        const platform::json::JsonObject baseObj =
            baseResp.isObject() ? baseResp.asObject() : platform::json::JsonObject();
        int64_t code = -1;  // 上游 unwrap_or(-1)
        int64_t parsedCode = -1;
        if (jsonInteger(baseObj.get("status_code"), parsedCode)) code = parsedCode;
        if (code != 0) {
            std::string msg = "Unknown error";  // 上游同款兜底文案（:491）
            const platform::json::JsonValue statusMsg = baseObj.get("status_msg");
            if (statusMsg.isString() && !statusMsg.asString().empty()) {
                msg = statusMsg.asString();
            }
            // 错误码与上游文案原样透出：这是用户 / 日志定位问题的唯一线索。
            // 分类依据：上游对非零 status_code 只给「通用失败」（make_error），
            // **不细分**是鉴权 / 配额 / 限流，因此也**不编造**「码值 → 语义」表；
            // 但这一类的现实主因是「套餐过期 / 额度用尽」（改密钥没用、得续费），
            // 正是 QuotaError::Entitlement 的处置语义（quota_provider.h:44-46）。
            // 不判 Transient：那会引导用户当网络抖动去重试（G8 只挂「可能过期」角标）。
            return makeError(QuotaError::Entitlement,
                             "MiniMax 接口业务错误（code " + std::to_string(code) +
                                 "）：" + msg +
                                 "——可能套餐已过期或额度已用尽，可到 MiniMax 控制台确认");
        }
    }

    // ② model_remains[] 取 model_name == "general"（跳过 video 等非编程模型）
    const platform::json::JsonValue remains = root.get("model_remains");
    if (!remains.isArray()) {
        return makeError(QuotaError::Shape, "响应缺少 model_remains 数组");
    }
    bool foundGeneral = false;
    platform::json::JsonObject general;
    for (int i = 0; i < remains.size(); ++i) {
        const platform::json::JsonValue item = remains[i];
        if (!item.isObject()) continue;  // 上游对非对象元素静默跳过（get 返 None）
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

    // ③ 五小时窗口：上游给的是**剩余**百分比，统一模型要的是**已用** → 反转。
    //    窗口 key 用全项目规范名 "five_hour"（上游叫 current_interval_*）。
    double remain = 0.0;
    if (jsonNumber(general.get("current_interval_remaining_percent"), remain)) {
        QuotaWindow w;
        w.key = "five_hour";
        w.usedPercent = clampUsedPercent(100.0 - remain);  // **反转**：本 provider 唯一的符号级错误源
        int64_t endMs = 0;
        if (jsonInteger(general.get("end_time"), endMs)) w.resetsAt = millisToIso8601(endMs);
        windows.push_back(std::move(w));
    }

    // ④ 周窗口：current_weekly_status == 1 才算激活（上游 :692-693 注释：无周限额
    //    套餐该字段为 3、remaining_percent 恒 100 → 画出来就是「已用 0%」的假周条）。
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

    // ⑤ 无月窗口——上游不提供月配额，不画月条（spec §3.3）。
    //    字段全缺 → Shape 而不是「成功的空卡」：G0 契约 2 禁止把「没拿到数据」
    //    渲染成 0%/100% 的假象（G7 五态里「从未成功过」只渲染错误行）。
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
    // 空 fields = 沿用 profile 的 api key（G0 契约：fields() 为空即不做额外字段收集）
    std::vector<ProviderField> fields() const override { return {}; }

    // 探测：host 标签边界匹配两个官方 host（含子域），伪造后缀不命中
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
            // G0 契约（quota_provider.h:114-116）：空缝必须返回 Transient，
            // 不许直接调用导致 std::bad_function_call（项目铁律：不抛 C++ 异常）
            return makeError(QuotaError::Transient, "HTTP 发送器未注入（内部错误，可重试）");
        }

        platform::http::Request req;
        req.method = "GET";
        req.url = kRemainsUrl;  // 旧域写死，不读 cfg.baseUrl（见文件头依据）
        req.headers["Authorization"] = "Bearer " + cfg.apiKey;
        req.headers["Content-Type"] = "application/json";
        req.timeoutMs = kTimeoutMs;

        const core::Result<platform::http::Response> resp = http(req);
        if (resp.isFail()) {
            return makeError(QuotaError::Transient, "网络错误：" + resp.errorMessage());
        }
        const platform::http::Response& response = resp.value();
        // 与 G3 / G4 同款：传输层失败的第二种形态（ok(Response{statusCode=0,
        // networkError=true}）——超时 / 连接失败 / 读体中断。第一种形态
        // （Result::fail）已在上面拦下。不查这条的话 statusCode 0 会落进下面的
        // <200 分支，用户看到「接口返回 HTTP 0」——一个不存在的状态码，
        // 把排查方向指向上游 HTTP，而真因在网络层。
        if (response.networkError) {
            return makeError(QuotaError::Transient,
                             "网络请求失败：" + response.errorMessage);
        }

        // 401 / 403 → Auth：上游把两者一起当「密钥无效」（coding_plan.rs:452-463），
        // 不做 403 = 无权限的细分。
        if (response.statusCode == 401 || response.statusCode == 403) {
            return makeError(QuotaError::Auth, "API 密钥无效或已过期（HTTP " +
                                                   std::to_string(response.statusCode) + "）");
        }
        // 其它非 2xx（含 429/5xx）→ Transient：可重试口径，与计划 §G1 段
        // 「其它 4xx/5xx → Transient」保持四个 provider 一致。
        if (response.statusCode < 200 || response.statusCode >= 300) {
            return makeError(QuotaError::Transient,
                             "接口返回 HTTP " + std::to_string(response.statusCode));
        }

        const core::Result<platform::json::JsonValue> parsed =
            platform::json::parse(response.body);
        if (parsed.isFail()) {
            // 拿到完整响应体后解析失败 = 确定性错误（上游同款注释 :470-479）
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
