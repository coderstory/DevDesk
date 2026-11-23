// 网页终端加密原语覆盖（need#490 W1，技术卡 §6 / §12.1）。
// 每条用例守一个具体事故：RFC 官方向量逐字节命中（删/改任一原语必红）、认证失败必须
// 报错（把 tag 校验删掉必红）、nonce 方向混用必拒（方向标签退化必红）、信封开销恒 29B
// （多加/少加字段必红）。
#include "doctest.h"

#include "platform/web_crypto.h"

#include <cstdint>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;
namespace wc = platform::web_crypto;

int nibble(char ch) { return (ch >= '0' && ch <= '9') ? (ch - '0') : (ch - 'a' + 10); }

Bytes hex(const char* text) {
    const std::string s(text);
    Bytes out(s.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::uint8_t>((nibble(s[i * 2]) << 4) | nibble(s[i * 2 + 1]));
    }
    return out;
}

std::string toHex(const Bytes& data) {
    static const char* kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (const std::uint8_t b : data) {
        out.push_back(kDigits[b >> 4]);
        out.push_back(kDigits[b & 0x0F]);
    }
    return out;
}

Bytes text(const std::string& s) { return Bytes(s.begin(), s.end()); }

std::string infoOf(const Bytes& bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

} // namespace

// RFC 7748 §5.2：X25519(scalar, u-coordinate) 两个向量
REGISTER_TEST(web_crypto_x25519_rfc7748_scalarmult,
              "web_crypto: X25519 RFC 7748 §5.2 标量乘法两向量逐字节命中", {
    const auto out1 = wc::x25519SharedSecret(
        hex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4"),
        hex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c"));
    DOCTEST_CHECK_TRUE(out1.isOk());
    DOCTEST_CHECK_EQ(toHex(out1.value()),
                     std::string("c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552"));

    const auto out2 = wc::x25519SharedSecret(
        hex("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d"),
        hex("e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493"));
    DOCTEST_CHECK_TRUE(out2.isOk());
    DOCTEST_CHECK_EQ(toHex(out2.value()),
                     std::string("95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957"));
    return true;
});

// RFC 7748 §6.1：公钥派生 + 双向共享密钥一致
REGISTER_TEST(web_crypto_x25519_rfc7748_dh,
              "web_crypto: X25519 RFC 7748 §6.1 公钥派生与共享密钥", {
    const Bytes alicePriv = hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    const Bytes bobPriv = hex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");

    const auto alicePub = wc::x25519PublicKey(alicePriv);
    DOCTEST_CHECK_TRUE(alicePub.isOk());
    DOCTEST_CHECK_EQ(toHex(alicePub.value()),
                     std::string("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a"));

    const auto bobPub = wc::x25519PublicKey(bobPriv);
    DOCTEST_CHECK_TRUE(bobPub.isOk());
    DOCTEST_CHECK_EQ(toHex(bobPub.value()),
                     std::string("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f"));

    const auto ab = wc::x25519SharedSecret(alicePriv, bobPub.value());
    const auto ba = wc::x25519SharedSecret(bobPriv, alicePub.value());
    DOCTEST_CHECK_TRUE(ab.isOk() && ba.isOk());
    DOCTEST_CHECK_EQ(toHex(ab.value()),
                     std::string("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"));
    DOCTEST_CHECK_EQ(ab.value(), ba.value());
    return true;
});

// RFC 7748 §6.1：全零公钥（低阶点）必须返回错误，不能给出可预测密钥
REGISTER_TEST(web_crypto_x25519_rejects_low_order,
              "web_crypto: X25519 低阶公钥/非法尺寸拒绝", {
    const Bytes priv = hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    const auto lowOrder = wc::x25519SharedSecret(priv, Bytes(32, 0));
    DOCTEST_CHECK_TRUE(lowOrder.isFail());
    DOCTEST_CHECK_TRUE(wc::x25519SharedSecret(Bytes(31, 1), Bytes(32, 1)).isFail());
    DOCTEST_CHECK_TRUE(wc::x25519PublicKey(Bytes(16, 1)).isFail());
    const auto pair = wc::x25519Generate();
    DOCTEST_CHECK_TRUE(pair.isOk());
    DOCTEST_CHECK_EQ(pair.value().privateKey.size(), wc::kKeySize);
    DOCTEST_CHECK_EQ(pair.value().publicKey.size(), wc::kKeySize);
    return true;
});

// RFC 5869 A.1：基本向量（32B salt / 10B info / L=42）
REGISTER_TEST(web_crypto_hkdf_rfc5869_a1,
              "web_crypto: HKDF-SHA256 RFC 5869 A.1 的 PRK 与 OKM", {
    const Bytes ikm(22, 0x0b);
    const Bytes salt = hex("000102030405060708090a0b0c");
    const Bytes info = hex("f0f1f2f3f4f5f6f7f8f9");

    const auto prk = wc::hkdfExtract(salt, ikm);
    DOCTEST_CHECK_TRUE(prk.isOk());
    DOCTEST_CHECK_EQ(toHex(prk.value()),
                     std::string("077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5"));

    const auto okm = wc::hkdfExpand(prk.value(), infoOf(info), 42);
    DOCTEST_CHECK_TRUE(okm.isOk());
    DOCTEST_CHECK_EQ(toHex(okm.value()),
                     std::string("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
                                 "34007208d5b887185865"));
    return true;
});

// RFC 5869 A.2（80B 全量）+ A.3（空 salt/info）
REGISTER_TEST(web_crypto_hkdf_rfc5869_a2_a3,
              "web_crypto: HKDF-SHA256 RFC 5869 A.2/A.3", {
    const Bytes ikm = hex(
        "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
        "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f"
        "404142434445464748494a4b4c4d4e4f");
    const Bytes salt = hex(
        "606162636465666768696a6b6c6d6e6f707172737475767778797a7b7c7d7e7f"
        "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f"
        "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf");
    const Bytes info = hex(
        "b0b1b2b3b4b5b6b7b8b9babbbcbdbebfc0c1c2c3c4c5c6c7c8c9cacbcccdcecf"
        "d0d1d2d3d4d5d6d7d8d9dadbdcdddedfe0e1e2e3e4e5e6e7e8e9eaebecedeeef"
        "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff");

    const auto prk2 = wc::hkdfExtract(salt, ikm);
    DOCTEST_CHECK_TRUE(prk2.isOk());
    DOCTEST_CHECK_EQ(toHex(prk2.value()),
                     std::string("06a6b88c5853361a06104c9ceb35b45cef760014904671014a193f40c15fc244"));
    const auto okm2 = wc::hkdfExpand(prk2.value(), infoOf(info), 82);
    DOCTEST_CHECK_TRUE(okm2.isOk());
    DOCTEST_CHECK_EQ(toHex(okm2.value()),
                     std::string("b11e398dc80327a1c8e7f78c596a49344f012eda2d4efad8a050cc4c19afa97c"
                                 "59045a99cac7827271cb41c65e590e09da3275600c2f09b8367793a9aca3db71"
                                 "cc30c58179ec3e87c14c01d5c1f3434f1d87"));

    const auto prk3 = wc::hkdfExtract({}, Bytes(22, 0x0b));
    DOCTEST_CHECK_TRUE(prk3.isOk());
    DOCTEST_CHECK_EQ(toHex(prk3.value()),
                     std::string("19ef24a32c717b167f33a91d6f648bdf96596776afdb6377ac434c1c293ccb04"));
    const auto okm3 = wc::hkdfExpand(prk3.value(), std::string(), 42);
    DOCTEST_CHECK_TRUE(okm3.isOk());
    DOCTEST_CHECK_EQ(toHex(okm3.value()),
                     std::string("8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d"
                                 "9d201395faa4b61a96c8"));
    DOCTEST_CHECK_TRUE(wc::hkdfExpand(prk3.value(), std::string(), 255 * 32 + 1).isFail());
    return true;
});

// RFC 2202 case 1（HMAC-SHA1）与 RFC 4231 case 1（HMAC-SHA256）
REGISTER_TEST(web_crypto_hmac_rfc_vectors,
              "web_crypto: HMAC-SHA1/SHA256 官方向量", {
    const Bytes key(20, 0x0b);
    const Bytes message = text("Hi There");

    const auto sha1 = wc::hmacSha1(key, message);
    DOCTEST_CHECK_TRUE(sha1.isOk());
    DOCTEST_CHECK_EQ(toHex(sha1.value()),
                     std::string("b617318655057264e28bc0b6fb378c8ef146be00"));

    const auto sha256 = wc::hmacSha256(key, message);
    DOCTEST_CHECK_TRUE(sha256.isOk());
    DOCTEST_CHECK_EQ(toHex(sha256.value()),
                     std::string("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));
    return true;
});

// RFC 8439 §2.8.2（封装）与 A.5（解封）同一组参数，AAD 参与认证
REGISTER_TEST(web_crypto_chacha20poly1305_rfc8439,
              "web_crypto: ChaCha20-Poly1305 RFC 8439 §2.8.2 / A.5", {
    const Bytes key = hex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
    const Bytes nonce = hex("070000004041424344454647");
    const Bytes aad = hex("50515253c0c1c2c3c4c5c6c7");
    const std::string plainText =
        "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the "
        "future, sunscreen would be it.";
    const Bytes plaintext = text(plainText);
    const std::string expected =
        "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca96712"
        "82fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c9803aee328091b5"
        "8fab324e4fad675945585808b4831d7bc3ff4def08e4b7a9de576d26586cec64b6116"
        "1ae10b594f09e26a7e902ecbd0600691";

    const auto sealed = wc::aeadSeal(key, nonce, plaintext, aad);
    DOCTEST_CHECK_TRUE(sealed.isOk());
    DOCTEST_CHECK_EQ(toHex(sealed.value()), expected);

    const auto opened = wc::aeadOpen(key, nonce, hex(expected.c_str()), aad);
    DOCTEST_CHECK_TRUE(opened.isOk());
    DOCTEST_CHECK_EQ(opened.value(), plaintext);
    return true;
});

// 认证失败三类（篡改密文 / 篡改 tag / 错密钥 / 错 nonce）都必须报错，不得返回明文
REGISTER_TEST(web_crypto_aead_rejects_tamper_and_wrong_key,
              "web_crypto: AEAD 篡改/错密钥/错 nonce 拒绝", {
    const Bytes key(32, 0x42);
    const Bytes nonce = wc::makeNonce(wc::kDirectionC2S, 7);
    const Bytes plaintext = text("devdesk web terminal");

    const auto sealed = wc::aeadSeal(key, nonce, plaintext);
    DOCTEST_CHECK_TRUE(sealed.isOk());
    DOCTEST_CHECK_EQ(sealed.value().size(), plaintext.size() + wc::kTagSize);
    DOCTEST_CHECK_EQ(wc::aeadOpen(key, nonce, sealed.value()).value(), plaintext);

    Bytes tamperedCipher = sealed.value();
    tamperedCipher[0] ^= 0x01;
    DOCTEST_CHECK_TRUE(wc::aeadOpen(key, nonce, tamperedCipher).isFail());

    Bytes tamperedTag = sealed.value();
    tamperedTag.back() ^= 0x80;
    DOCTEST_CHECK_TRUE(wc::aeadOpen(key, nonce, tamperedTag).isFail());

    const Bytes wrongKey(32, 0x43);
    DOCTEST_CHECK_TRUE(wc::aeadOpen(wrongKey, nonce, sealed.value()).isFail());
    DOCTEST_CHECK_TRUE(
        wc::aeadOpen(key, wc::makeNonce(wc::kDirectionC2S, 8), sealed.value()).isFail());
    DOCTEST_CHECK_TRUE(wc::aeadOpen(key, nonce, Bytes(15, 0)).isFail());
    return true;
});

// nonce 方向隔离：c2s 密文用 s2c nonce 解不开；布局 = 方向 4B ‖ 计数器 8B 大端
REGISTER_TEST(web_crypto_nonce_direction_isolation,
              "web_crypto: nonce 方向标签隔离与布局", {
    const Bytes c2s = wc::makeNonce(wc::kDirectionC2S, 0x0102030405060708ULL);
    DOCTEST_CHECK_EQ(c2s.size(), wc::kNonceSize);
    DOCTEST_CHECK_EQ(toHex(c2s), std::string("000000010102030405060708"));

    const Bytes key(32, 0x11);
    const auto sealed = wc::aeadSeal(key, c2s, text("x"));
    DOCTEST_CHECK_TRUE(sealed.isOk());
    const Bytes s2c = wc::makeNonce(wc::kDirectionS2C, 0x0102030405060708ULL);
    DOCTEST_CHECK_TRUE(wc::aeadOpen(key, s2c, sealed.value()).isFail());
    DOCTEST_CHECK_TRUE(wc::aeadOpen(key, c2s, sealed.value()).isOk());
    return true;
});

// 信封固定开销 29B（版本 1 + nonce 12 + tag 16）；解封回填 nonce
REGISTER_TEST(web_crypto_envelope_overhead_29,
              "web_crypto: AEAD 信封开销恒 29B 且回填 nonce", {
    const Bytes key(32, 0x23);
    const Bytes nonce = wc::makeNonce(wc::kDirectionS2C, 3);
    for (const std::size_t size : {std::size_t{0}, std::size_t{1}, std::size_t{50}, std::size_t{4096}}) {
        const Bytes plaintext(size, 0x5A);
        const auto envelope = wc::sealEnvelope(key, nonce, plaintext);
        DOCTEST_CHECK_TRUE(envelope.isOk());
        DOCTEST_CHECK_EQ(envelope.value().size(), plaintext.size() + 29);
        DOCTEST_CHECK_EQ(envelope.value().size() - plaintext.size(), wc::kEnvelopeOverhead);
        DOCTEST_CHECK_EQ(static_cast<int>(envelope.value()[0]), 1);

        Bytes seenNonce;
        const auto opened = wc::openEnvelope(key, envelope.value(), {}, &seenNonce);
        DOCTEST_CHECK_TRUE(opened.isOk());
        DOCTEST_CHECK_EQ(opened.value(), plaintext);
        DOCTEST_CHECK_EQ(seenNonce, nonce);
    }
    return true;
});

// 信封版本/长度/篡改拒绝（版本位退化或跳过 tag 校验必红）
REGISTER_TEST(web_crypto_envelope_rejects_bad_version,
              "web_crypto: 信封版本与篡改拒绝", {
    const Bytes key(32, 0x24);
    const Bytes nonce = wc::makeNonce(wc::kDirectionC2S, 1);
    const auto envelope = wc::sealEnvelope(key, nonce, text("payload"));
    DOCTEST_CHECK_TRUE(envelope.isOk());

    Bytes versionBroken = envelope.value();
    versionBroken[0] = 2;
    DOCTEST_CHECK_TRUE(wc::openEnvelope(key, versionBroken).isFail());

    Bytes tampered = envelope.value();
    tampered.back() ^= 0x01;
    DOCTEST_CHECK_TRUE(wc::openEnvelope(key, tampered).isFail());

    DOCTEST_CHECK_TRUE(wc::openEnvelope(key, Bytes(28, 0)).isFail());
    return true;
});

// RFC 4648 §5 / §10：base64url 无填充，含 -_ 字母表
REGISTER_TEST(web_crypto_base64url,
              "web_crypto: base64url 向量与往返", {
    DOCTEST_CHECK_EQ(wc::base64UrlEncode(text("foobar")), std::string("Zm9vYmFy"));
    DOCTEST_CHECK_EQ(wc::base64UrlEncode(Bytes{0xFB, 0xFF}), std::string("-_8"));

    for (std::size_t size = 0; size <= 40; ++size) {
        Bytes data(size);
        for (std::size_t i = 0; i < size; ++i) data[i] = static_cast<std::uint8_t>(i * 7 + 3);
        const auto decoded = wc::base64UrlDecode(wc::base64UrlEncode(data));
        DOCTEST_CHECK_TRUE(decoded.isOk());
        DOCTEST_CHECK_EQ(decoded.value(), data);
    }
    DOCTEST_CHECK_TRUE(wc::base64UrlDecode("A").isFail());
    DOCTEST_CHECK_TRUE(wc::base64UrlDecode("+/8").isFail());
    DOCTEST_CHECK_TRUE(wc::base64UrlDecode("AA A").isFail());
    return true;
});

// RFC 4648 §6 / §10：base32 大写无填充，TOTP 密钥载体
REGISTER_TEST(web_crypto_base32,
              "web_crypto: base32 向量与往返（容忍填充/小写）", {
    DOCTEST_CHECK_EQ(wc::base32Encode(text("foobar")), std::string("MZXW6YTBOI"));

    const auto decoded = wc::base32Decode("MZXW6YTBOI======");
    DOCTEST_CHECK_TRUE(decoded.isOk());
    DOCTEST_CHECK_EQ(decoded.value(), text("foobar"));
    DOCTEST_CHECK_EQ(wc::base32Decode("mzxw6ytboi").value(), text("foobar"));

    for (std::size_t size = 0; size <= 32; ++size) {
        Bytes data(size);
        for (std::size_t i = 0; i < size; ++i) data[i] = static_cast<std::uint8_t>(i * 11 + 5);
        const auto roundTrip = wc::base32Decode(wc::base32Encode(data));
        DOCTEST_CHECK_TRUE(roundTrip.isOk());
        DOCTEST_CHECK_EQ(roundTrip.value(), data);
    }
    DOCTEST_CHECK_TRUE(wc::base32Decode("MZXW1").isFail());
    DOCTEST_CHECK_TRUE(wc::base32Decode("MZXW=6YT").isFail());
    return true;
});

// 定长比较 + 随机数基本契约
REGISTER_TEST(web_crypto_constant_time_equals_and_random,
              "web_crypto: constantTimeEquals 与 randomBytes", {
    DOCTEST_CHECK_TRUE(wc::constantTimeEquals(std::string("abc"), std::string("abc")));
    DOCTEST_CHECK_FALSE(wc::constantTimeEquals(std::string("abc"), std::string("abd")));
    DOCTEST_CHECK_FALSE(wc::constantTimeEquals(std::string("abc"), std::string("abcd")));
    const std::uint8_t a[3] = {1, 2, 3};
    const std::uint8_t b[3] = {1, 2, 3};
    const std::uint8_t c[3] = {1, 2, 4};
    DOCTEST_CHECK_TRUE(wc::constantTimeEquals(a, b, 3));
    DOCTEST_CHECK_FALSE(wc::constantTimeEquals(a, c, 3));

    const Bytes random = wc::randomBytes(32);
    DOCTEST_CHECK_EQ(random.size(), std::size_t{32});
    bool nonZero = false;
    for (const std::uint8_t byte : random) nonZero = nonZero || byte != 0;
    DOCTEST_CHECK_TRUE(nonZero);
    DOCTEST_CHECK_TRUE(wc::randomBytes(0).empty());
    return true;
});
