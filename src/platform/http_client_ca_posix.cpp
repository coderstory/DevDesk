// src/platform/http_client_ca_posix.cpp
// 非 Windows（macOS/Linux）的 CA 策略（接口见 http_client_ca.h）。
//
// libcurl 默认 CA 路径在这些平台本就存在，无需额外设置。保留本文件是为了让
// http_client.cpp 完全平台无关（审计 CP-03）；将来某平台需要自己的 CA 策略
// （证书路径 / 企业代理 / ALPN 差异）时，在 CMake 里为它拆出该平台文件即可，
// 共享层不用再改。
#include "http_client_ca.h"

namespace platform::http {

void applyCaOptions(CURL* curl) {
    (void)curl; // 本平台沿用 libcurl 默认
}

} // namespace platform::http
