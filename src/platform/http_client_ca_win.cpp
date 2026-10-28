// src/platform/http_client_ca_win.cpp
// Windows 的 CA 策略：静态链接的 MinGW libcurl 编译期 CA 路径（C:/msys64/...）在用户机上不存在，
// 表现为 "Problem with the SSL CA cert (path? access rights?)"；改用 CURLSSLOPT_NATIVE_CA
// 让 OpenSSL 导入 Windows ROOT 存储。
//
// 覆盖口更正（CA 批）：本文件旧注释称「CURL_CA_BUNDLE 环境变量可覆盖（libcurl 自己会读）」
// ——不成立。CURL_CA_BUNDLE 是 curl **命令行工具**的约定，libcurl **库**不读该 env（实测
// 8.x 设了无效果）。DevDesk 的覆盖口 = DEVDESK_CA_BUNDLE：由我们代码读 env，经
// resolveCaPolicy（http_client.cpp）解析后显式下 CURLOPT_CAINFO，此时不再叠加 NATIVE
// ——换信任根，不弱化校验。详见 http_client_ca.h。
#include "http_client_ca.h"

namespace platform::http {

void applyCaOptions(CURL* curl) {
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS,
                     static_cast<long>(CURLSSLOPT_NATIVE_CA));
}

} // namespace platform::http
