// tests/test_platform/test_credential_protection.cpp
// 分平台凭据保护：protect / unprotect 往返 +「本平台列里存的到底是什么形态」的契约。
// 2026-11-03 起全平台统一明文口径（保密靠文件权限 / NTFS ACL）；旧版 Windows DPAPI 密文
// （dpapi: 前缀 / bug #126 裸 hex 残角）：Windows 只读迁移尝试本机解密，解不开与
// POSIX 侧一律 fail 并提示重填——不把密文当 API Key 发出去（会变成难查的 401）。
#include "doctest.h"

#include "platform/credential_protection.h"

#include <string>

#if defined(_WIN32)
// bug #126 夹具需真实 CryptProtectData（平台 API；测试侧平台条件是 test_platform 既有惯例，
// 与业务代码零 #ifdef 约束不冲突）。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#endif

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

#if defined(_WIN32)
// bug #126 夹具一：裸 hex 往返——CryptProtectData 产物转小写 hex 落列（旧版形态，无 dpapi: 前缀），
// isDpapiBlob 三重判据必须认出，realUnprotect 本机解密回原明文（只读迁移，旧数据免重填）。
REGISTER_TEST(credential_bare_hex_dpapi_roundtrip,
              "credential: 裸 hex DPAPI blob（无前缀）→ 认出且本机解密回明文（bug #126）", {
    const std::string key = "sk-ant-bare-hex-roundtrip-0123456789abcdef";
    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(key.data()));
    in.cbData = static_cast<DWORD>(key.size());
    DATA_BLOB out{};
    DOCTEST_REQUIRE(CryptProtectData(&in, nullptr, nullptr, nullptr, nullptr,
                                     CRYPTPROTECT_UI_FORBIDDEN, &out));
    const std::string blob(reinterpret_cast<const char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    // 夹具自检：真实 DPAPI blob 定长头就 60 字节，必过长度关
    DOCTEST_REQUIRE(blob.size() >= 60);

    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string stored;
    stored.reserve(blob.size() * 2);
    for (const unsigned char b : blob) {
        stored += kHexDigits[b >> 4];
        stored += kHexDigits[b & 0x0F];
    }
    DOCTEST_CHECK(stored.rfind("dpapi:", 0) != 0);   // 裸 hex：确实无前缀
    DOCTEST_CHECK(platform::credential::isDpapiBlob(stored));

    auto r = platform::credential::unprotect(stored);
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_EQ(r.value(), key);
    return true;
})

// bug #126 夹具二：截断篡改 → 失败臂。截到 70 字节（magic 与 ≥60 字节关都完好，
// isDpapiBlob 照样认出）但 blob 结构已破坏 → CryptUnprotectData 必败 → fail 带
// 「重新填写」可执行文案；透传原 blob 当明文的实现会 ok 返回密文，被 isFail 断言挡下。
REGISTER_TEST(credential_bare_hex_dpapi_tampered_refill_error,
              "credential: 裸 hex 截断篡改 → fail 带「重新填写」可执行文案（bug #126）", {
    const std::string key = "sk-ant-tamper-arm-0123456789";
    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(key.data()));
    in.cbData = static_cast<DWORD>(key.size());
    DATA_BLOB out{};
    DOCTEST_REQUIRE(CryptProtectData(&in, nullptr, nullptr, nullptr, nullptr,
                                     CRYPTPROTECT_UI_FORBIDDEN, &out));
    std::string blob(reinterpret_cast<const char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    DOCTEST_REQUIRE(blob.size() >= 70);
    blob.resize(70);   // 截断：判据三关全过，CryptUnprotectData 结构校验必败

    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string stored;
    stored.reserve(blob.size() * 2);
    for (const unsigned char b : blob) {
        stored += kHexDigits[b >> 4];
        stored += kHexDigits[b & 0x0F];
    }
    DOCTEST_CHECK(platform::credential::isDpapiBlob(stored));
    auto r = platform::credential::unprotect(stored);
    DOCTEST_REQUIRE(r.isFail());
    DOCTEST_CHECK(r.errorMessage().find("重新填写") != std::string::npos);
    return true;
})
#endif  // _WIN32

// bug #126 夹具三：防误杀——16/32 字符短 hex 明文 key 不得被当 DPAPI 密文（三重判据的
// 长度关一票否决）；长 hex 但首字节非 magic 同样放行（magic 关兜底）。全平台生效。
REGISTER_TEST(credential_short_hex_plaintext_no_false_positive,
              "credential: 短 hex 明文 key 零误杀——不识别为 DPAPI、unprotect 原样返回（bug #126）", {
    const std::string short16 = "0123456789abcdef";
    const std::string short32 = "a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6";
    DOCTEST_CHECK(!platform::credential::isDpapiBlob(short16));
    DOCTEST_CHECK(!platform::credential::isDpapiBlob(short32));
    auto a = platform::credential::unprotect(short16);
    DOCTEST_REQUIRE(a.isOk());
    DOCTEST_CHECK_EQ(a.value(), short16);
    auto b = platform::credential::unprotect(short32);
    DOCTEST_REQUIRE(b.isOk());
    DOCTEST_CHECK_EQ(b.value(), short32);

    // 200 个 'a'（=100 字节 0xAA）：过长度关与 hex 关，死在 magic 关 → 仍按明文放行
    const std::string longNonMagic(200, 'a');
    DOCTEST_CHECK(!platform::credential::isDpapiBlob(longNonMagic));
    auto c = platform::credential::unprotect(longNonMagic);
    DOCTEST_REQUIRE(c.isOk());
    DOCTEST_CHECK_EQ(c.value(), longNonMagic);
    return true;
})
