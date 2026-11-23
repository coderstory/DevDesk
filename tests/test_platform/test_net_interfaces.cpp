// 本机 IPv4 枚举覆盖（need#490 W1）：网页访问页要展示可扫码地址，回环判定决定
// 「回环免验」分支。用例守两件事：枚举结果含回环且不去重就重复（把 find 去重删掉必红）；
// 回环判定只认 IPv4 127/8（把 "::1" 判 true 或宽进 "127.0.0.999" 必红）。
#include "doctest.h"

// 顺带钉住 core 窄接口的 include 纪律：该头只允许标准库 + core，平台/EUI 头混入即编译失败
#include "core/web_terminal_service.h"
#include "platform/net_interfaces.h"

#include <algorithm>
#include <string>
#include <vector>

REGISTER_TEST(net_interfaces_local_ipv4_addresses,
              "net_interfaces: 本机 IPv4 含回环、去重、无 0.0.0.0", {
    const std::vector<std::string> addresses = platform::localIpv4Addresses();
    DOCTEST_CHECK_TRUE(
        std::find(addresses.begin(), addresses.end(), "127.0.0.1") != addresses.end());

    std::vector<std::string> sorted = addresses;
    std::sort(sorted.begin(), sorted.end());
    DOCTEST_CHECK_TRUE(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());

    DOCTEST_CHECK_TRUE(std::find(addresses.begin(), addresses.end(), "0.0.0.0") ==
                       addresses.end());
    return true;
});

REGISTER_TEST(net_interfaces_is_loopback_address,
              "net_interfaces: isLoopbackAddress 严格 IPv4 127/8", {
    DOCTEST_CHECK_TRUE(platform::isLoopbackAddress("127.0.0.1"));
    DOCTEST_CHECK_TRUE(platform::isLoopbackAddress("127.255.255.255"));
    DOCTEST_CHECK_TRUE(platform::isLoopbackAddress("127.1.2.3"));
    DOCTEST_CHECK_FALSE(platform::isLoopbackAddress("::1"));
    DOCTEST_CHECK_FALSE(platform::isLoopbackAddress("128.0.0.1"));
    DOCTEST_CHECK_FALSE(platform::isLoopbackAddress("192.168.1.10"));
    DOCTEST_CHECK_FALSE(platform::isLoopbackAddress(""));
    DOCTEST_CHECK_FALSE(platform::isLoopbackAddress("127.0.0.256"));
    DOCTEST_CHECK_FALSE(platform::isLoopbackAddress("127.0.0.1x"));
    DOCTEST_CHECK_FALSE(platform::isLoopbackAddress("127.0.0"));
    return true;
});
