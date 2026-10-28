// S6a 平台代理扩展：Request.proxyMode → CURLOPT_PROXY/CURLOPT_NOPROXY 三模式映射。
// 全部用例只打纯函数缝 resolveProxy()（无 curl 句柄、无网络），每条注明防什么事故。
#include "doctest.h"
#include "test_support.h"
#include "platform/http_client.h"

#include <string>

namespace {

using platform::http::ProxyDecision;
using platform::http::ProxyMode;

int kindAsInt(ProxyDecision::Kind k) { return static_cast<int>(k); }

} // namespace

// 防：默认构造的 Request 代理行为漂移——不传 proxy_mode/proxy 的既有消费方
//（claude_proxy 等）必须与现状逐字节一致（吃环境变量代理 + 回环排除）。
REGISTER_TEST(proxy_mode_default_is_follow_system, "S6a: Request 默认 proxyMode = FollowSystem", {
    const platform::http::Request req;
    DOCTEST_CHECK_EQ(static_cast<int>(req.proxyMode),
                     static_cast<int>(ProxyMode::FollowSystem));
    DOCTEST_CHECK(req.proxy.empty());
    return true;
})

// 防：FollowSystem + 空代理误接线成「显式空 PROXY」——那会整体杀掉环境变量代理
//（变直连），所有走系统代理的上游全断。正确口径是 UseSystem：不设 CURLOPT_PROXY，
// libcurl 现状（环境变量 + 回环排除）照旧。
REGISTER_TEST(proxy_follow_system_empty_uses_env, "S6a: FollowSystem + 空 proxy → 环境变量 + 回环排除", {
    const auto d = platform::http::resolveProxy(ProxyMode::FollowSystem, "");
    DOCTEST_CHECK(d.isOk());
    DOCTEST_CHECK_EQ(kindAsInt(d.value().kind),
                     kindAsInt(ProxyDecision::Kind::UseSystem));
    DOCTEST_CHECK(d.value().proxyUrl.empty());
    return true;
})

// 防：既有消费方回归——历史语义「proxy 非空即显式代理」在 FollowSystem 下必须保留
//（claude_proxy 设 proxy 不设 mode）；若 FollowSystem 忽略 proxy 字段，本地代理链即断。
REGISTER_TEST(proxy_follow_system_nonempty_legacy, "S6a: FollowSystem + 非空 proxy → 显式代理（历史现状）", {
    const auto d = platform::http::resolveProxy(ProxyMode::FollowSystem, "127.0.0.1:7890");
    DOCTEST_CHECK(d.isOk());
    DOCTEST_CHECK_EQ(kindAsInt(d.value().kind),
                     kindAsInt(ProxyDecision::Kind::Custom));
    DOCTEST_CHECK_EQ(d.value().proxyUrl, std::string("http://127.0.0.1:7890"));
    return true;
})

// 防：Disabled 漏接线成 UseSystem——语义是「显式置空 CURLOPT_PROXY 压过环境变量」，
// 映射错则公司代理环境下的直连请求被劫持（与 tcpReachable 失真同源）。
REGISTER_TEST(proxy_disabled_forces_direct, "S6a: Disabled → CURLOPT_PROXY 显式置空（直连）", {
    const auto d = platform::http::resolveProxy(ProxyMode::Disabled, "127.0.0.1:7890");
    DOCTEST_CHECK(d.isOk());
    DOCTEST_CHECK_EQ(kindAsInt(d.value().kind),
                     kindAsInt(ProxyDecision::Kind::Disabled));
    // Disabled 忽略 proxy 字段：带上代理地址也必须是直连口径。
    DOCTEST_CHECK(d.value().proxyUrl.empty());
    return true;
})

// 防：Custom 压环境变量失败——curl 语义「显式设置 CURLOPT_PROXY 覆盖环境变量」，
// 映射错成 UseSystem 则环境代理抢先、渠道级代理配置静默失效（最难察觉的回归）。
REGISTER_TEST(proxy_custom_overrides_env, "S6a: Custom → CURLOPT_PROXY 覆盖（压过环境变量）", {
    const auto d = platform::http::resolveProxy(ProxyMode::Custom, "proxy.corp:8080");
    DOCTEST_CHECK(d.isOk());
    DOCTEST_CHECK_EQ(kindAsInt(d.value().kind),
                     kindAsInt(ProxyDecision::Kind::Custom));
    DOCTEST_CHECK_EQ(d.value().proxyUrl, std::string("http://proxy.corp:8080"));
    return true;
})

// 防：Custom 空 URL 静默直连/吃环境——渠道配了 Custom 却忘填地址时，请求要么
// 静默直连要么被环境代理劫持，都不符合「自定义代理」的显式意图；必须在发起前报错。
REGISTER_TEST(proxy_custom_empty_url_fails, "S6a: Custom + 空 proxy URL → ValidationError", {
    const auto d = platform::http::resolveProxy(ProxyMode::Custom, "");
    DOCTEST_CHECK(d.isFail());
    DOCTEST_CHECK_EQ(static_cast<int>(d.errorCode()),
                     static_cast<int>(core::ErrorCode::ValidationError));
    return true;
})
