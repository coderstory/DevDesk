// src/module_claude_proxy/quota/quota_volcano.cpp
// 火山方舟余量 provider（G4）—— 实现
// 逐字移植 cc-switch `src-tauri/src/services/coding_plan.rs`（rev bfaaba16）的火山分支
// （约 :835-1308）；行号对照表见 quota_volcano.h 头注释。
//
// 为什么不能复用「数据面 Bearer」那套：火山用量接口是**控制面 OpenAPI**（统一网关
// `open.volcengineapi.com`，不是推理域名 `ark.cn-beijing.volces.com`），强制火山引擎
// 签名 V4（AK/SK）。实测复用推理 Bearer Key 会被网关以 400 InvalidAuthorization 拒
// （格式层拒绝，非权限问题）——所以本 provider 的 fields() 收 AK/SK，不吃 profile 的
// apiKey（coding_plan.rs:837-847）。
//
// ⚠️ 诚实边界：本签名实现**从未在真实火山服务端验证过**（无火山账号 / 无 AK/SK）。
// 单测锁的是结构契约（canonical 串拼装 + 三处偏离标准 SigV4 的火山特性 + 密钥派生
// 链），期望值由**独立实现**（Python hmac/hashlib 按同一规格重算）给出，不是本文件
// 自己反算的；但"服务端会接受这个签名"这件事，只能等有账号时出网手测一次。
#include "quota_volcano.h"

#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "quota_percent.h"

#include "platform/json.h"

namespace module::claude::quota {
namespace volcano_detail {
namespace {

constexpr const char* kHexLower = "0123456789abcdef";

std::string toHex(const std::string& raw) {
    std::string out;
    out.reserve(raw.size() * 2);
    for (char ch : raw) {
        const unsigned char b = static_cast<unsigned char>(ch);
        out.push_back(kHexLower[b >> 4]);
        out.push_back(kHexLower[b & 0x0F]);
    }
    return out;
}

std::string sha256Hex(const std::string& data) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest);
    return toHex(std::string(reinterpret_cast<const char*>(digest), SHA256_DIGEST_LENGTH));
}

// HMAC-SHA256 的**原始字节**输出：密钥派生链的中间结果是二进制（不是十六进制串），
// 直接作为下一轮的 key 使用（coding_plan.rs:1016-1019）。
std::string hmacSha256Raw(const std::string& key, const std::string& data) {
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int outLen = 0;
    HMAC(EVP_sha256(), key.empty() ? "" : key.data(), static_cast<int>(key.size()),
         data.empty() ? reinterpret_cast<const unsigned char*>("")
                      : reinterpret_cast<const unsigned char*>(data.data()),
         data.size(), out, &outLen);
    return std::string(reinterpret_cast<const char*>(out), outLen);
}

std::string hmacSha256Hex(const std::string& key, const std::string& data) {
    return toHex(hmacSha256Raw(key, data));
}

// Howard Hinnant civil_from_days：公历日序号 → (y, m, d)。
// 用纯算术而不是 libc 的 gmtime：免平台分支（gmtime_r / gmtime_s 是两套 API）、
// 免 gmtime 的线程安全隐患。与 core::time::daysFromCivil 互为逆运算。
void civilFromDays(std::int64_t z, int& year, unsigned& month, unsigned& day) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);          // [0, 146096]
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  // [0, 399]
    const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);          // [0, 365]
    const unsigned mp = (5 * doy + 2) / 153;                               // [0, 11]
    day = doy - (153 * mp + 2) / 5 + 1;                                    // [1, 31]
    month = mp < 10 ? mp + 3 : mp - 9;                                     // [1, 12]
    year = static_cast<int>(y + (month <= 2 ? 1 : 0));
}

void utcPartsFromEpoch(std::int64_t seconds, int& year, unsigned& month, unsigned& day,
                       int& hour, int& minute, int& second) {
    std::int64_t days = seconds / 86400;
    std::int64_t rem = seconds % 86400;
    if (rem < 0) {  // 1970 之前的时间戳：C++ 的 / 与 % 向零截断，需手工回借
        rem += 86400;
        --days;
    }
    civilFromDays(days, year, month, day);
    hour = static_cast<int>(rem / 3600);
    minute = static_cast<int>((rem % 3600) / 60);
    second = static_cast<int>(rem % 60);
}

