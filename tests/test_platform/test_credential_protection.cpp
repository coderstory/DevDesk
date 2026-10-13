// tests/test_platform/test_credential_protection.cpp
// 分平台凭据保护：protect / unprotect 往返 +「本平台列里存的到底是什么形态」的契约。
//
// 核心回归网：Windows 上**不得降级为明文**这条由
// `credential_stored_form_is_not_plaintext_on_encrypting_platform` 钉住——若有人把
// credential_protection_win.cpp 的 DPAPI 换成透传，本用例在 Windows 上立刻变红。
// macOS/POSIX 上该断言按平台契约自动放行（见 storesPlaintext()）。
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

    // 非 ASCII / 含 NUL 的边界（DPAPI 是二进制路径，hex 编码必须扛住）
    const std::string weird = std::string("键\0值-\xc3\xa9", 9);
    auto sw = platform::credential::protect(weird);
    DOCTEST_REQUIRE(sw.isOk());
    auto bw = platform::credential::unprotect(sw.value());
    DOCTEST_REQUIRE(bw.isOk());
    DOCTEST_CHECK_EQ(bw.value(), weird);
    return true;
})

REGISTER_TEST(credential_stored_form_is_not_plaintext_on_encrypting_platform,
              "credential: 加密平台列里不得是明文（Windows DPAPI 硬约束）", {
    const std::string key = "sk-ant-secret-must-not-appear-verbatim";
    auto stored = platform::credential::protect(key);
    DOCTEST_REQUIRE(stored.isOk());
    DOCTEST_REQUIRE(!stored.value().empty());

    if (platform::credential::storesPlaintext()) {
        // macOS / POSIX：设计就是「明文进列 + 文件权限 0600」，保护级别由
        // core/file_perms.h 提供（那条闸的用例见 test_file_perms.cpp）
        DOCTEST_CHECK_EQ(stored.value(), key);
    } else {
        // 加密平台：列里既不含明文，也不能退化成任何可直读的形态
        DOCTEST_CHECK(stored.value().find(key) == std::string::npos);
        DOCTEST_CHECK_NE(stored.value(), key);
        // 加密不得破坏往返
        auto back = platform::credential::unprotect(stored.value());
        DOCTEST_REQUIRE(back.isOk());
        DOCTEST_CHECK_EQ(back.value(), key);
    }
    return true;
})

REGISTER_TEST(credential_description_matches_implementation,
              "credential: UI 说明与实际存储形态一致（不撒谎）", {
    const std::string desc = platform::credential::protectionDescription();
    DOCTEST_CHECK(!desc.empty());
    // 明文平台必须写明文件权限；加密平台必须写明加密。两者不得互换（UI 文案与实际
    // 行为不一致是把「已加密」说成没加密的反向打脸）。
    if (platform::credential::storesPlaintext()) {
        DOCTEST_CHECK(desc.find("0600") != std::string::npos);
        DOCTEST_CHECK(desc.find("DPAPI") == std::string::npos);
    } else {
        DOCTEST_CHECK(desc.find("DPAPI") != std::string::npos);
    }
    return true;
})

REGISTER_TEST(credential_unprotect_rejects_broken_form,
              "credential: 形态损坏 → fail，不静默回落空串", {
    // 「解不开」与「没配过」必须可分辨：不静默回落空串，否则「密钥不可用」被
    // 伪装成「没配过密钥」，用户无从诊断。
    const std::string broken = platform::credential::storesPlaintext()
                                   ? std::string("not-a-valid-form-on-this-platform")
                                   : std::string("zz-not-hex-zz");
    auto r = platform::credential::unprotect(broken);
    if (platform::credential::storesPlaintext()) {
        // 明文平台没有「形态」概念：任何非空串都是合法明文
        DOCTEST_CHECK(r.isOk());
    } else {
        DOCTEST_CHECK(r.isFail());
    }
    return true;
})
