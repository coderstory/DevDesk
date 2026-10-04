// src/module_claude_proxy/quota/quota_volcano.h
// 火山方舟余量查询 provider——接口 + 可单测的签名 / 解析细节。逐字移植 cc-switch coding_plan.rs。
// 签名细节暴露到头文件的理由：火山签名有三处偏离标准 AWS SigV4（固定序 canonical headers /
//   HMAC-SHA256 无 AWS4 前缀 / scope 结尾 request 且密钥派生不带前缀），没有服务端金标准向量，
//   只能靠「给定固定 AK/SK/时间/请求 → 逐字节比对签名串」的结构契约钉死；声明在 volcano_detail 里对测试可见。
#pragma once

#include <cstdint>
#include <ctime>
#include <string>

#include "quota_provider.h"

namespace module::claude::quota {

// 注册表 id（quota_registry.cpp 的火山行必须与本值一致——结构守卫会查）。
inline constexpr const char* kVolcanoProviderId = "volcano";

// 工厂：返回进程内单例（provider 无状态），调用方只借不还（见 quota_registry.h）。
QuotaProvider* volcanoProvider();

// 生产调用方只有 quota_volcano.cpp；单测直接调这些函数锁结构契约。本命名空间不承诺稳定 API。
namespace volcano_detail {

// 控制面 OpenAPI 统一网关（不是数据面推理域名 ark.cn-beijing.volces.com）；签名覆盖的 host 是它，与 base_url 无关。
inline constexpr const char* kOpenApiHost = "open.volcengineapi.com";
inline constexpr const char* kApiVersion = "2024-01-01";
inline constexpr const char* kDefaultRegion = "cn-beijing";

// 签名常量
inline constexpr const char* kService = "ark";
inline constexpr const char* kContentType = "application/json; charset=utf-8";
inline constexpr const char* kSignedHeaders = "host;x-date;x-content-sha256;content-type";

// 两个被探测的 Action
inline constexpr const char* kActionAfpUsage = "GetAFPUsage";
inline constexpr const char* kActionCodingPlanUsage = "GetCodingPlanUsage";

// RFC3986：unreserved（ALPHA / DIGIT / - _ . ~）原样，其余按 %XX 大写十六进制（逐字节，非 ASCII 走 UTF-8 字节）。
std::string uriEncode(const std::string& input);

// canonical query：Action / Region / Version 三对按 key 字母序拼接，key 与 value 都过 uriEncode。
// 同一份字符串既用于签名也用于实际 URL（必须逐字一致，否则签名不匹配）。
std::string canonicalQuery(const std::string& action, const std::string& region);

// 从 base_url 的 host 段提取 cn-* / ap-*（如 ark.cn-beijing.volces.com → cn-beijing）；无命中回落 kDefaultRegion。控制面 host 固定，不随 base_url 变化。
std::string regionFromBaseUrl(const std::string& baseUrl);

// OpenAPI 错误码是否属鉴权类：小写后子串匹配 auth / signature / accessdenied / denied / unauthorized / forbidden / credential / token。
bool isAuthErrorCode(const std::string& code);

// epoch 秒 → YYYY-MM-DDTHH:MM:SSZ（UTC，秒级）。纯算术（Howard Hinnant），不碰 libc gmtime（免平台分支与线程安全问题）。
// 上游给毫秒时调用方先 /1000，本实现截断到秒（展示「何时重置」不需要毫秒）。
std::string iso8601FromEpochSeconds(std::int64_t epochSeconds);

// 一次签名产出的三个请求头
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