// `20240621T000000Z`（签名头的 X-Date，coding_plan.rs:997）
std::string xDateFromEpoch(std::time_t nowUtc) {
    int year = 0, hour = 0, minute = 0, second = 0;
    unsigned month = 0, day = 0;
    utcPartsFromEpoch(static_cast<std::int64_t>(nowUtc), year, month, day, hour, minute, second);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d%02u%02uT%02d%02u%02uZ", year, month, day, hour, minute,
                  second);
    return std::string(buf);
}

}  // namespace

std::string uriEncode(const std::string& input) {
    constexpr const char* kUpperHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(input.size());
    for (char ch : input) {
        const unsigned char b = static_cast<unsigned char>(ch);
        const bool unreserved = (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') ||
                                (b >= '0' && b <= '9') || b == '-' || b == '_' || b == '.' ||
                                b == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(b));
        } else {
            out.push_back('%');
            out.push_back(kUpperHex[b >> 4]);
            out.push_back(kUpperHex[b & 0x0F]);
        }
    }
    return out;
}

std::string canonicalQuery(const std::string& action, const std::string& region) {
    std::vector<std::pair<std::string, std::string>> pairs = {
        {"Action", action}, {"Region", region}, {"Version", kApiVersion}};
    // stable_sort：与 cc-switch 的 Rust `sort_by`（稳定排序）同语义。key 互不相同，
    // 稳定性其实不影响结果，但保持"同输入必同输出"这条不依赖实现细节。
    std::stable_sort(pairs.begin(), pairs.end(),
                     [](const std::pair<std::string, std::string>& a,
                        const std::pair<std::string, std::string>& b) {
                         return a.first < b.first;
                     });
    std::string out;
    for (const std::pair<std::string, std::string>& kv : pairs) {
        if (!out.empty()) out.push_back('&');
        out += uriEncode(kv.first);
        out.push_back('=');
        out += uriEncode(kv.second);
    }
    return out;
}

std::string regionFromBaseUrl(const std::string& baseUrl) {
    std::string host = baseUrl;
    const std::size_t scheme = host.find("://");
    if (scheme != std::string::npos) host = host.substr(scheme + 3);
    const std::size_t slash = host.find('/');
    if (slash != std::string::npos) host = host.substr(0, slash);

    // host 必须先小写化再切段比前缀：base_url 是用户手填 / 粘贴导入的自由文本，
    // 而同文件其余 URL helper（hostIsVolces / pathSegments）都比小写后的形式。
    // 不做这一步时，`Ark.CN-Shanghai.volces.com` 会「匹配得上本 provider
    // （matchesBaseUrl 小写化）却拿不到 region（这里不）」——回落成 cn-beijing，
    // 签名 scope 与账号不符被网关拒签，而错误文案又把方向指向「检查你的 AK/SK」，
    // 用户会去反复更换一把**本来正确**的密钥。形式与 isAuthErrorCode 同款
    //（lowerAscii 定义在本 TU 另一个匿名命名空间、位于本函数之后，故就地小写）。
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    std::size_t begin = 0;
    while (begin <= host.size()) {
        const std::size_t dot = host.find('.', begin);
        const std::string label =
            host.substr(begin, dot == std::string::npos ? std::string::npos : dot - begin);
        if (label.rfind("cn-", 0) == 0 || label.rfind("ap-", 0) == 0) return label;
        if (dot == std::string::npos) break;
        begin = dot + 1;
    }
    return kDefaultRegion;
}

bool isAuthErrorCode(const std::string& code) {
    std::string lower = code;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    static constexpr const char* kAuthMarkers[] = {
        "auth", "signature", "accessdenied", "denied", "unauthorized", "forbidden", "credential",
        "token"};
    for (const char* marker : kAuthMarkers) {
        if (lower.find(marker) != std::string::npos) return true;
    }
    return false;
}

std::string iso8601FromEpochSeconds(std::int64_t epochSeconds) {
    int year = 0, hour = 0, minute = 0, second = 0;
    unsigned month = 0, day = 0;
    utcPartsFromEpoch(epochSeconds, year, month, day, hour, minute, second);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02u-%02uT%02d:%02u:%02uZ", year, month, day, hour,
                  minute, second);
    return std::string(buf);
}

