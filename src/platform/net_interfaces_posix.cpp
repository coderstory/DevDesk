// 本机 IPv4 枚举：POSIX（macOS / Linux 兜底）实现。
#include "platform/net_interfaces.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <cstring>

namespace platform {

std::vector<std::string> localIpv4Addresses() {
    std::vector<std::string> out;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;

    for (ifaddrs* it = list; it != nullptr; it = it->ifa_next) {
        if (it->ifa_addr == nullptr || it->ifa_addr->sa_family != AF_INET) continue;
        if ((it->ifa_flags & IFF_UP) == 0) continue;

        char buffer[INET_ADDRSTRLEN] = {};
        const auto* addr = reinterpret_cast<const sockaddr_in*>(it->ifa_addr);
        if (inet_ntop(AF_INET, &addr->sin_addr, buffer, sizeof buffer) == nullptr) continue;

        const std::string ip(buffer);
        if (ip == "0.0.0.0") continue;
        if (std::find(out.begin(), out.end(), ip) != out.end()) continue;
        out.push_back(ip);
    }

    freeifaddrs(list);
    return out;
}

} // namespace platform
