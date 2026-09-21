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