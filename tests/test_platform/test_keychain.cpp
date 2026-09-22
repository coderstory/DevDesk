#include "doctest.h"
#include "platform/keychain.h"

REGISTER_TEST(keychain_store_load, "keychain: store and load", {
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