// tests/test_modules/test_claude_quota_volcano.cpp
// 火山方舟余量 provider（G4）契约测试
// （计划 docs/superpowers/plans/2026-10-02-quota-implementation-plan.md §G4）
//
// 被测实现逐字移植自 cc-switch `src-tauri/src/services/coding_plan.rs`（rev bfaaba16）
// 火山分支（:835-1308）。行号对照见 quota_volcano.h 头注释。
//
// ⚠️ 诚实边界（本文件里最重要的一句）：火山签名**没有服务端金标准向量**。
// 下面 signature_byte_exact_* 两条用例的期望值是用**独立实现**（Python hashlib/hmac
// 按同一算法规格重算，非本仓库代码产出）算出来的，它能锁住「canonical 串拼装 +
// 三处火山特有偏离 + 密钥派生链」的结构契约，能抓住 HMAC 用错、header 排序、
// scope 结尾、日期格式等实现错误；但**它证明不了服务端会接受这个签名**——
// 真实正确性必须等有火山账号时出网手测一次。
#include "doctest.h"

#include "platform/http_client.h"
#include "quota/quota_provider.h"
#include "quota/quota_registry.h"
#include "quota/quota_volcano.h"

#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace {

using module::claude::quota::HttpSendFn;
using module::claude::quota::ProviderConfig;
using module::claude::quota::ProviderField;
using module::claude::quota::QuotaError;
using module::claude::quota::QuotaKind;
using module::claude::quota::QuotaProvider;
using module::claude::quota::QuotaResult;
using module::claude::quota::QuotaWindow;
namespace volc = module::claude::quota::volcano_detail;

// ---- 假 HTTP 发送器：**零真实网络**（按调用序号回放脚本化响应）----
// 与 test_claude_quota.cpp 的注入缝范式一致：provider 只通过 HttpSendFn 发请求。
struct ScriptedHttp {
    std::vector<platform::http::Response> responses;  // 第 n 次调用回放第 n 条
    std::vector<platform::http::Request> requests;    // 记录收到的请求
    std::size_t served = 0;
    bool transportFail = false;  // 缝本身失败（Result::fail）
    bool networkErrorFlag = false;  // 缝成功但 Response.networkError=true

    HttpSendFn fn() {
        return [this](const platform::http::Request& req)
                   -> core::Result<platform::http::Response> {
            requests.push_back(req);
            if (transportFail) {
                return core::Result<platform::http::Response>::fail(
                    core::ErrorCode::NetworkUnreachable, "seam transport failure");
            }
            platform::http::Response resp;
            if (served < responses.size()) {
                resp = responses[served];
            } else {
                resp.statusCode = 200;
                resp.body = "{}";
            }
            ++served;
            if (networkErrorFlag) {
                resp.networkError = true;
                resp.errorMessage = "curl error 28";
            }
            return core::Result<platform::http::Response>::ok(std::move(resp));
        };
    }
};

platform::http::Response httpResp(int status, const std::string& body) {
    platform::http::Response r;
    r.statusCode = status;
    r.body = body;
    return r;
}

ProviderConfig volcanoConfig(
    const std::string& baseUrl = "https://ark.cn-beijing.volces.com/api/coding",
    const std::string& ak = "AKLTtest", const std::string& sk = "secretkey") {
    ProviderConfig cfg;
    cfg.providerId = "volcano";
    cfg.baseUrl = baseUrl;
    cfg.fields["accessKeyId"] = ak;
    cfg.fields["secretAccessKey"] = sk;
    return cfg;
}

const QuotaWindow* findWindow(const QuotaResult& r, const std::string& key) {
    for (const QuotaWindow& w : r.windows) {
        if (w.key == key) return &w;
    }
    return nullptr;
}

bool within(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// 官方文档 GetAFPUsage 返回示例（逐字取自 coding_plan.rs:2020-2026 的测试夹具）：
// 5h 25% / weekly 30% / monthly 42.525%，AFPDaily 控制台隐藏应跳过。
const char* kAfpOfficialBody = R"({
  "ResponseMetadata": { "RequestId": "req-afp" },
  "Result": {
    "PlanType": "Large",
    "AFPFiveHour": { "Quota": 50.0,   "Used": 12.5,  "SubscribeTime": 1778788800000, "ResetTime": 1778806800000 },
    "AFPDaily":    { "Quota": 100.0,  "Used": 22.5,  "SubscribeTime": 1778716800000, "ResetTime": 1778803200000 },
    "AFPWeekly":   { "Quota": 500.0,  "Used": 150.0, "SubscribeTime": 1778457600000, "ResetTime": 1779062400000 },
    "AFPMonthly":  { "Quota": 2000.0, "Used": 850.5, "SubscribeTime": 1777939200000, "ResetTime": 1780531200000 }
  }
})";

// 真实 GetCodingPlanUsage 响应（coding_plan.rs:2068-2076，用户 2026-06-21 实测）：
// 字段名是 `Level`，只有百分比，秒级 ResetTimestamp；session 无活跃窗口回 -1。
const char* kCodingPlanRealBody = R"({
  "ResponseMetadata": { "RequestId": "req-coding" },
  "Result": {
    "Status": "Running",
    "UpdateTimestamp": 1782053286,
    "QuotaUsage": [
      { "Level": "session", "Percent": 0.0,      "ResetTimestamp": -1 },
      { "Level": "weekly",  "Percent": 1.672568, "ResetTimestamp": 1782057600 },
      { "Level": "monthly", "Percent": 0.836284, "ResetTimestamp": 1784303999 }
    ]
  }
})";

}  // namespace

// ==================== id / displayName / fields / 注册表 ====================

REGISTER_TEST(claude_quota_volcano_identity_and_fields,
              "claude_quota: 火山 id/displayName 与 fields() 两项 AK/SK（secret 标记正确）", {
    const QuotaProvider* p = module::claude::quota::volcanoProvider();
    DOCTEST_CHECK(p != nullptr);
    DOCTEST_CHECK_EQ(p->id(), std::string("volcano"));
    DOCTEST_CHECK_EQ(p->displayName(), std::string("火山方舟"));

    // G6 按这两项动态渲染弹窗：accessKeyId 明文、secretAccessKey 密文。
    const std::vector<ProviderField> f = p->fields();
    DOCTEST_CHECK_EQ(f.size(), static_cast<std::size_t>(2));
    DOCTEST_CHECK_EQ(f[0].key, std::string("accessKeyId"));
    DOCTEST_CHECK_FALSE(f[0].secret);
    DOCTEST_CHECK_EQ(f[1].key, std::string("secretAccessKey"));
    DOCTEST_CHECK_TRUE(f[1].secret);
    // 标签非空（UI 直接显示，空标签等于用户看不懂填什么）
    DOCTEST_CHECK(!f[0].label.empty());
    DOCTEST_CHECK(!f[1].label.empty());
    return true;
})

