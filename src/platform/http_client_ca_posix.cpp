// src/platform/http_client_ca_posix.cpp
// 非 Windows（macOS/Linux）的 CA 策略：libcurl 默认 CA 路径本就存在，无需额外设置。
// 保留本文件是为让 http_client.cpp 完全平台无关；将来某平台需自己的 CA 策略时在 CMake 里拆文件即可。
#include "http_client_ca.h"

namespace platform::http {

void applyCaOptions(CURL* curl) {
    (void)curl; // 本平台沿用 libcurl 默认
}

} // namespace platform::http
