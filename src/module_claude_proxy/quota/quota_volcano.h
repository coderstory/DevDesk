// src/module_claude_proxy/quota/quota_volcano.h
// 火山方舟余量查询 provider（G4）——接口 + **可单测的签名/解析细节**
// （实施计划 docs/superpowers/plans/2026-10-02-quota-implementation-plan.md §G4）
//
// 业务逻辑逐字移植自 cc-switch `src-tauri/src/services/coding_plan.rs`（rev bfaaba16）
// 的火山分支（约 :835-1308）。与本文件对应的行号：
//   - 控制面网关常量 / 错误三分类        :849-867
//   - region 提取                        :872-884
//   - 鉴权错误码子串表                   :887-897
//   - ResponseMetadata.Error 抽取        :900-920
//   - 签名 V4（火山变体）                :926-1029
//   - OpenAPI 调用 + 状态码分类          :1031-1113
//   - AFP 解析（Quota<=0 跳过）          :1122-1148
//   - Coding Plan 窗口归一 + 解析        :1151-1207
//   - 双 plan 探测顺序 / 回落            :1235-1308
//   - base_url 命中                      :51-56
//
// 为什么签名细节要暴露到头文件：火山签名有三处**偏离标准 AWS SigV4** 的地方
// （固定序 canonical headers / `HMAC-SHA256` 无 AWS4 前缀 / scope 结尾 `request`
// 且签名密钥派生不带 AWS4 前缀，coding_plan.rs:926-934），没有服务端金标准向量
// 时只能靠「给定固定 AK/SK/时间/请求 → 逐字节比对签名串」的结构契约把它们钉死。
// 签名函数放在 quota_volcano.cpp，但声明在 volcano_detail 命名空间里对测试可见。
#pragma once

#include <cstdint>
#include <ctime>
#include <string>

#include "quota_provider.h"

namespace module::claude::quota {

// 注册表 id（quota_registry.cpp 的火山行必须与本值一致——结构守卫会查）。
inline constexpr const char* kVolcanoProviderId = "volcano";

// 工厂：返回**进程内单例**（provider 无状态），调用方只借不还（见 quota_registry.h）。
QuotaProvider* volcanoProvider();

// ── 签名 / 解析细节 ────────────────────────────────────────
// 生产调用方只有 quota_volcano.cpp；单测直接调这些函数锁结构契约。
// 本命名空间**不承诺稳定 API**，对外只有 kVolcanoProviderId / volcanoProvider()。
namespace volcano_detail {

// 控制面 OpenAPI 统一网关（coding_plan.rs:850-853）——**不是**数据面推理域名
// `ark.cn-beijing.volces.com`。签名覆盖的 host 是它，与 profile 的 base_url 无关。
inline constexpr const char* kOpenApiHost = "open.volcengineapi.com";
inline constexpr const char* kApiVersion = "2024-01-01";
inline constexpr const char* kDefaultRegion = "cn-beijing";

// 签名常量（coding_plan.rs:936-938）
inline constexpr const char* kService = "ark";
inline constexpr const char* kContentType = "application/json; charset=utf-8";
inline constexpr const char* kSignedHeaders = "host;x-date;x-content-sha256;content-type";

// 两个被探测的 Action（coding_plan.rs:1251 / :1272）
inline constexpr const char* kActionAfpUsage = "GetAFPUsage";
inline constexpr const char* kActionCodingPlanUsage = "GetCodingPlanUsage";

// RFC3986：unreserved（ALPHA / DIGIT / `-` / `_` / `.` / `~`）原样，其余按 %XX
// **大写**十六进制（coding_plan.rs:954-968，逐字节处理，非 ASCII 走 UTF-8 字节）。
std::string uriEncode(const std::string& input);

// canonical query：`Action` / `Region` / `Version` 三对按 **key 字母序**拼接，
// key 与 value 都过 uriEncode（coding_plan.rs:972-984）。
// 同一份字符串既用于签名也用于实际 URL（两者必须逐字一致，否则签名不匹配）。
std::string canonicalQuery(const std::string& action, const std::string& region);

// 从数据面 base_url 的 host 段提取 `cn-*` / `ap-*`（如
// `ark.cn-beijing.volces.com` → `cn-beijing`）；无命中回落 kDefaultRegion
// （coding_plan.rs:872-884）。控制面 host 固定，不随 base_url 变化。
std::string regionFromBaseUrl(const std::string& baseUrl);

// OpenAPI 错误码是否属鉴权类（coding_plan.rs:887-897）：小写后子串匹配
// auth / signature / accessdenied / denied / unauthorized / forbidden /
// credential / token。
bool isAuthErrorCode(const std::string& code);

// epoch 秒 → `YYYY-MM-DDTHH:MM:SSZ`（UTC，秒级精度）。
// 纯算术（Howard Hinnant civil_from_days），不碰 libc 的 gmtime——免平台分支、
// 免线程安全问题。上游给毫秒时由调用方先 /1000（cc-switch 的 millis_to_iso8601
// 会保留毫秒小数位，本实现截断到秒：展示「何时重置」不需要毫秒，且格式与 G0
// 头文件示例 `2026-10-02T12:00:00Z` 一致）。
std::string iso8601FromEpochSeconds(std::int64_t epochSeconds);

// 一次签名产出的三个请求头（coding_plan.rs:986-1029）。
struct SignedHeaders {
    std::string authorization;   // `HMAC-SHA256 Credential=..., SignedHeaders=..., Signature=...`
    std::string xDate;           // `20240621T000000Z`
    std::string xContentSha256;  // body 的 sha256 hex
};

// 火山签名 V4（**逐字照搬**，勿"优化"）：
//   canonical_request = "POST\n/\n{q}\n{canonical_headers}\n{SIGNED_HEADERS}\n{body_sha}"
//   canonical_headers = 固定序 `host:x-date:x-content-sha256:content-type`（不排序！）
//   string_to_sign    = "HMAC-SHA256\n{x_date}\n{scope}\n{sha256(canonical_request)}"
//   scope             = "{yyyymmdd}/{region}/ark/request"（结尾是 `request`）
//   kDate=HMAC(SK,date) → kRegion → kService → kSigning=HMAC(kService,"request")
// `nowUtc` 作参数传入（生产传 time(nullptr)），单测才能确定性复现。
SignedHeaders signRequest(const std::string& accessKeyId,
                          const std::string& secretAccessKey,
                          const std::string& region,
                          const std::string& canonicalQuery,
                          const std::string& body,
                          std::time_t nowUtc);

}  // namespace volcano_detail

}  // namespace module::claude::quota
