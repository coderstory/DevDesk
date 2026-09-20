#include "doctest.h"
#include "platform/keychain.h"

DOCTEST_TEST_CASE("keychain: store and load") {
    const std::string service = "DevDeskTest";
    const std::string account = "test_account";
    const std::string secret = "sk-ant-test-secret-12345";

    // 清理
    platform::keychain::deleteSecret(service, account);

    auto store = platform::keychain::storeSecret(service, account, secret);
    DOCTEST_REQUIRE(store.isOk());

    auto load = platform::keychain::loadSecret(service, account);
    DOCTEST_REQUIRE(load.isOk());
    DOCTEST_CHECK_EQ(load.value(), secret);

    // 清理
    auto del = platform::keychain::deleteSecret(service, account);
    DOCTEST_CHECK(del.isOk());
}

DOCTEST_TEST_CASE("keychain: load nonexistent") {
    auto load = platform::keychain::loadSecret("DevDeskTest", "nonexistent_account_xyz");
    DOCTEST_CHECK(load.isFail());
}

DOCTEST_TEST_CASE("keychain: delete nonexistent ok") {
    auto del = platform::keychain::deleteSecret("DevDeskTest", "nonexistent_account_xyz");
    DOCTEST_CHECK(del.isOk());
}