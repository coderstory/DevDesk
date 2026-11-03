// tests/test_platform/test_credential_protection.cpp
// 分平台凭据保护：protect / unprotect 往返 +「本平台列里存的到底是什么形态」的契约。
// 2026-11-03 起全平台统一明文口径（保密靠文件权限 / NTFS ACL）；唯一不支持形态是
// 旧版 Windows DPAPI 密文（dpapi: 前缀），两平台都必须 fail 并提示重填。
#include "doctest.h"

#include "platform/credential_protection.h"

#include <string>

REGISTER_TEST(credential_protect_unprotect_roundtrip,
              "credential: protect/unprotect 往返（含空串与长串）", {
    // 空串 = 「该 profile 没有密钥」这一状态本身：protect 直接返回空串，
    // unprotect 也返回空串（不把它当成「解不开」）。
    DOCTEST_CHECK(platform::credential::protect("").value().empty());
    DOCTEST_CHECK(platform::credential::unprotect("").value().empty());

    const std::string key = "sk-ant-api03-abcdefghijklmnopqrstuvwxyz-0123456789";
    auto stored = platform::credential::protect(key);
    DOCTEST_REQUIRE(stored.isOk());
    DOCTEST_CHECK(!stored.value().empty());

    auto back = platform::credential::unprotect(stored.value());
    DOCTEST_REQUIRE(back.isOk());
    DOCTEST_CHECK_EQ(back.value(), key);

    // 非 ASCII / 含 NUL 的边界：明文透传必须字节级原样
    const std::string weird = std::string("键\0值-\xc3\xa9", 9);
    auto sw = platform::credential::protect(weird);
    DOCTEST_REQUIRE(sw.isOk());
    auto bw = platform::credential::unprotect(sw.value());
    DOCTEST_REQUIRE(bw.isOk());
    DOCTEST_CHECK_EQ(bw.value(), weird);
    return true;
})

REGISTER_TEST(credential_plaintext_storage_contract,
              "credential: 全平台明文口径——protect 原样返回、storesPlaintext 为 true", {
    DOCTEST_CHECK(platform::credential::storesPlaintext());
    const std::string key = "sk-ant-plaintext-by-design-contract";
    auto stored = platform::credential::protect(key);
    DOCTEST_REQUIRE(stored.isOk());
    DOCTEST_CHECK_EQ(stored.value(), key);
    auto back = platform::credential::unprotect(key);
    DOCTEST_REQUIRE(back.isOk());
    DOCTEST_CHECK_EQ(back.value(), key);
    return true;
})

REGISTER_TEST(credential_description_matches_implementation,
              "credential: UI 说明与实际存储形态一致（不撒谎）", {
    const std::string desc = platform::credential::protectionDescription();
    DOCTEST_CHECK(!desc.empty());
    // 明文口径：说明必须点出保护落点（POSIX 0600 或 Windows NTFS），不得再宣称 DPAPI 加密
    DOCTEST_CHECK(desc.find("DPAPI") == std::string::npos);
    DOCTEST_CHECK(desc.find("0600") != std::string::npos ||
                  desc.find("NTFS") != std::string::npos);
    return true;
})

REGISTER_TEST(credential_dpapi_blob_cross_platform_guard,
              "credential: dpapi: 前缀跨平台语义——旧密文本机解不了必须报错，不当明文用", {
    const std::string blob =
        std::string(platform::credential::kDpapiBlobPrefix) +
        "01000000d08c9ddf0115d1118c7a00c04fc297eb01000000";
    DOCTEST_CHECK(platform::credential::isDpapiBlob(blob));
    DOCTEST_CHECK(!platform::credential::isDpapiBlob("sk-ant-xxx"));
    // 两平台都必须失败：谁也不能把旧 DPAPI 密文当 API Key 发出去（会变成难查的 401）
    auto r = platform::credential::unprotect(blob);
    DOCTEST_REQUIRE(r.isFail());
    DOCTEST_CHECK(!r.errorMessage().empty());
    return true;
})

REGISTER_TEST(
    credential_unprotect_rejects_unsupported_form,
    "credential: 旧 DPAPI 密文 → fail，不静默回落空串；其余非空串一律按明文", {
    // 「不可用」与「没配过」必须可分辨：dpapi: 旧密文明确报错而不是回落空串，
    // 否则「密钥不可用」被伪装成「没配过密钥」，用户无从诊断。
    const std::string legacy =
        std::string(platform::credential::kDpapiBlobPrefix) + "cafebabe";
    auto r = platform::credential::unprotect(legacy);
    DOCTEST_CHECK(r.isFail());
    // 明文平台没有「形态」概念：dpapi: 前缀之外的任何非空串都是合法明文
    DOCTEST_CHECK(platform::credential::unprotect("whatever-legacy-value").isOk());
    return true;
})
