// src/platform/http_client_ca.h
// 平台无关接口：给 libcurl 句柄套上「本平台信任根证书」策略。
//
// 实现按平台拆文件（CLAUDE.md「平台适配代码必须接口隔离」）：
//   Windows: http_client_ca_win.cpp   → CURLSSLOPT_NATIVE_CA（系统 ROOT 存储）
//   其他:    http_client_ca_posix.cpp → 空实现（libcurl 默认 CA 路径可用的平台）
//
// 审计 CP-03：这段策略此前写在 http_client.cpp 里用平台宏就地分叉，是 src/
// 里最后一处平台宏回流（T6/W2「业务代码零平台宏」的 grep 验收口径因此失效）；
// 抽出后 http_client.cpp 回到平台无关（本文件照惯例不写平台宏字面量，避免
// 污染 grep 口径）。
#pragma once

#include <curl/curl.h>

namespace platform::http {

// 设置 CA 相关 curl_easy_setopt。必须在 curl_easy_perform 之前调用。
// 逐平台实现见 http_client_ca_win.cpp / http_client_ca_posix.cpp。
void applyCaOptions(CURL* curl);

} // namespace platform::http
