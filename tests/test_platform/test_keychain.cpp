#include "doctest.h"
#include "platform/keychain.h"

#include <core/paths.h>

#include <filesystem>

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

// ==================== CP-11：account → 文件名映射 + 旧命名兼容 ====================
// 下面两个用例断言的是 keychain_win.cpp 的磁盘命名契约（<encode(account)>.key），
// 只看 Windows 实现（文件存储）；macOS 走 Security.framework、Linux 走 posix 文件
// 实现，命名规则不同 → 整段用 _WIN32 门控（与 test_claude_service.cpp 同款处理）。
#if defined(_WIN32)

// 旧规则（非白名单字符一律折成 '_'）下 "claude:1" 与 "claude/1" 折叠成同一个
// claude_1.key：后写的覆盖先写的。新编码（%XX 转义）下必须落两个不同文件。
REGISTER_TEST(keychain_account_names_do_not_fold,
              "keychain: 折叠 account 落不同文件且互不覆盖", {
    const std::string service = "DevDeskTestKeychainEncode";
    const std::string colon = "claude:1";
    const std::string slash = "claude/1";   // 旧规则下与 claude:1 同文件

    platform::keychain::deleteSecret(service, colon);
    platform::keychain::deleteSecret(service, slash);

    DOCTEST_REQUIRE(
        platform::keychain::storeSecret(service, colon, "sk-colon").isOk());
    DOCTEST_REQUIRE(
        platform::keychain::storeSecret(service, slash, "sk-slash").isOk());

    // 两个 account 的文件并存（旧规则下只会有 1 个 claude_1.key）
    const auto dir = core::paths::appDataDir() / "keychain" / service;
    std::error_code ec;
    DOCTEST_CHECK(std::filesystem::exists(dir / "claude%3A1.key", ec));
    DOCTEST_CHECK(std::filesystem::exists(dir / "claude%2F1.key", ec));
    DOCTEST_CHECK(!std::filesystem::exists(dir / "claude_1.key", ec));

    // 互不覆盖：重写其中一个，另一个的值不动
    DOCTEST_REQUIRE(
        platform::keychain::storeSecret(service, colon, "sk-colon-2").isOk());
    auto c = platform::keychain::loadSecret(service, colon);
    DOCTEST_REQUIRE(c.isOk());
    DOCTEST_CHECK_EQ(c.value(), std::string("sk-colon-2"));
    auto s = platform::keychain::loadSecret(service, slash);
    DOCTEST_REQUIRE(s.isOk());
    DOCTEST_CHECK_EQ(s.value(), std::string("sk-slash"));

    platform::keychain::deleteSecret(service, colon);
    platform::keychain::deleteSecret(service, slash);
    return true;
})

// 升级前写入的文件用的是旧命名规则：新编码路径缺失时必须回退读旧文件（不迁移），
// 且孤儿清理不能把「账号仍在用、只是文件名是旧形态」的条目当孤儿删掉，删除时
// 两侧都清（否则 load 会经回退把已删的密钥读回来）。
REGISTER_TEST(keychain_legacy_naming_still_readable,
              "keychain: 旧命名文件仍可读且不被孤儿清理误删", {
    const std::string service = "DevDeskTestKeychainLegacy";
    const std::string account = "legacy:probe";   // 旧规则名 = legacy_probe

    platform::keychain::deleteSecret(service, account);

    // 用新编码写一份合法密文（DPAPI 只能同机同用户解，密文必须真造），
    // 再在磁盘上改名成旧规则的名字，模拟升级前留下的文件
    DOCTEST_REQUIRE(platform::keychain::storeSecret(
        service, account, "sk-legacy-value").isOk());

    const auto dir = core::paths::appDataDir() / "keychain" / service;
    const auto encoded = dir / "legacy%3Aprobe.key";
    const auto legacy = dir / "legacy_probe.key";
    std::error_code ec;
    DOCTEST_REQUIRE(std::filesystem::exists(encoded, ec));
    std::filesystem::remove(legacy, ec);
    ec.clear();
    std::filesystem::rename(encoded, legacy, ec);
    DOCTEST_REQUIRE_MESSAGE(!ec, "模拟旧命名文件失败: " + ec.message());

    // 新编码路径缺失 → 回退读旧命名文件
    auto r = platform::keychain::loadSecret(service, account);
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_EQ(r.value(), std::string("sk-legacy-value"));

    // 孤儿清理：account 仍在 keep 集合里 → 旧命名文件必须保留
    auto swept = platform::keychain::deleteSecretsNotIn(service, {account});
    DOCTEST_REQUIRE(swept.isOk());
    DOCTEST_CHECK(swept.value().empty());
    DOCTEST_CHECK(std::filesystem::exists(legacy, ec));

    // 删除：新旧两侧都清 → load 必须 NotFound（不能经回退把密钥读回来）
    DOCTEST_REQUIRE(platform::keychain::deleteSecret(service, account).isOk());
    DOCTEST_CHECK(platform::keychain::loadSecret(service, account).isFail());
    DOCTEST_CHECK(!std::filesystem::exists(legacy, ec));
    return true;
})

#endif  // _WIN32