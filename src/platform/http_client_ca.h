// src/platform/http_client_ca.h
// 平台无关接口：给 libcurl 句柄套上「本平台信任根证书」策略，必须在 curl_easy_perform 之前调用。
// Windows → CURLSSLOPT_NATIVE_CA（系统 ROOT 存储）；其他 → 空实现（libcurl 默认 CA 路径可用）。
#pragma once

#include <curl/curl.h>

namespace platform::http {

void applyCaOptions(CURL* curl);

} // namespace platform::http
