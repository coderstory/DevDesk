// 本机 IPv4 枚举：Windows 实现（GetAdaptersAddresses(AF_INET)）。
// winsock2.h 必须先于 windows.h（项目其它 WIN32 TU 同一纪律）；本 TU 不引 windows.h。
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include "platform/net_interfaces.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace platform {
namespace {

constexpr ULONG kSkipFlags =
    GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;

} // namespace

std::vector<std::string> localIpv4Addresses() {
    std::vector<std::string> out;
    ULONG size = 16 * 1024;
    std::vector<std::uint8_t> storage(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
    ULONG rc = GetAdaptersAddresses(AF_INET, kSkipFlags, nullptr, adapters, &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        storage.resize(size);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
        rc = GetAdaptersAddresses(AF_INET, kSkipFlags, nullptr, adapters, &size);
    }
    if (rc != NO_ERROR) return out;

    for (IP_ADAPTER_ADDRESSES* adapter = adapters; adapter != nullptr;
         adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp) continue;
        for (IP_ADAPTER_UNICAST_ADDRESS* unicast = adapter->FirstUnicastAddress;
             unicast != nullptr; unicast = unicast->Next) {
            if (unicast->Address.lpSockaddr == nullptr) continue;
            if (unicast->Address.lpSockaddr->sa_family != AF_INET) continue;

            const auto* addr =
                reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
            char buffer[INET_ADDRSTRLEN] = {};
            if (inet_ntop(AF_INET, &addr->sin_addr, buffer, sizeof buffer) == nullptr) continue;

            const std::string ip(buffer);
            if (ip == "0.0.0.0") continue;
            if (std::find(out.begin(), out.end(), ip) != out.end()) continue;
            out.push_back(ip);
        }
    }
    return out;
}

} // namespace platform