REGISTER_TEST(claude_quota_volcano_registry_row_and_singleton,
              "claude_quota: 火山注册表行生效（findProvider + 单例 + detectByBaseUrl 命中）", {
    const QuotaProvider* byId = module::claude::quota::findProvider("volcano");
    DOCTEST_CHECK(byId != nullptr);
    DOCTEST_CHECK_EQ(byId->id(), std::string("volcano"));
    // 进程内单例：重复查找拿回同一对象（create 每次 new 就是稳定泄漏）
    DOCTEST_CHECK(byId == module::claude::quota::findProvider("volcano"));

    // base_url 自动选中：Agent Plan 与 Coding Plan 两档入口都命中本 provider
    const QuotaProvider* detected = module::claude::quota::detectByBaseUrl(
        "https://ark.cn-beijing.volces.com/api/coding");
    DOCTEST_CHECK(detected != nullptr);
    DOCTEST_CHECK_EQ(detected->id(), std::string("volcano"));
    return true;
})

// ==================== matchesBaseUrl（host + 路径段，非裸子串）====================

REGISTER_TEST(claude_quota_volcano_matches_base_url,
              "claude_quota: 火山 matchesBaseUrl 两档命中、/api/v3 与伪造域不命中", {
    const QuotaProvider* p = module::claude::quota::volcanoProvider();

    // 命中：Agent Plan（/api/plan[/v3]）与 Coding Plan（/api/coding[/v3]）
    DOCTEST_CHECK(p->matchesBaseUrl("https://ark.cn-beijing.volces.com/api/plan"));
    DOCTEST_CHECK(p->matchesBaseUrl("https://ark.cn-beijing.volces.com/api/plan/v3"));
    DOCTEST_CHECK(p->matchesBaseUrl("https://ark.cn-beijing.volces.com/api/coding"));
    DOCTEST_CHECK(p->matchesBaseUrl("https://ark.cn-beijing.volces.com/api/coding/v3"));
    // host 大小写不敏感
    DOCTEST_CHECK(p->matchesBaseUrl("https://ARK.CN-BEIJING.VOLCES.COM/api/coding"));

    // 不命中：DouBaoSeed 按量付费走 /api/v3 与 /api/compatible，没有套餐额度
    DOCTEST_CHECK_FALSE(p->matchesBaseUrl("https://ark.cn-beijing.volces.com/api/v3"));
    DOCTEST_CHECK_FALSE(p->matchesBaseUrl("https://ark.cn-beijing.volces.com/api/compatible"));
    // 不命中：host 标签边界伪造（裸子串匹配会误命中，等于把别人的额度查到本 profile）
    DOCTEST_CHECK_FALSE(p->matchesBaseUrl("https://volces.com.evil.com/api/plan"));
    DOCTEST_CHECK_FALSE(p->matchesBaseUrl("https://evil.com/volces.com/api/plan"));
    DOCTEST_CHECK_FALSE(p->matchesBaseUrl("https://notvolces.com/api/coding"));
    // 不命中：空串 + 别的供应商域
    DOCTEST_CHECK_FALSE(p->matchesBaseUrl(""));
    DOCTEST_CHECK_FALSE(p->matchesBaseUrl("https://api.deepseek.com/v1"));
    return true;
})

// ==================== region 提取 ====================

REGISTER_TEST(claude_quota_volcano_region_extraction,
              "claude_quota: 火山 region 从 host 段提取（两例）与缺省回落", {
    DOCTEST_CHECK_EQ(volc::regionFromBaseUrl("https://ark.cn-beijing.volces.com/api/coding"),
                     std::string("cn-beijing"));
    // 另一 region 的数据面域名按段提取（含 /v3 后缀）
    DOCTEST_CHECK_EQ(
        volc::regionFromBaseUrl("https://ark.cn-shanghai.volces.com/api/coding/v3"),
        std::string("cn-shanghai"));
    // ap-* 段同样命中
    DOCTEST_CHECK_EQ(volc::regionFromBaseUrl("https://ark.ap-southeast-1.volces.com/api/plan"),
                     std::string("ap-southeast-1"));
    // 无可识别 region 段 → 回落 cn-beijing（控制面 host 固定，不随 base_url 变）
    DOCTEST_CHECK_EQ(volc::regionFromBaseUrl("https://example.com/api/coding"),
                     std::string("cn-beijing"));
    DOCTEST_CHECK_EQ(volc::regionFromBaseUrl(""), std::string("cn-beijing"));
    return true;
})

REGISTER_TEST(claude_quota_volcano_region_extraction_is_case_insensitive,
              "claude_quota: 火山 region 对 host 大小写不敏感（混合大小写不回落成 cn-beijing）", {
    // 为什么这条单列：base_url 是用户手填 / 粘贴导入的自由文本。matchesBaseUrl
    // 走 hostIsVolces + pathSegments，两处都小写化 → 混合大小写**照样命中**本
    // provider；而 region 提取若不小写化，标签 `CN-Shanghai` 匹配不上小写前缀
    // `cn-` → 回落 cn-beijing → 签名 credential scope 与账号不符被网关拒签，
    // 而 authMessage 恰好把排查方向指向「检查你的 AK/SK」，用户会去反复更换
    // 一把**本来正确**的密钥。
    DOCTEST_CHECK_EQ(volc::regionFromBaseUrl("https://Ark.CN-Shanghai.volces.com/api/coding"),
                     std::string("cn-shanghai"));
    DOCTEST_CHECK_EQ(volc::regionFromBaseUrl("https://ARK.AP-Southeast-1.volces.com/api/plan"),
                     std::string("ap-southeast-1"));
    DOCTEST_CHECK_EQ(volc::regionFromBaseUrl("HTTPS://ark.CN-Beijing.volces.com/api/coding"),
                     std::string("cn-beijing"));
    // 钉死「大小写混合」与「全小写」给的是同一个 region（不是碰巧都回落成默认值）
    DOCTEST_CHECK_EQ(volc::regionFromBaseUrl("https://Ark.CN-Shanghai.volces.com/api/coding"),
                     volc::regionFromBaseUrl("https://ark.cn-shanghai.volces.com/api/coding"));
    return true;
})

// ==================== canonical query / uri encode ====================