SignedHeaders signRequest(const std::string& accessKeyId, const std::string& secretAccessKey,
                          const std::string& region, const std::string& query,
                          const std::string& body, std::time_t nowUtc) {
    SignedHeaders out;
    out.xDate = xDateFromEpoch(nowUtc);
    const std::string shortDate = out.xDate.substr(0, 8);  // yyyymmdd
    out.xContentSha256 = sha256Hex(body);

    // ① canonical headers 用**固定顺序**，不排序（火山特有，coding_plan.rs:1001-1004）
    const std::string canonicalHeaders = std::string("host:") + kOpenApiHost +
                                         "\nx-date:" + out.xDate +
                                         "\nx-content-sha256:" + out.xContentSha256 +
                                         "\ncontent-type:" + kContentType + "\n";
    const std::string canonicalRequest = std::string("POST\n/\n") + query + "\n" +
                                         canonicalHeaders + "\n" + kSignedHeaders + "\n" +
                                         out.xContentSha256;

    // ③ credential scope 结尾是 `request`（不是 aws4_request）
    const std::string credentialScope =
        shortDate + "/" + region + "/" + std::string(kService) + "/request";
    // ② string_to_sign 的算法串是 `HMAC-SHA256`（**无** AWS4 前缀）
    const std::string stringToSign = std::string("HMAC-SHA256\n") + out.xDate + "\n" +
                                     credentialScope + "\n" + sha256Hex(canonicalRequest);

    // 签名密钥派生：kDate = HMAC(SK, date)（SK **不加** AWS4 前缀），终止串 `request`
    const std::string kDate = hmacSha256Raw(secretAccessKey, shortDate);
    const std::string kRegion = hmacSha256Raw(kDate, region);
    const std::string kServiceKey = hmacSha256Raw(kRegion, kService);
    const std::string kSigning = hmacSha256Raw(kServiceKey, "request");
    const std::string signature = hmacSha256Hex(kSigning, stringToSign);

    out.authorization = std::string("HMAC-SHA256 Credential=") + accessKeyId + "/" +
                        credentialScope + ", SignedHeaders=" + kSignedHeaders +
                        ", Signature=" + signature;
    return out;
}

}  // namespace volcano_detail

