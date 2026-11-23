// 本机网卡枚举（need#490 W1）：网页访问要展示可扫码的 IPv4 地址。
// IPv4 only（PRD §1.7）；平台差异在 *_posix / *_win 实现，本头平台无关。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace platform {

// 去重；排除 0.0.0.0；含回环 127.0.0.1（扫码页需要展示回环可访问性）。
// 系统枚举失败返回空集（不抛、不阻塞）。
std::vector<std::string> localIpv4Addresses();

// 严格点分四段解析；仅 IPv4 回环段 127.0.0.0/8 为 true，"::1" / 非法串为 false。
inline bool isLoopbackAddress(const std::string& address) {
    unsigned parts[4] = {0, 0, 0, 0};
    int index = 0;
    unsigned value = 0;
    bool hasDigit = false;
    for (std::size_t i = 0; i <= address.size(); ++i) {
        const char ch = (i < address.size()) ? address[i] : '.';
        if (ch >= '0' && ch <= '9') {
            value = value * 10 + static_cast<unsigned>(ch - '0');
            if (value > 255) return false;
            hasDigit = true;
        } else if (ch == '.') {
            if (!hasDigit || index >= 4) return false;
            parts[index++] = value;
            value = 0;
            hasDigit = false;
        } else {
            return false;
        }
    }
    if (index != 4) return false;
    return parts[0] == 127;
}

} // namespace platform