REGISTER_TEST(claude_quota_volcano_canonical_query_sorted_and_encoded,
              "claude_quota: 火山 canonical query 按 key 字母序 + RFC3986 编码", {
    // Action < Region < Version；`-` 属 unreserved 不编码
    DOCTEST_CHECK_EQ(volc::canonicalQuery("GetAFPUsage", "cn-beijing"),
                     std::string("Action=GetAFPUsage&Region=cn-beijing&Version=2024-01-01"));
    DOCTEST_CHECK_EQ(volc::canonicalQuery("GetCodingPlanUsage", "cn-shanghai"),
                     std::string("Action=GetCodingPlanUsage&Region=cn-shanghai"
                                 "&Version=2024-01-01"));

    // unreserved 原样保留；空格 / `/` / 非 ASCII UTF-8 字节 → %XX **大写**
    DOCTEST_CHECK_EQ(volc::uriEncode("a b/c~d.e_f-g"),
                     std::string("a%20b%2Fc~d.e_f-g"));
    DOCTEST_CHECK_EQ(volc::uriEncode("中"), std::string("%E4%B8%AD"));
    DOCTEST_CHECK_EQ(volc::uriEncode("AZaz09-_.~"), std::string("AZaz09-_.~"));
    return true;
})

// ==================== 签名：逐字节结构契约 ====================
//
// ⚠️ 期望值来自**独立实现**（Python hashlib/hmac 按同一算法规格重算），不是本仓库
// 的 C++ 反算出来的。它锁死 canonical 串拼装 + 密钥派生链，但**不能**证明服务端
// 会接受（无服务端金标准向量）——真实正确性等有火山账号时出网手测。

REGISTER_TEST(claude_quota_volcano_signature_byte_exact_vector_a,
              "claude_quota: 火山签名逐字节（向量 A：GetAFPUsage/cn-beijing/2024-06-21）", {
    const std::time_t now = 1718928000;  // 2024-06-21T00:00:00Z
    const std::string query = volc::canonicalQuery("GetAFPUsage", "cn-beijing");
    const volc::SignedHeaders h =
        volc::signRequest("AKLTtest", "secretkey", "cn-beijing", query, "", now);

    DOCTEST_CHECK_EQ(h.xDate, std::string("20240621T000000Z"));
    // 空 body 的 SHA-256（固定值，证明签的是空 body 而不是某个默认 JSON）
    DOCTEST_CHECK_EQ(h.xContentSha256,
                     std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495"
                                 "991b7852b855"));
    // 逐字节金标准（独立实现算出）：
    DOCTEST_CHECK_EQ(
        h.authorization,
        std::string("HMAC-SHA256 Credential=AKLTtest/20240621/cn-beijing/ark/request, "
                    "SignedHeaders=host;x-date;x-content-sha256;content-type, "
                    "Signature=4ceb5e7c3c834fe8604cccf04eeec5ab09045d2e5a36723921f8cea691b0a186"));
    return true;
})

REGISTER_TEST(claude_quota_volcano_signature_byte_exact_vector_b,
              "claude_quota: 火山签名逐字节（向量 B：GetCodingPlanUsage/cn-shanghai/2026-10-02）", {
    const std::time_t now = 1790944496;  // 2026-10-02T12:34:56Z
    const std::string query = volc::canonicalQuery("GetCodingPlanUsage", "cn-shanghai");
    const volc::SignedHeaders h =
        volc::signRequest("AKLTtest", "secretkey", "cn-shanghai", query, "", now);

    DOCTEST_CHECK_EQ(h.xDate, std::string("20261002T123456Z"));
    // 换 region / 换 action / 换日期 → 签名必须整串不同（证明这些量真进了签名，
    // 而不是被常量吞掉——单向量测不出这一类错误）
    DOCTEST_CHECK_EQ(
        h.authorization,
        std::string("HMAC-SHA256 Credential=AKLTtest/20261002/cn-shanghai/ark/request, "
                    "SignedHeaders=host;x-date;x-content-sha256;content-type, "
                    "Signature=1b2c1164b2e302e057788121043a1360f8e7f1cd22e700f82ce39342428d51b1"));
    return true;
})

