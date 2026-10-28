// src/platform/http_client_ca.h
// 平台无关接口：给 libcurl 句柄套上「本平台信任根证书」策略，必须在 curl_easy_perform 之前调用。
// Windows → CURLSSLOPT_NATIVE_CA（系统 ROOT 存储）；其他 → 空实现（libcurl 默认 CA 路径可用）。
//
// 覆盖口（CA 批，DevDesk 自有约定）：环境变量 DEVDESK_CA_BUNDLE 指向一个 CA bundle
// 文件时，改用该文件做完整校验（CURLOPT_CAINFO），不再叠加 NATIVE——换信任根，不弱化校验。
// 注意：libcurl **库**不读 CURL_CA_BUNDLE（那是 curl 命令行工具的约定；实测 8.x 设了
// 也不生效），所以覆盖口由我们代码读 env 并显式下给 CURLOPT_CAINFO。
// 路径编码口径：bundle 路径按窄字节经 getenv→path→CAINFO 三级传递，Windows 上
// 仅保证 ASCII 路径可靠（非 ASCII 路径存在 ACP/UTF-8 错配风险，未验证；失败形态
// 为 fail-closed 可读报错，不静默）。
#pragma once

#include <curl/curl.h>

#include <core/result.h>

#include <string>

namespace platform::http {

void applyCaOptions(CURL* curl);

// CA 覆盖口的环境变量名（DevDesk 自有约定；与 curl CLI 的 CURL_CA_BUNDLE 无关）。
inline constexpr const char* kCaBundleEnv = "DEVDESK_CA_BUNDLE";

struct CaPolicy {
    bool useNative = true;  // true = 平台信任根（NATIVE_CA / 平台默认）；false = caInfo 生效
    std::string caInfo;     // useNative = false 时的 CA bundle 文件路径
};

// 解析 CA 策略（平台无关，供单测直测）：
//   envValue 为空（未设）  → Native（现状不变）；
//   非空且指向存在的文件   → Bundle（caInfo = 该路径），不叠加 NATIVE；
//   非空但文件不存在       → fail，错误含变量名与路径（可读）。
core::Result<CaPolicy> resolveCaPolicy(const std::string& envValue);

// 套用已解析策略（单一事实源：send/sendStreaming 共用，防两调用点漂移）：
// native → applyCaOptions(平台信任根)；bundle → CURLOPT_CAINFO（完整校验，不叠 NATIVE）。
void applyCaPolicy(CURL* curl, const CaPolicy& policy);

} // namespace platform::http
