#include "doctest.h"
#include "platform/keychain.h"

// macOS 平台守卫：本平台实现走 Security.framework，只能操作**真实 login keychain**
// （keychain_macos.cpp 用 SecKeychainAddGenericPassword + nullptr 默认钥匙串，没有
// 测试专用钥匙串的注入口）——会写条目 / 全库枚举的用例若照跑，会污染用户钥匙串、
// 可能弹授权框（deleteSecretsNotIn 还按 kSecMatchLimitAll 枚举整库）。
// Windows（DPAPI + %APPDATA% 文件）与 Linux（$HOME 文件）实现不碰系统凭据库，
// 行为与从前一字不差，照常执行。
#if defined(__APPLE__)
#define SKIP_ON_MACOS_KEYCHAIN()                       \
    DOCTEST_SKIP(                                      \
        "macOS login keychain 不可隔离（无测试注入口）：跳过以免写真实钥匙串")
#else
#define SKIP_ON_MACOS_KEYCHAIN() do { } while (0)
#endif

REGISTER_TEST(keychain_store_load, "keychain: store and load", {
    SKIP_ON_MACOS_KEYCHAIN();   // 会写入真实钥匙串 → macOS 跳过

    const std::string service = "DevDeskTest";
    const std::string account = "test_account";
    const std::string secret = "sk-ant-test-secret-12345";

    platform::keychain::deleteSecret(service, account);

    auto store = platform::keychain::storeSecret(service, account, secret);
    DOCTEST_REQUIRE(store.isOk());

    auto load = platform::keychain::loadSecret(service, account);
    DOCTEST_REQUIRE(load.isOk());
    DOCTEST_CHECK_EQ(load.value(), secret);

    auto del = platform::keychain::deleteSecret(service, account);
    DOCTEST_CHECK(del.isOk());
    return true;
})

REGISTER_TEST(keychain_load_nonexistent, "keychain: load nonexistent", {
    auto load = platform::keychain::loadSecret("DevDeskTest", "nonexistent_account_xyz");
    DOCTEST_CHECK(load.isFail());
    return true;
})

REGISTER_TEST(keychain_delete_nonexistent, "keychain: delete nonexistent ok", {
    auto del = platform::keychain::deleteSecret("DevDeskTest", "nonexistent_account_xyz");
    DOCTEST_CHECK(del.isOk());
    return true;
})

REGISTER_TEST(keychain_delete_secrets_not_in,
              "keychain: deleteSecretsNotIn 只留 keep 集合", {
    SKIP_ON_MACOS_KEYCHAIN();   // 写 3 条 + 枚举整库 → macOS 跳过

    const std::string service = "DevDeskTestOrphan";
    // 三个密钥：keep 两个，孤儿一个
    DOCTEST_REQUIRE(platform::keychain::storeSecret(
        service, "claude:1", "sk-keep-1").isOk());
    DOCTEST_REQUIRE(platform::keychain::storeSecret(
        service, "claude:2", "sk-keep-2").isOk());
    DOCTEST_REQUIRE(platform::keychain::storeSecret(
        service, "claude:999", "sk-orphan").isOk());

    auto r = platform::keychain::deleteSecretsNotIn(
        service, {"claude:1", "claude:2"});
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_EQ(r.value().size(), 1u);

    // 孤儿被删，保留的仍可读
    DOCTEST_CHECK(platform::keychain::loadSecret(service, "claude:999").isFail());
    auto k1 = platform::keychain::loadSecret(service, "claude:1");
    DOCTEST_REQUIRE(k1.isOk());
    DOCTEST_CHECK_EQ(k1.value(), std::string("sk-keep-1"));
    auto k2 = platform::keychain::loadSecret(service, "claude:2");
    DOCTEST_REQUIRE(k2.isOk());
    DOCTEST_CHECK_EQ(k2.value(), std::string("sk-keep-2"));

    platform::keychain::deleteSecret(service, "claude:1");
    platform::keychain::deleteSecret(service, "claude:2");
    return true;
})