REGISTER_TEST(claude_quota_volcano_signature_three_volcengine_traits,
              "claude_quota: 火山签名三处偏离标准 SigV4（固定序 headers / 无 AWS4 / scope 结尾 request）", {
    const std::time_t now = 1718928000;
    const volc::SignedHeaders h = volc::signRequest(
        "AKLTtest", "secretkey", "cn-beijing",
        volc::canonicalQuery("GetAFPUsage", "cn-beijing"), "", now);

    // ① canonical headers 与 SignedHeaders 用**固定顺序**
    //    `host;x-date;x-content-sha256;content-type`（不按字母序）
    DOCTEST_CHECK_EQ(std::string(volc::kSignedHeaders),
                     std::string("host;x-date;x-content-sha256;content-type"));
    DOCTEST_CHECK_NE(std::string(volc::kSignedHeaders),
                     std::string("content-type;host;x-content-sha256;x-date"));  // 字母序 ≠ 火山序
    DOCTEST_CHECK(contains(h.authorization,
                           "SignedHeaders=host;x-date;x-content-sha256;content-type,"));

    // ② algorithm 串 `HMAC-SHA256`（**无** AWS4 前缀）
    DOCTEST_CHECK(h.authorization.rfind("HMAC-SHA256 Credential=", 0) == 0);
    DOCTEST_CHECK_FALSE(contains(h.authorization, "AWS4"));

    // ③ credential scope 结尾 `request`（非 aws4_request）
    DOCTEST_CHECK(contains(h.authorization, "/cn-beijing/ark/request,"));
    DOCTEST_CHECK_FALSE(contains(h.authorization, "aws4_request"));

    // 签名是 64 位小写十六进制（HMAC-SHA256 输出）
    const std::size_t sigPos = h.authorization.rfind("Signature=");
    DOCTEST_CHECK(sigPos != std::string::npos);
    const std::string sig = h.authorization.substr(sigPos + 10);
    DOCTEST_CHECK_EQ(sig.size(), static_cast<std::size_t>(64));
    for (char c : sig) {
        DOCTEST_CHECK((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }
    // 确定性：同输入同输出（无隐藏时钟 / 随机源）
    const volc::SignedHeaders again = volc::signRequest(
        "AKLTtest", "secretkey", "cn-beijing",
        volc::canonicalQuery("GetAFPUsage", "cn-beijing"), "", now);
    DOCTEST_CHECK_EQ(again.authorization, h.authorization);
    // 换 SK → 签名必须变（密钥真进了派生链）
    const volc::SignedHeaders otherSk = volc::signRequest(
        "AKLTtest", "secretkey2", "cn-beijing",
        volc::canonicalQuery("GetAFPUsage", "cn-beijing"), "", now);
    DOCTEST_CHECK_NE(otherSk.authorization, h.authorization);
    return true;
})

// ==================== 请求形态（控制面网关）====================

REGISTER_TEST(claude_quota_volcano_request_shape,
              "claude_quota: 火山请求形态（控制面网关 URL / 四个签名头 / POST 空 body）", {
    ScriptedHttp http;
    http.responses.push_back(httpResp(200, kAfpOfficialBody));
    const QuotaResult r = module::claude::quota::volcanoProvider()->fetch(
        volcanoConfig("https://ark.cn-beijing.volces.com/api/coding"), http.fn());

    DOCTEST_CHECK(r.error == QuotaError::None);
    DOCTEST_CHECK_EQ(http.requests.size(), static_cast<std::size_t>(1));
    const platform::http::Request& req = http.requests[0];
    DOCTEST_CHECK_EQ(req.method, std::string("POST"));
    // 控制面统一网关（**不是**数据面 `ark.cn-beijing.volces.com`）
    DOCTEST_CHECK_EQ(req.url,
                     std::string("https://open.volcengineapi.com/"
                                 "?Action=GetAFPUsage&Region=cn-beijing&Version=2024-01-01"));
    DOCTEST_CHECK_EQ(req.body, std::string(""));
    // 四个头都要在（少一个网关就拒签）
    DOCTEST_CHECK_EQ(req.headers.count("X-Date"), static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(req.headers.at("Content-Type"), std::string(volc::kContentType));
    DOCTEST_CHECK_EQ(req.headers.at("X-Content-Sha256"),
                     std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495"
                                 "991b7852b855"));
    const std::string auth = req.headers.count("Authorization") ? req.headers.at("Authorization")
                                                                : std::string();
    DOCTEST_CHECK(auth.rfind("HMAC-SHA256 Credential=AKLTtest/", 0) == 0);
    DOCTEST_CHECK(contains(auth, "SignedHeaders=host;x-date;x-content-sha256;content-type,"));
    // X-Date 形如 20261002T123456Z（长度固定 16）
    DOCTEST_CHECK_EQ(req.headers.at("X-Date").size(), static_cast<std::size_t>(16));
    return true;
})

// ==================== AFP 路径 ====================

REGISTER_TEST(claude_quota_volcano_afp_three_windows_and_daily_skipped,
              "claude_quota: 火山 AFP 三窗口（daily 跳过、Used/Quota 不截断、套餐名）", {
    ScriptedHttp http;
    http.responses.push_back(httpResp(200, kAfpOfficialBody));
    const QuotaResult r = module::claude::quota::volcanoProvider()->fetch(
        volcanoConfig("https://ark.cn-beijing.volces.com/api/plan"), http.fn());

    DOCTEST_CHECK(r.error == QuotaError::None);
    DOCTEST_CHECK(r.kind == QuotaKind::Tiered);
    // AFPDaily 被控制台隐藏 → 只剩 5h/周/月，顺序固定
    DOCTEST_CHECK_EQ(r.windows.size(), static_cast<std::size_t>(3));
    DOCTEST_CHECK_EQ(r.windows[0].key, std::string("five_hour"));
    DOCTEST_CHECK_EQ(r.windows[1].key, std::string("weekly"));
    DOCTEST_CHECK_EQ(r.windows[2].key, std::string("monthly"));
    DOCTEST_CHECK(within(r.windows[0].usedPercent, 25.0));      // 12.5/50*100
    DOCTEST_CHECK(within(r.windows[1].usedPercent, 30.0));      // 150/500*100
    DOCTEST_CHECK(within(r.windows[2].usedPercent, 42.525));    // 850.5/2000*100，double 不截断
    // 重置时间：毫秒 epoch → UTC ISO8601（Z 结尾）
    DOCTEST_CHECK_EQ(r.windows[0].resetsAt, std::string("2026-05-15T01:00:00Z"));
    DOCTEST_CHECK_EQ(r.windows[1].resetsAt, std::string("2026-05-18T00:00:00Z"));
    DOCTEST_CHECK_EQ(r.windows[2].resetsAt, std::string("2026-06-04T00:00:00Z"));
    // 套餐名（Agent Plan + PlanType）
    DOCTEST_CHECK_EQ(r.planName, std::string("Agent Plan Large"));
    // Tiered 型不许夹带余额字段
    DOCTEST_CHECK(within(r.balanceRemaining, 0.0));
    DOCTEST_CHECK(r.currency.empty());
    return true;
})

REGISTER_TEST(claude_quota_volcano_afp_zero_quota_falls_back_to_coding_plan,
              "claude_quota: 火山 AFP 全部 Quota<=0 视为未订阅 → 回落 Coding Plan", {
    // 已鉴权但无 Agent Plan：三个窗口 Quota=0 → 三窗口全跳过 → 探测第二个 plan
    ScriptedHttp http;
    http.responses.push_back(httpResp(200, R"({
      "Result": { "PlanType": "",
        "AFPFiveHour": { "Quota": 0.0, "Used": 0.0 },
        "AFPWeekly":   { "Quota": 0.0, "Used": 0.0 },
        "AFPMonthly":  { "Quota": 0.0, "Used": 0.0 } } })"));
    http.responses.push_back(httpResp(200, kCodingPlanRealBody));

    const QuotaResult r = module::claude::quota::volcanoProvider()->fetch(
        volcanoConfig("https://ark.cn-beijing.volces.com/api/coding"), http.fn());

    DOCTEST_CHECK(r.error == QuotaError::None);
    DOCTEST_CHECK_EQ(r.planName, std::string("Coding Plan"));
    // 探测顺序：先 GetAFPUsage，未命中再 GetCodingPlanUsage（URL 逐字核对）
    DOCTEST_CHECK_EQ(http.requests.size(), static_cast<std::size_t>(2));
    DOCTEST_CHECK_EQ(http.requests[0].url,
                     std::string("https://open.volcengineapi.com/"
                                 "?Action=GetAFPUsage&Region=cn-beijing&Version=2024-01-01"));
    DOCTEST_CHECK_EQ(http.requests[1].url,
                     std::string("https://open.volcengineapi.com/"
                                 "?Action=GetCodingPlanUsage&Region=cn-beijing&Version=2024-01-01"));
    // 两个请求用同一份 AK/SK 签名（同凭据）→ Authorization 只随时间变化
    DOCTEST_CHECK(http.requests[0].headers.count("Authorization") == 1);
    DOCTEST_CHECK(http.requests[1].headers.count("Authorization") == 1);
    return true;
})

REGISTER_TEST(claude_quota_volcano_afp_partial_windows_only_subscribed,
              "claude_quota: 火山 AFP 部分窗口有额度（缺周/月）只产出一个窗口", {
    ScriptedHttp http;
    http.responses.push_back(httpResp(200, R"({
      "Result": {
        "AFPFiveHour": { "Quota": 40.0, "Used": 10.0, "ResetTime": 1778806800000 },
        "AFPWeekly":   { "Quota": 0.0,  "Used": 0.0 } } })"));
    const QuotaResult r = module::claude::quota::volcanoProvider()->fetch(
        volcanoConfig("https://ark.cn-beijing.volces.com/api/plan"), http.fn());

    DOCTEST_CHECK(r.error == QuotaError::None);
    DOCTEST_CHECK_EQ(r.windows.size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(r.windows[0].key, std::string("five_hour"));
    DOCTEST_CHECK(within(r.windows[0].usedPercent, 25.0));
    // 只探测一次（AFP 命中即返回，不再打 Coding Plan）
    DOCTEST_CHECK_EQ(http.requests.size(), static_cast<std::size_t>(1));
    // PlanType 缺失 → 没有套餐名（不是空字符串硬造）
    DOCTEST_CHECK(r.planName.empty());
    return true;
})

// ==================== Coding Plan 路径 ====================

REGISTER_TEST(claude_quota_volcano_coding_plan_real_response_levels,
              "claude_quota: 火山 Coding Plan 真实响应（Level 归一 + 秒级重置 + -1 无重置）", {
    ScriptedHttp http;
    http.responses.push_back(httpResp(200, R"({"Result":{}})"));  // AFP 无额度
    http.responses.push_back(httpResp(200, kCodingPlanRealBody));
    const QuotaResult r = module::claude::quota::volcanoProvider()->fetch(
        volcanoConfig(), http.fn());

    DOCTEST_CHECK(r.error == QuotaError::None);
    DOCTEST_CHECK_EQ(r.planName, std::string("Coding Plan"));
    DOCTEST_CHECK_EQ(r.windows.size(), static_cast<std::size_t>(3));
    const QuotaWindow* w5 = findWindow(r, "five_hour");
    const QuotaWindow* wk = findWindow(r, "weekly");
    const QuotaWindow* mo = findWindow(r, "monthly");
    DOCTEST_CHECK(w5 != nullptr);
    DOCTEST_CHECK(wk != nullptr);
    DOCTEST_CHECK(mo != nullptr);
    DOCTEST_CHECK(within(w5->usedPercent, 0.0));
    DOCTEST_CHECK(within(wk->usedPercent, 1.672568));  // Percent 已用%，不截断
    DOCTEST_CHECK(within(mo->usedPercent, 0.836284));
    // session 的 ResetTimestamp=-1（无活跃窗口）→ 空串，不是 1970 也不是 "0"
    DOCTEST_CHECK(w5->resetsAt.empty());
    DOCTEST_CHECK_EQ(wk->resetsAt, std::string("2026-06-21T16:00:00Z"));  // 秒级时间戳
    DOCTEST_CHECK_EQ(mo->resetsAt, std::string("2026-07-17T15:59:59Z"));
    return true;
})

REGISTER_TEST(claude_quota_volcano_coding_plan_maps_by_level_not_reset_order,
              "claude_quota: 火山 Coding Plan 按 Level 映射（重置时间倒序也不串窗口）", {
    // 陷阱夹具：ResetTimestamp 与 Level 的周期快慢**反着来**（monthly 最早重置）。
    // 任何"按重置时间排序猜周期"的实现都会把 monthly 认成 five_hour——本用例专门
    // 钉死"按显式 Level 字段映射"。
    ScriptedHttp http;
    http.responses.push_back(httpResp(200, R"({"Result":{}})"));  // AFP 无额度
    http.responses.push_back(httpResp(200, R"({
      "Result": { "QuotaUsage": [
        { "Level": "monthly", "Percent": 11.0, "ResetTimestamp": 1000000000 },
        { "Level": "session", "Percent": 22.0, "ResetTimestamp": 1900000000 },
        { "Level": "weekly",  "Percent": 33.0, "ResetTimestamp": 1500000000 } ] } })"));

    const QuotaResult r = module::claude::quota::volcanoProvider()->fetch(
        volcanoConfig(), http.fn());
    DOCTEST_CHECK(r.error == QuotaError::None);
    DOCTEST_CHECK_EQ(r.windows.size(), static_cast<std::size_t>(3));

    const QuotaWindow* w5 = findWindow(r, "five_hour");
    const QuotaWindow* wk = findWindow(r, "weekly");
    const QuotaWindow* mo = findWindow(r, "monthly");
    DOCTEST_CHECK(w5 != nullptr);
    DOCTEST_CHECK(wk != nullptr);
    DOCTEST_CHECK(mo != nullptr);
    DOCTEST_CHECK(within(w5->usedPercent, 22.0));  // session → five_hour
    DOCTEST_CHECK(within(wk->usedPercent, 33.0));  // weekly → weekly
    DOCTEST_CHECK(within(mo->usedPercent, 11.0));  // monthly → monthly
    DOCTEST_CHECK_EQ(w5->resetsAt, std::string("2030-03-17T17:46:40Z"));
    DOCTEST_CHECK_EQ(wk->resetsAt, std::string("2017-07-14T02:40:00Z"));
    DOCTEST_CHECK_EQ(mo->resetsAt, std::string("2001-09-09T01:46:40Z"));
    // 数组序即展示序（移植 cc-switch：不重排上游给的顺序）
    DOCTEST_CHECK_EQ(r.windows[0].key, std::string("monthly"));
    return true;
})

REGISTER_TEST(claude_quota_volcano_coding_plan_fallback_arrays_and_unknown_level,
              "claude_quota: 火山 Coding Plan 兼容 Usages/Details 数组、未知 Level 跳过", {
    // 未知 Level（daily）跳过，只剩 weekly
    ScriptedHttp httpDaily;
    httpDaily.responses.push_back(httpResp(200, R"({"Result":{}})"));
    httpDaily.responses.push_back(httpResp(200, R"({
      "Result": { "QuotaUsage": [
        { "Level": "daily",  "Percent": 9.0 },
        { "Level": "weekly", "Percent": 20.0 } ] } })"));
    const QuotaResult rDaily = module::claude::quota::volcanoProvider()->fetch(
        volcanoConfig(), httpDaily.fn());
    DOCTEST_CHECK(rDaily.error == QuotaError::None);
    DOCTEST_CHECK_EQ(rDaily.windows.size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(rDaily.windows[0].key, std::string("weekly"));
    DOCTEST_CHECK(within(rDaily.windows[0].usedPercent, 20.0));

    // 备用数组名 Usages：字段名用 Type（Level 缺失时的防御式 fallback）
    ScriptedHttp httpUsages;
    httpUsages.responses.push_back(httpResp(200, R"({"Result":{}})"));
    httpUsages.responses.push_back(httpResp(200, R"({
      "Result": { "Usages": [ { "Type": "5h", "Percent": 5.5 } ] } })"));
    const QuotaResult rUsages = module::claude::quota::volcanoProvider()->fetch(
        volcanoConfig(), httpUsages.fn());
    DOCTEST_CHECK(rUsages.error == QuotaError::None);
    DOCTEST_CHECK_EQ(rUsages.windows.size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(rUsages.windows[0].key, std::string("five_hour"));
    DOCTEST_CHECK(within(rUsages.windows[0].usedPercent, 5.5));

    // 备用数组名 Details：字符串形态的百分比（parse_f64 兼容 "100" 这种字符串）
    ScriptedHttp httpDetails;
    httpDetails.responses.push_back(httpResp(200, R"({"Result":{}})"));
    httpDetails.responses.push_back(httpResp(200, R"({
      "Result": { "Details": [ { "Period": "monthly", "Percent": "7.25" } ] } })"));
    const QuotaResult rDetails = module::claude::quota::volcanoProvider()->fetch(
        volcanoConfig(), httpDetails.fn());
    DOCTEST_CHECK(rDetails.error == QuotaError::None);
    DOCTEST_CHECK_EQ(rDetails.windows.size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(rDetails.windows[0].key, std::string("monthly"));
    DOCTEST_CHECK(within(rDetails.windows[0].usedPercent, 7.25));
    return true;
})

REGISTER_TEST(claude_quota_volcano_coding_plan_level_without_percent_is_shape_not_zero,
              "claude_quota: 火山 Coding Plan 有 Level 无 Percent → Shape（绝不画成「已用 0%」）", {
    // 契约（quota_provider.h 约定 2）的反向：没拿到值时**不得**画成有效值。
    // 0.0 的语义是「已用 0%」= 还剩 100%，把「字段漂移」伪装成「额度充足」。
    // 本夹具：Level 可识别（所以窗口周期认得出来），但三个百分比字段一个都没有
    // ——正是上游给 Coding Plan 改字段名时会落进的形态。
    //
    // 其余三家 provider 在同类输入下都报 Shape（opencode_go / minimax_cn /
    // deepseek），这里原先是**唯一**返回 error==None 成功结果的一条。
    ScriptedHttp http;
    http.responses.push_back(httpResp(200, R"({"Result":{}})"));  // AFP 无额度
    http.responses.push_back(httpResp(200, R"({
      "Result": { "QuotaUsage": [
        { "Level": "session", "ResetTimestamp": 1782053286 },
        { "Level": "weekly",  "ResetTimestamp": 1782057600 },
        { "Level": "monthly", "ResetTimestamp": 1784303999 } ] } })"));

    const QuotaResult r = module::claude::quota::volcanoProvider()->fetch(
        volcanoConfig(), http.fn());

    // 必须是「形态不认识」的失败，不是一条成功的空卡
    DOCTEST_CHECK(r.error == QuotaError::Shape);
    // 失败结果里绝不许夹带半截成功值（画出来就是三条满格 = 还剩 100% 的假象）
    DOCTEST_CHECK(r.windows.empty());
    DOCTEST_CHECK(r.planName.empty());
    // 报错带截断后的原始响应，便于核对真实字段名
    DOCTEST_CHECK(contains(r.errorMessage, "QuotaUsage"));
    return true;
})

REGISTER_TEST(claude_quota_volcano_coding_plan_partial_percent_keeps_parsed_windows,
              "claude_quota: 火山 Coding Plan 部分条目缺 Percent → 只丢那一条，不伪造其余", {
    // 反向对照：缺百分比的**只有一条**时，其余条目必须照常解析（不是一有缺失就
    // 整个丢弃，也不是把缺的那条补成 0）。
    ScriptedHttp http;
    http.responses.push_back(httpResp(200, R"({"Result":{}})"));
    http.responses.push_back(httpResp(200, R"({
      "Result": { "QuotaUsage": [
        { "Level": "session", "ResetTimestamp": 1782053286 },
        { "Level": "weekly",  "Percent": 42.5, "ResetTimestamp": 1782057600 } ] } })"));

    const QuotaResult r = module::claude::quota::volcanoProvider()->fetch(
        volcanoConfig(), http.fn());
    DOCTEST_CHECK(r.error == QuotaError::None);
    DOCTEST_CHECK_EQ(r.windows.size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(r.windows[0].key, std::string("weekly"));
    DOCTEST_CHECK(within(r.windows[0].usedPercent, 42.5));
    return true;
})

REGISTER_TEST(claude_quota_volcano_coding_plan_percent_is_clamped_to_range,
              "claude_quota: 火山 Coding Plan 百分比裁剪到 [0,100]（上游给 150 不外流）", {
    // 上游给 >100（超额 / 量纲漂移）时不得原样画出去。裁剪由代码保证
    //（clampUsedPercent），不是「渲染层负责」——渲染层 G7 尚不存在。
    ScriptedHttp http;
    http.responses.push_back(httpResp(200, R"({"Result":{}})"));
    http.responses.push_back(httpResp(200, R"({
      "Result": { "QuotaUsage": [
        { "Level": "weekly",  "Percent": 150.0 },
        { "Level": "monthly", "Percent": -7.5 } ] } })"));

    const QuotaResult r = module::claude::quota::volcanoProvider()->fetch(
        volcanoConfig(), http.fn());
    DOCTEST_CHECK(r.error == QuotaError::None);
    const QuotaWindow* wk = findWindow(r, "weekly");
    const QuotaWindow* mo = findWindow(r, "monthly");
    DOCTEST_REQUIRE(wk != nullptr);
    DOCTEST_REQUIRE(mo != nullptr);
    DOCTEST_CHECK(within(wk->usedPercent, 100.0));
    DOCTEST_CHECK(within(mo->usedPercent, 0.0));
    return true;
})

// ==================== 错误分类 ====================

REGISTER_TEST(claude_quota_volcano_auth_http_401_maps_to_auth,
              "claude_quota: 火山 HTTP 401 → Auth，且不发第二次请求（同凭据硬停）", {
    ScriptedHttp http;
    http.responses.push_back(httpResp(401, R"({"ResponseMetadata":{"Error":{"Code":"Unauthorized"}}})"));
    const QuotaResult r = module::claude::quota::volcanoProvider()->fetch(
        volcanoConfig(), http.fn());

    DOCTEST_CHECK(r.error == QuotaError::Auth);
    DOCTEST_CHECK(contains(r.errorMessage, "AK/SK"));  // 文案指向凭据
    // 两个 plan 共用一份 AK/SK → 鉴权失败立即停，不试第二个 plan
    DOCTEST_CHECK_EQ(http.requests.size(), static_cast<std::size_t>(1));
    // 失败结果不许夹带半截成功值
    DOCTEST_CHECK(r.windows.empty());
    return true;
})

REGISTER_TEST(claude_quota_volcano_auth_error_code_in_200_body_maps_to_auth,
              "claude_quota: 火山 200 + SignatureDoesNotMatch → Auth（文案指向 AK/SK 或签名）", {
    // 火山 OpenAPI 业务错误常以 200 + ResponseMetadata.Error 返回
    ScriptedHttp http;
    http.responses.push_back(httpResp(200, R"({
      "ResponseMetadata": { "RequestId": "x",
        "Error": { "Code": "SignatureDoesNotMatch", "Message": "signature mismatch" } } })"));
    const QuotaResult r = module::claude::quota::volcanoProvider()->fetch(
        volcanoConfig(), http.fn());

    DOCTEST_CHECK(r.error == QuotaError::Auth);
    DOCTEST_CHECK(contains(r.errorMessage, "AK/SK"));
    DOCTEST_CHECK(contains(r.errorMessage, "签名"));
    DOCTEST_CHECK(contains(r.errorMessage, "SignatureDoesNotMatch"));  // 带原始错误码便于排查
    DOCTEST_CHECK_EQ(http.requests.size(), static_cast<std::size_t>(1));
    return true;
})

REGISTER_TEST(claude_quota_volcano_http_400_auth_code_maps_to_auth,
              "claude_quota: 火山 400(非 401/403) + AccessDenied 错误码同样归 Auth", {
    // 网关对签名/凭据类错误常返 4xx（多为 400）而不是 401/403 —— 必须解析错误信封
    ScriptedHttp http;
    http.responses.push_back(httpResp(400, R"({
      "ResponseMetadata": { "Error": { "Code": "AccessDenied", "Message": "no permission" } } })"));
    const QuotaResult r = module::claude::quota::volcanoProvider()->fetch(
        volcanoConfig(), http.fn());

    DOCTEST_CHECK(r.error == QuotaError::Auth);
    DOCTEST_CHECK(contains(r.errorMessage, "AccessDenied"));
    return true;
})

REGISTER_TEST(claude_quota_volcano_error_code_classifier,
              "claude_quota: 火山鉴权错误码子串表（auth/signature/denied/token/credential...）", {
    DOCTEST_CHECK(volc::isAuthErrorCode("AccessDenied"));
    DOCTEST_CHECK(volc::isAuthErrorCode("SignatureDoesNotMatch"));
    DOCTEST_CHECK(volc::isAuthErrorCode("InvalidAuthorization"));
    DOCTEST_CHECK(volc::isAuthErrorCode("Unauthorized"));
    DOCTEST_CHECK(volc::isAuthErrorCode("Forbidden"));
    DOCTEST_CHECK(volc::isAuthErrorCode("InvalidCredential"));
    DOCTEST_CHECK(volc::isAuthErrorCode("InvalidToken"));
    DOCTEST_CHECK(volc::isAuthErrorCode("accessdenied"));  // 大小写不敏感
    // 非鉴权错误码不许误判（否则把普通 API 错误报成"换密钥"）
    DOCTEST_CHECK_FALSE(volc::isAuthErrorCode("InvalidParameter.Action"));
    DOCTEST_CHECK_FALSE(volc::isAuthErrorCode("InternalError"));
    DOCTEST_CHECK_FALSE(volc::isAuthErrorCode("Throttling"));
    DOCTEST_CHECK_FALSE(volc::isAuthErrorCode(""));
    return true;
})

REGISTER_TEST(claude_quota_volcano_soft_errors_map_to_transient,
              "claude_quota: 火山非鉴权错误（4xx/5xx/非法 JSON）→ Transient", {
    // 非鉴权 4xx + 错误信封
    ScriptedHttp http4xx;
    http4xx.responses.push_back(httpResp(400, R"({
      "ResponseMetadata": { "Error": { "Code": "InvalidParameter.Action", "Message": "bad action" } } })"));
    http4xx.responses.push_back(httpResp(400, R"({
      "ResponseMetadata": { "Error": { "Code": "InvalidParameter.Action", "Message": "bad action" } } })"));
    const QuotaResult r4xx = module::claude::quota::volcanoProvider()->fetch(
        volcanoConfig(), http4xx.fn());
    DOCTEST_CHECK(r4xx.error == QuotaError::Transient);
    DOCTEST_CHECK(contains(r4xx.errorMessage, "InvalidParameter.Action"));
    // 非鉴权软失败会继续试第二个 plan（同 cc-switch）
    DOCTEST_CHECK_EQ(http4xx.requests.size(), static_cast<std::size_t>(2));

    // 5xx + 非 JSON 体
    ScriptedHttp http5xx;
    http5xx.responses.push_back(httpResp(500, "<html>gateway error</html>"));
    http5xx.responses.push_back(httpResp(500, "<html>gateway error</html>"));
    const QuotaResult r5xx = module::claude::quota::volcanoProvider()->fetch(
        volcanoConfig(), http5xx.fn());
    DOCTEST_CHECK(r5xx.error == QuotaError::Transient);

    // 200 但响应体不是 JSON（确定性失败，不是瞬时网络问题，但仍归软失败通道）
    ScriptedHttp httpBadJson;
    httpBadJson.responses.push_back(httpResp(200, "not-json"));
    httpBadJson.responses.push_back(httpResp(200, "not-json"));
    const QuotaResult rBadJson = module::claude::quota::volcanoProvider()->fetch(
        volcanoConfig(), httpBadJson.fn());
    DOCTEST_CHECK(rBadJson.error == QuotaError::Transient);
    return true;
})

REGISTER_TEST(claude_quota_volcano_transport_failures_map_to_transient,
              "claude_quota: 火山传输失败（缝 Result::fail / networkError）→ Transient", {
    // 缝本身失败（DNS / 连不上）
    ScriptedHttp httpFail;
    httpFail.transportFail = true;
    const QuotaResult rFail = module::claude::quota::volcanoProvider()->fetch(
        volcanoConfig(), httpFail.fn());
    DOCTEST_CHECK(rFail.error == QuotaError::Transient);
    DOCTEST_CHECK_EQ(httpFail.requests.size(), static_cast<std::size_t>(1));  // 不试第二个 plan

    // 缝成功但 Response 标了 networkError（超时 / 读体中断）
    ScriptedHttp httpNetErr;
    httpNetErr.networkErrorFlag = true;
    httpNetErr.responses.push_back(httpResp(0, ""));
    const QuotaResult rNetErr = module::claude::quota::volcanoProvider()->fetch(
        volcanoConfig(), httpNetErr.fn());
    DOCTEST_CHECK(rNetErr.error == QuotaError::Transient);
    DOCTEST_CHECK_EQ(httpNetErr.requests.size(), static_cast<std::size_t>(1));
    return true;
})

REGISTER_TEST(claude_quota_volcano_unknown_shape_maps_to_shape,
              "claude_quota: 火山两 plan 均无可解析额度 → Shape（报错，不是空卡）", {
    ScriptedHttp http;
    http.responses.push_back(httpResp(200, R"({"Result":{"PlanType":""}})"));   // 无 AFP 窗口
    http.responses.push_back(httpResp(200, R"({"Result":{"Status":"Running"}})"));  // 无 QuotaUsage
    const QuotaResult r = module::claude::quota::volcanoProvider()->fetch(
        volcanoConfig(), http.fn());

    DOCTEST_CHECK(r.error == QuotaError::Shape);
    // 错误文案要带诊断信息（区分"真没订阅"与"字段名猜错"）
    DOCTEST_CHECK(contains(r.errorMessage, "GetAFPUsage"));
    DOCTEST_CHECK(contains(r.errorMessage, "GetCodingPlanUsage"));
    // 结构不认识 ≠ 成功：绝不返回 0 窗口的"成功"（G7 会画成没有额度）
    DOCTEST_CHECK(r.windows.empty());
    DOCTEST_CHECK_EQ(http.requests.size(), static_cast<std::size_t>(2));
    return true;
})

REGISTER_TEST(claude_quota_volcano_missing_credentials_not_configured,
              "claude_quota: 火山缺 AK/SK → NotConfigured，且一个请求都不发", {
    // fetch 非 const（G0 契约），故此处取非 const 指针
    QuotaProvider* p = module::claude::quota::volcanoProvider();

    ScriptedHttp http;
    http.responses.push_back(httpResp(200, kAfpOfficialBody));

    // 两项都缺
    ProviderConfig none;
    none.providerId = "volcano";
    none.baseUrl = "https://ark.cn-beijing.volces.com/api/coding";
    const QuotaResult rNone = p->fetch(none, http.fn());
    DOCTEST_CHECK(rNone.error == QuotaError::NotConfigured);
    DOCTEST_CHECK(contains(rNone.errorMessage, "AccessKey"));

    // 只缺 SK（空白串也算缺）
    ProviderConfig noSk = volcanoConfig();
    noSk.fields["secretAccessKey"] = "   ";
    const QuotaResult rNoSk = p->fetch(noSk, http.fn());
    DOCTEST_CHECK(rNoSk.error == QuotaError::NotConfigured);

    // 只缺 AK
    ProviderConfig noAk = volcanoConfig();
    noAk.fields["accessKeyId"] = "";
    const QuotaResult rNoAk = p->fetch(noAk, http.fn());
    DOCTEST_CHECK(rNoAk.error == QuotaError::NotConfigured);

    // 凭据不全时**不发请求**（省一次必然失败的出网 + 不把空串签进 Authorization）
    DOCTEST_CHECK_EQ(http.requests.size(), static_cast<std::size_t>(0));
    return true;
})

REGISTER_TEST(claude_quota_volcano_empty_http_seam_returns_transient,
              "claude_quota: 火山空 HttpSendFn → Transient（不抛 bad_function_call）", {
    const QuotaResult r = module::claude::quota::volcanoProvider()->fetch(
        volcanoConfig(), module::claude::quota::HttpSendFn{});
    DOCTEST_CHECK(r.error == QuotaError::Transient);
    DOCTEST_CHECK(!r.errorMessage.empty());
    DOCTEST_CHECK(r.windows.empty());
    return true;
})

// ==================== 重置时间形态 ====================

REGISTER_TEST(claude_quota_volcano_reset_time_forms,
              "claude_quota: 火山重置时间（毫秒/秒/字符串/0 与负 → 空）", {
    // AFP 的 ResetTime 是毫秒；Coding Plan 的 ResetTimestamp 是秒（同一解析器按
    // 1e12 阈值区分），字符串按 ISO8601 原样透传，0/负 = 无重置
    ScriptedHttp http;
    http.responses.push_back(httpResp(200, R"({
      "Result": {
        "AFPFiveHour": { "Quota": 10.0, "Used": 1.0, "ResetTime": 1778806800123 },
        "AFPWeekly":   { "Quota": 10.0, "Used": 1.0, "ResetTime": "2026-06-21T16:00:00Z" },
        "AFPMonthly":  { "Quota": 10.0, "Used": 1.0, "ResetTime": 0 } } })"));
    const QuotaResult r = module::claude::quota::volcanoProvider()->fetch(
        volcanoConfig("https://ark.cn-beijing.volces.com/api/plan"), http.fn());

    DOCTEST_CHECK(r.error == QuotaError::None);
    DOCTEST_CHECK_EQ(r.windows.size(), static_cast<std::size_t>(3));
    // 毫秒 → 截断到秒（展示「何时重置」不需要毫秒）
    DOCTEST_CHECK_EQ(r.windows[0].resetsAt, std::string("2026-05-15T01:00:00Z"));
    // 字符串原样透传
    DOCTEST_CHECK_EQ(r.windows[1].resetsAt, std::string("2026-06-21T16:00:00Z"));
    // 0 → 空（不是 1970-01-01）
    DOCTEST_CHECK(r.windows[2].resetsAt.empty());

    // 直接对 epoch 转换函数断言（闰年 / 边界）
    DOCTEST_CHECK_EQ(volc::iso8601FromEpochSeconds(0), std::string("1970-01-01T00:00:00Z"));
    DOCTEST_CHECK_EQ(volc::iso8601FromEpochSeconds(1778806800),
                     std::string("2026-05-15T01:00:00Z"));
    DOCTEST_CHECK_EQ(volc::iso8601FromEpochSeconds(1782057600),
                     std::string("2026-06-21T16:00:00Z"));
    // 2024-02-29（闰日）+ 跨年边界
    DOCTEST_CHECK_EQ(volc::iso8601FromEpochSeconds(1709164800),
                     std::string("2024-02-29T00:00:00Z"));
    DOCTEST_CHECK_EQ(volc::iso8601FromEpochSeconds(1735689599),
                     std::string("2024-12-31T23:59:59Z"));
    return true;
})