namespace {

using platform::json::JsonObject;
using platform::json::JsonValue;
namespace json = platform::json;

constexpr long kOpenApiTimeoutMs = 15000;  // coding_plan.rs:1058

// 单次 OpenAPI 调用的归类（照搬 coding_plan.rs:855-867 的 VolcCall 四态）
enum class CallKind {
    Body,       // 2xx + JSON 可解析 + 无 OpenAPI 错误信封（业务 Result 仍可能为空 = 未订阅）
    Auth,       // 硬鉴权失败（401/403 或 AccessDenied/Signature 类错误码）——两个 plan 共用
                // 凭据，命中即停
    Soft,       // 非鉴权 HTTP 错误 / 响应体非法 JSON——记录后可继续试另一个 plan
    Transient,  // 瞬时传输失败（网络 / 超时 / 读体中断）——立即传播
};

struct CallOutcome {
    CallKind kind = CallKind::Soft;
    JsonValue body;      // 仅 kind == Body 有效
    std::string detail;  // 其余三类：诊断文案
};

std::string trim(const std::string& s) {
    const std::size_t begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const std::size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

std::string lowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// JSON 数字取值。**必须按槽取**：platform/json.cpp:55-65 把整数（intVal）与实数
// （dblVal）分两个字段存，JsonValue::asDouble() 对 JSON 整数恒返回 0——只看
// asDouble() 会把 Quota/Used/Percent 全读成 0。字符串数字（"7.25"）按整串解析
// （cc-switch 的 parse_f64 对字符串走 Rust `str::parse`，尾部有残留即失败）。
bool jsonF64(const JsonValue& v, double& out) {
    if (v.type() == JsonValue::T_Number) {
        const double d = v.asDouble();
        out = (d != 0.0) ? d : static_cast<double>(v.asInt());
        return true;
    }
    if (v.type() == JsonValue::T_String) {
        const std::string s = v.asString();
        if (s.empty()) return false;
        const char* begin = s.c_str();
        char* end = nullptr;
        const double d = std::strtod(begin, &end);
        if (end == begin) return false;
        while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') ++end;
        if (*end != '\0') return false;
        out = d;
        return true;
    }
    return false;
}

bool jsonI64(const JsonValue& v, std::int64_t& out) {
    if (v.type() != JsonValue::T_Number) return false;
    const double d = v.asDouble();
    out = (d != 0.0) ? static_cast<std::int64_t>(d) : v.asInt();
    return true;
}

// 重置时间归一（coding_plan.rs:75-92 的 extract_reset_time）：
//   字符串 → 原样（上游给 ISO8601）
//   数字   → <= 0 视为「无重置」（火山 session 无活跃窗口回 -1）；
//             < 1e12 按秒、否则按毫秒，统一转 epoch 秒 → ISO8601（UTC）
// 无重置 / 未给 → 空串（QuotaWindow::resetsAt 的"无重置语义"约定）。
std::string resetTimeOf(const JsonValue& v) {
    if (v.type() == JsonValue::T_String) return v.asString();
    std::int64_t n = 0;
    if (!jsonI64(v, n)) return {};
    if (n <= 0) return {};
    if (n < 1000000000000LL) n *= 1000;  // 秒 → 毫秒
    return volcano_detail::iso8601FromEpochSeconds(n / 1000);
}

// 取第一个存在且为字符串的键（coding_plan.rs:1176-1183 的防御式 fallback 链）
std::string firstString(const JsonObject& obj, const std::vector<const char*>& keys) {
    for (const char* key : keys) {
        const JsonValue v = obj.get(key);
        if (v.isString()) return v.asString();
    }
    return {};
}

// `Result` 包裹层解包（coding_plan.rs:1256 的 `body.get("Result").unwrap_or(&body)`）
JsonValue unwrapResult(const JsonValue& body) {
    if (body.isObject()) {
        const JsonValue result = body.asObject().get("Result");
        if (!result.isNull()) return result;
    }
    return body;
}

// OpenAPI 错误信封抽取（coding_plan.rs:900-920）：
// ResponseMetadata.Error 优先，缺失时看顶层 Error；Code 与 Message 都空 = 无错误。
struct OpenApiError {
    std::string code;
    std::string message;
};

std::optional<OpenApiError> responseError(const JsonValue& body) {
    if (!body.isObject()) return std::nullopt;
    const JsonObject obj = body.asObject();
    JsonValue err;
    bool found = false;
    const JsonValue meta = obj.get("ResponseMetadata");
    if (meta.isObject()) {
        const JsonValue sub = meta.asObject().get("Error");
        if (!sub.isNull()) {
            err = sub;
            found = true;
        }
    }
    if (!found) {
        const JsonValue sub = obj.get("Error");
        if (!sub.isNull()) {
            err = sub;
            found = true;
        }
    }
    if (!found) return std::nullopt;

    OpenApiError out;
    const JsonObject errObj = err.isObject() ? err.asObject() : JsonObject{};
    const JsonValue code = errObj.get("Code");
    if (code.isString()) out.code = code.asString();
    const JsonValue message = errObj.get("Message");
    if (message.isString()) out.message = message.asString();
    if (out.code.empty() && out.message.empty()) return std::nullopt;
    return out;
}

// 面向用户的错误文案里塞上游原文时的统一截断（上限 700）。
// 上游 / 中间网关返一张几百 KB 的 HTML 错误页时，不截断会让错误行撑爆布局、
// 并把整页内容带进日志。这条是**代码**保证，不是「记得截断」的约定——本 TU 里
// 每一处把上游原文拼进 errorMessage 的地方都必须过它。
constexpr std::size_t kMaxErrorTextChars = 700;

std::string truncateForError(std::string text) {
    if (text.size() > kMaxErrorTextChars) {
        text.resize(kMaxErrorTextChars);
        text += "…（已截断）";
    }
    return text;
}

// 鉴权失败文案：必须同时点出「AK/SK」与「签名」两条排查方向
// （coding_plan.rs:922-924 的 VOLCENGINE_AKSK_HINT 是英文单条，这里按本仓库
//  "面向用户的中文文案"约定改写，信息量不减少）。
std::string authMessage(const std::string& where, const std::string& code,
                        const std::string& message) {
    std::string out = "火山方舟鉴权失败（" + where;
    if (!code.empty()) out += "，" + code;
    out += "）";
    if (!message.empty()) out += "：" + truncateForError(message);
    out += "。请检查账号的 AccessKey ID / Secret（AK/SK）是否正确、该账号是否有 Ark 用量查询"
           "（OpenAPI）权限；若 AK/SK 无误，则可能是请求签名未被网关接受。";
    return out;
}

std::string summarize(const char* action, const JsonValue& body) {
    const core::Result<std::string> dumped = json::stringify(body, false);
    std::string text = dumped.isOk() ? dumped.value() : std::string("<序列化失败>");
    if (text.size() > 700) text.resize(700);  // 截断，避免把整包塞进错误行
    return std::string(action) + "=" + text;
}

// 一次控制面 OpenAPI 调用（coding_plan.rs:1031-1113）
CallOutcome openApiCall(const std::string& region, const std::string& accessKeyId,
                        const std::string& secretAccessKey, const char* action,
                        const HttpSendFn& http) {
    // canonical query 同时用于签名与实际 URL——两者必须逐字一致，否则签名不匹配
    const std::string query = volcano_detail::canonicalQuery(action, region);
    const std::string url =
        std::string("https://") + volcano_detail::kOpenApiHost + "/?" + query;
    const volcano_detail::SignedHeaders signedHeaders = volcano_detail::signRequest(
        accessKeyId, secretAccessKey, region, query, "", std::time(nullptr));

    platform::http::Request req;
    req.method = "POST";
    req.url = url;
    req.headers["X-Date"] = signedHeaders.xDate;
    req.headers["X-Content-Sha256"] = signedHeaders.xContentSha256;
    req.headers["Content-Type"] = volcano_detail::kContentType;
    req.headers["Authorization"] = signedHeaders.authorization;
    req.body = "";
    req.timeoutMs = kOpenApiTimeoutMs;

    const core::Result<platform::http::Response> sent = http(req);
    if (sent.isFail()) {
        return {CallKind::Transient, {}, "网络错误：" + sent.errorMessage()};
    }
    const platform::http::Response resp = sent.value();
    if (resp.networkError) return {CallKind::Transient, {}, "网络错误：" + resp.errorMessage};

    const std::string status = std::to_string(resp.statusCode);
    if (resp.statusCode == 401 || resp.statusCode == 403) {
        return {CallKind::Auth, {}, authMessage("HTTP " + status, "", "")};
    }
    if (resp.statusCode < 200 || resp.statusCode >= 300) {
        // 网关对签名 / 凭据类错误常返 4xx（多为 HTTP 400）并携带与 200 路径相同的
        // ResponseMetadata.Error 信封，而非 401/403。这里也解析信封，让 AK/SK 被拒时
        // 仍能给出凭据引导（coding_plan.rs:1073-1088）。
        const core::Result<JsonValue> parsed = json::parse(resp.body);
        if (parsed.isOk()) {
            const std::optional<OpenApiError> err = responseError(parsed.value());
            if (err.has_value()) {
                if (volcano_detail::isAuthErrorCode(err->code)) {
                    return {CallKind::Auth, {},
                            authMessage("HTTP " + status, err->code, err->message)};
                }
                return {CallKind::Soft, {},
                        "API 错误（HTTP " + status + "，" + err->code + "）：" +
                            truncateForError(err->message)};
            }
        }
        return {CallKind::Soft, {},
                "API 错误（HTTP " + status + "）：" + truncateForError(resp.body)};
    }

    // 先整体解析再判错误信封——读体失败是瞬时，解析失败是确定性（两者分属不同通道）
    const core::Result<JsonValue> parsed = json::parse(resp.body);
    if (parsed.isFail()) {
        return {CallKind::Soft, {}, "响应体不是合法 JSON：" + parsed.errorMessage()};
    }
    const std::optional<OpenApiError> err = responseError(parsed.value());
    if (err.has_value()) {
        if (volcano_detail::isAuthErrorCode(err->code)) {
            return {CallKind::Auth, {}, authMessage(err->code, "", err->message)};
        }
        return {CallKind::Soft, {},
                "API 错误（" + err->code + "）：" + truncateForError(err->message)};
    }
    return {CallKind::Body, parsed.value(), ""};
}

// 窗口标签归一（coding_plan.rs:1151-1158）
std::string canonicalWindowKey(const std::string& label) {
    const std::string lower = lowerAscii(label);
    if (lower == "session" || lower == "5h" || lower == "fivehour" || lower == "five_hour" ||
        lower == "rolling_5h") {
        return "five_hour";
    }
    if (lower == "weekly" || lower == "week" || lower == "7d") return "weekly";
    if (lower == "monthly" || lower == "month") return "monthly";
    return {};
}

// GetAFPUsage → 窗口（coding_plan.rs:1122-1148）。
// 展示 5h / 周 / 月（与控制台一致）；AFPDaily 被官方控制台隐藏（其 Quota 常高于周上限，
// 属历史默认值而非强制限额）→ 跳过。Quota 是绝对额度，已用% = Used/Quota*100，
// 范围裁剪走 clampUsedPercent（quota_percent.h）——**不再**假手「下游渲染层负责」，
// G7 尚不存在，那句承诺当前无人兑现；Quota <= 0 视为该窗口未订阅 / 未启用 → 跳过，
// 这也让"已鉴权但无 Agent Plan"产出空结果，从而回落到 Coding Plan 探测。
void appendAfpWindows(const JsonValue& result, std::vector<QuotaWindow>& windows) {
    if (!result.isObject()) return;
    const JsonObject resultObj = result.asObject();
    struct Entry {
        const char* jsonKey;
        const char* canonicalKey;
    };
    static constexpr Entry kEntries[] = {
        {"AFPFiveHour", "five_hour"},
        {"AFPWeekly", "weekly"},
        {"AFPMonthly", "monthly"},
    };
    for (const Entry& entry : kEntries) {
        const JsonValue window = resultObj.get(entry.jsonKey);
        if (!window.isObject()) continue;
        const JsonObject windowObj = window.asObject();
        double quota = 0.0;
        if (!jsonF64(windowObj.get("Quota"), quota) || quota <= 0.0) continue;
        double used = 0.0;
        jsonF64(windowObj.get("Used"), used);  // 缺失按 0（cc-switch: unwrap_or(0.0)）
        QuotaWindow out;
        out.key = entry.canonicalKey;
        out.usedPercent = clampUsedPercent(used / quota * 100.0);
        out.resetsAt = resetTimeOf(windowObj.get("ResetTime"));
        windows.push_back(std::move(out));
    }
}

// GetCodingPlanUsage → 窗口（coding_plan.rs:1160-1207，防御式）。
// 该接口官方文档没给逐字段规格：回 session/weekly/monthly 三类窗口、**只给百分比**
// （已用）、重置时间是秒级。宽松匹配 QuotaUsage / Usages / Details 三个数组名与多种
// 字段名，命中即用、未命中跳过。**按显式 Level 字段映射周期，绝不按 ResetTimestamp
// 排序猜**（周期末尾周桶比 5h 桶更早重置，排序猜必错——quota_provider.h 明文约定）。
void appendCodingPlanWindows(const JsonValue& result, std::vector<QuotaWindow>& windows) {
    if (!result.isObject()) return;
    const JsonObject resultObj = result.asObject();
    JsonValue arr = resultObj.get("QuotaUsage");
    if (!arr.isArray()) arr = resultObj.get("Usages");
    if (!arr.isArray()) arr = resultObj.get("Details");
    if (!arr.isArray()) return;

    for (int i = 0; i < arr.size(); ++i) {
        const JsonValue item = arr[i];
        if (!item.isObject()) continue;
        const JsonObject itemObj = item.asObject();
        // 真实字段是 `Level`（实测 2026-06-21：session/weekly/monthly）；其余作防御式 fallback
        const std::string label =
            firstString(itemObj, {"Level", "Type", "Period", "Label", "Window"});
        const std::string key = canonicalWindowKey(label);
        if (key.empty()) continue;

        // 「读不到百分比」绝不能降级成 0.0。0.0 的语义是「已用 0%」= 还剩 100%：
        // 字段一改名 / 一漂形态，这条兜底就把「拿不到数据」画成「额度充足」，
        // 正是 quota_provider.h 约定 2 明令禁止的假象（"error != None 时不得被当作
        // 有效值呈现" 的反向也成立）。本函数自己就防御式兼容了三个数组名与五个
        // Level 字段名，说明形态漂移是预期内的——漂的若是 Percent 这一支，
        // 读不到就跳过该条目，让「本轮没解析出任何额度窗口」落到 queryVolcano
        // 已有的 emptyResponses → QuotaError::Shape 分支（那里带截断后的原始
        // 响应，便于核对真实字段名），而不是返回 error == None 的成功结果。
        double percent = 0.0;
        if (!jsonF64(itemObj.get("Percent"), percent) &&
            !jsonF64(itemObj.get("UsedPercent"), percent) &&
            !jsonF64(itemObj.get("UsagePercent"), percent)) {
            continue;
        }
        JsonValue resetValue = itemObj.get("ResetTime");
        if (resetValue.isNull()) resetValue = itemObj.get("ResetTimestamp");

        QuotaWindow out;
        out.key = key;
        out.usedPercent = clampUsedPercent(percent);
        out.resetsAt = resetTimeOf(resetValue);
        windows.push_back(std::move(out));
    }
}

QuotaResult errorResult(QuotaError error, std::string message) {
    QuotaResult r;
    r.error = error;
    r.errorMessage = std::move(message);
    return r;
}

std::string joinAll(const std::vector<std::string>& parts, const char* separator) {
    std::string out;
    for (const std::string& part : parts) {
        if (!out.empty()) out += separator;
        out += part;
    }
    return out;
}

// 双 plan 自动探测（coding_plan.rs:1235-1308）：先 Agent Plan（GetAFPUsage），
// 未订阅再 Coding Plan（GetCodingPlanUsage）。
QuotaResult queryVolcano(const std::string& region, const std::string& accessKeyId,
                         const std::string& secretAccessKey, const HttpSendFn& http) {
    std::vector<std::string> softErrors;
    std::vector<std::string> emptyResponses;

    // 1) Agent Plan
    const CallOutcome afp = openApiCall(region, accessKeyId, secretAccessKey,
                                        volcano_detail::kActionAfpUsage, http);
    switch (afp.kind) {
        case CallKind::Auth:
            return errorResult(QuotaError::Auth, afp.detail);
        case CallKind::Transient:
            return errorResult(QuotaError::Transient,
                               std::string(volcano_detail::kActionAfpUsage) + "：" + afp.detail);
        case CallKind::Soft:
            softErrors.push_back(std::string(volcano_detail::kActionAfpUsage) + "：" + afp.detail);
            break;
        case CallKind::Body: {
            const JsonValue result = unwrapResult(afp.body);
            std::vector<QuotaWindow> windows;
            appendAfpWindows(result, windows);
            if (!windows.empty()) {
                QuotaResult ok;
                ok.windows = std::move(windows);
                const JsonValue planType = result.isObject() ? result.asObject().get("PlanType")
                                                             : JsonValue{};
                const std::string trimmed =
                    planType.isString() ? trim(planType.asString()) : std::string();
                if (!trimmed.empty()) ok.planName = "Agent Plan " + trimmed;
                return ok;
            }
            emptyResponses.push_back(summarize(volcano_detail::kActionAfpUsage, afp.body));
            break;
        }
    }

    // 2) Coding Plan
    const CallOutcome coding = openApiCall(region, accessKeyId, secretAccessKey,
                                           volcano_detail::kActionCodingPlanUsage, http);
    switch (coding.kind) {
        case CallKind::Auth:
            return errorResult(QuotaError::Auth, coding.detail);
        case CallKind::Transient:
            return errorResult(
                QuotaError::Transient,
                std::string(volcano_detail::kActionCodingPlanUsage) + "：" + coding.detail);
        case CallKind::Soft:
            softErrors.push_back(std::string(volcano_detail::kActionCodingPlanUsage) + "：" +
                                 coding.detail);
            break;
        case CallKind::Body: {
            const JsonValue result = unwrapResult(coding.body);
            std::vector<QuotaWindow> windows;
            appendCodingPlanWindows(result, windows);
            if (!windows.empty()) {
                QuotaResult ok;
                ok.windows = std::move(windows);
                ok.planName = "Coding Plan";
                return ok;
            }
            emptyResponses.push_back(summarize(volcano_detail::kActionCodingPlanUsage, coding.body));
            break;
        }
    }

    // 3) 两个 plan 都没给出可用额度（coding_plan.rs:1293-1307）
    if (!softErrors.empty()) {
        return errorResult(QuotaError::Transient,
                           "火山方舟用量查询失败：" + joinAll(softErrors, "；"));
    }
    if (!emptyResponses.empty()) {
        // 签名已通过、请求到达业务层，但响应里没有可解析的额度。带上截断的原始响应，
        // 便于核对真实字段名 / 包裹层，或确认确实未订阅。
        return errorResult(QuotaError::Shape,
                           "火山方舟用量响应结构无法识别（签名已通过，但没解析出任何额度窗口）：" +
                               joinAll(emptyResponses, " || "));
    }
    return errorResult(QuotaError::Shape,
                       "火山方舟账号下没有找到可用的 Agent Plan / Coding Plan 订阅。");
}

// base_url 探测（coding_plan.rs:51-56 的命中规则，按 quota_provider.h 的"标签边界"
// 要求做成 host + 路径段判断，不是裸子串 contains——裸子串会让
// `https://volces.com.evil.com/api/plan` 误命中，等于把别人的额度查到本 profile 上）。
bool hostIsVolces(const std::string& host) {
    std::string h = host;
    const std::size_t port = h.find(':');
    if (port != std::string::npos) h = h.substr(0, port);
    while (!h.empty() && h.back() == '.') h.pop_back();  // 尾点 FQDN 写法
    h = lowerAscii(h);
    const std::string suffix = ".volces.com";
    if (h == "volces.com") return true;
    return h.size() > suffix.size() &&
           h.compare(h.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::vector<std::string> pathSegments(const std::string& url) {
    std::string rest = url;
    const std::size_t scheme = rest.find("://");
    if (scheme != std::string::npos) rest = rest.substr(scheme + 3);
    const std::size_t slash = rest.find('/');
    if (slash == std::string::npos) return {};
    rest = rest.substr(slash);
    const std::size_t cut = rest.find_first_of("?#");
    if (cut != std::string::npos) rest = rest.substr(0, cut);

    std::vector<std::string> segments;
    std::string current;
    for (char c : rest) {
        if (c == '/') {
            if (!current.empty()) {
                segments.push_back(lowerAscii(current));
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) segments.push_back(lowerAscii(current));
    return segments;
}

std::string hostOf(const std::string& url) {
    std::string rest = url;
    const std::size_t scheme = rest.find("://");
    if (scheme != std::string::npos) rest = rest.substr(scheme + 3);
    const std::size_t slash = rest.find('/');
    if (slash != std::string::npos) rest = rest.substr(0, slash);
    const std::size_t at = rest.rfind('@');  // 去 userinfo
    if (at != std::string::npos) rest = rest.substr(at + 1);
    return rest;
}

// 火山用量查询 provider。无状态 → 工厂返回进程内单例（见 quota_registry.h 契约）。
class VolcanoProvider final : public QuotaProvider {
public:
    std::string id() const override { return kVolcanoProviderId; }
    std::string displayName() const override { return "火山方舟"; }

    // 两项：AccessKey ID（非密文）+ Secret AccessKey（密文）。
    // 与其它供应商不同——火山用量走控制面 OpenAPI 签名，吃的是火山账号 AK/SK，
    // **不是** profile 里的推理 API Key（两套凭据，coding_plan.rs:841-844）。
    std::vector<ProviderField> fields() const override {
        return {
            {"accessKeyId", "AccessKey ID", false},
            {"secretAccessKey", "AccessKey Secret", true},
        };
    }

    bool matchesBaseUrl(const std::string& baseUrl) const override {
        if (baseUrl.empty()) return false;
        if (!hostIsVolces(hostOf(baseUrl))) return false;
        const std::vector<std::string> segments = pathSegments(baseUrl);
        if (segments.size() < 2 || segments[0] != "api") return false;
        // 只认 Agent Plan（/api/plan[/v3]）与 Coding Plan（/api/coding[/v3]）入口；
        // DouBaoSeed 按量付费走 /api/v3 与 /api/compatible，没有套餐额度 → 不命中。
        return segments[1] == "plan" || segments[1] == "coding";
    }

    QuotaResult fetch(const ProviderConfig& cfg, const HttpSendFn& http) const override {
        // 空缝必须返回错误而不是调用（调用即 std::bad_function_call，违反项目
        // "不抛 C++ 异常"铁律，见 quota_provider.h 的 HttpSendFn 约定）。
        if (!http) {
            return errorResult(QuotaError::Transient,
                               "火山方舟余量查询的 HTTP 通道不可用（内部错误，可重试）。");
        }
        const auto field = [&cfg](const char* key) {
            const auto it = cfg.fields.find(key);
            return it == cfg.fields.end() ? std::string() : trim(it->second);
        };
        const std::string accessKeyId = field("accessKeyId");
        const std::string secretAccessKey = field("secretAccessKey");
        if (accessKeyId.empty() || secretAccessKey.empty()) {
            return errorResult(
                QuotaError::NotConfigured,
                "火山方舟余量查询需要账号的 AccessKey ID 与 AccessKey Secret（AK/SK）——"
                "与推理 API Key 是两套凭据，请到火山控制台「访问控制」创建。");
        }
        return queryVolcano(volcano_detail::regionFromBaseUrl(cfg.baseUrl), accessKeyId,
                            secretAccessKey, http);
    }
};

}  // namespace

QuotaProvider* volcanoProvider() {
    // 进程内单例：provider 无状态，调用方只借不还（quota_registry.h 的工厂契约）
    static VolcanoProvider instance;
    return &instance;
}

}  // namespace module::claude::quota
