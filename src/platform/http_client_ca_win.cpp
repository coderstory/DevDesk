// src/platform/http_client_ca_win.cpp
// Windows 的 CA 策略：静态链接的 MinGW libcurl 编译期 CA 路径（C:/msys64/...）在用户机上不存在，
// 表现为 "Problem with the SSL CA cert (path? access rights?)"；改用 CURLSSLOPT_NATIVE_CA
// 让 OpenSSL 导入 Windows ROOT 存储。CURL_CA_BUNDLE 环境变量可覆盖（libcurl 自己会读）。
#include "http_client_ca.h"

namespace platform::http {

void applyCaOptions(CURL* curl) {
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS,
                     static_cast<long>(CURLSSLOPT_NATIVE_CA));
}

} // namespace platform::http
