// src/platform/http_client_ca.h
// 平台无关接口：给 libcurl 句柄套上「本平台信任根证书」策略，必须在 curl_easy_perform 之前调用。
// 实现按平台拆文件（平台适配接口隔离）：Windows → CURLSSLOPT_NATIVE_CA（系统 ROOT 存储）；
// 其他 → 空实现（libcurl 默认 CA 路径可用）。抽出动机：此前在 http_client.cpp 用平台宏就地分叉，
// 是 src/ 里最后一处平台宏回流；抽出后 http_client.cpp 回到平台无关。
#pragma once

#include <curl/curl.h>

namespace platform::http {

void applyCaOptions(CURL* curl);

} // namespace platform::http
