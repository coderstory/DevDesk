// src/platform/http_client_ca_posix.cpp
// 非 Windows（macOS/Linux）的 CA 策略：libcurl 默认 CA 路径本就存在，无需额外设置；
// 本文件让 http_client.cpp 保持平台无关。
#include "http_client_ca.h"

namespace platform::http {

void applyCaOptions(CURL* curl) {
    (void)curl; // 本平台沿用 libcurl 默认
}

} // namespace platform::http
