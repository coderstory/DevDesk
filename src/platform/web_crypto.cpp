// 网页终端加密原语实现（need#490 W1）：OpenSSL EVP/HMAC。
// 约定：失败一律返回 Result 错误态（不抛、不返回半成品），认证失败不区分「密钥错 /
// 篡改 / nonce 错」——HTTP 层统一映射 400 且不回显原因。
#include "platform/web_crypto.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace platform::web_crypto {
namespace {

using Bytes = std::vector<std::uint8_t>;

core::Result<Bytes> fail(std::string message) {
    return core::Result<Bytes>::fail(core::ErrorCode::ValidationError, std::move(message));
}

// HMAC 单次 API（非 EVP_MAC）：OpenSSL 1.1 / 3.x 通用，TOTP 的 SHA-1 与 HKDF 的 SHA-256 共用
core::Result<Bytes> hmac(const EVP_MD* md, const Bytes& key, const Bytes& message) {
    unsigned char mac[EVP_MAX_MD_SIZE] = {};
    unsigned int length = 0;
    const unsigned char* keyData = key.empty() ? nullptr : key.data();
    if (HMAC(md, keyData, static_cast<int>(key.size()),
             message.empty() ? nullptr : message.data(), message.size(), mac, &length) == nullptr) {
        return fail("HMAC 计算失败");
    }
    return core::Result<Bytes>::ok(Bytes(mac, mac + length));
}

} // namespace

std::vector<std::uint8_t> makeNonce(std::uint8_t direction, std::uint64_t counter) {
    Bytes nonce(kNonceSize, 0);
    nonce[0] = 0;
    nonce[1] = 0;
    nonce[2] = 0;
    nonce[3] = direction;
    for (int i = 0; i < 8; ++i) {
        nonce[4 + i] = static_cast<std::uint8_t>((counter >> (56 - 8 * i)) & 0xFF);
    }
    return nonce;
}

std::vector<std::uint8_t> randomBytes(std::size_t n) {
    Bytes out(n);
    if (n == 0) return out;
    if (RAND_bytes(out.data(), static_cast<int>(n)) != 1) return {};
    return out;
}

bool constantTimeEquals(const std::uint8_t* a, const std::uint8_t* b, std::size_t n) {
    if (a == nullptr || b == nullptr) return n == 0;
    return CRYPTO_memcmp(a, b, n) == 0;
}

bool constantTimeEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;  // 长度是公开信息
    return constantTimeEquals(reinterpret_cast<const std::uint8_t*>(a.data()),
                              reinterpret_cast<const std::uint8_t*>(b.data()), a.size());
}

core::Result<std::vector<std::uint8_t>> hmacSha1(const std::vector<std::uint8_t>& key,
                                                 const std::vector<std::uint8_t>& message) {
    return hmac(EVP_sha1(), key, message);
}

core::Result<std::vector<std::uint8_t>> hmacSha256(const std::vector<std::uint8_t>& key,
                                                   const std::vector<std::uint8_t>& message) {
    return hmac(EVP_sha256(), key, message);
}

core::Result<std::vector<std::uint8_t>> hkdfExtract(const std::vector<std::uint8_t>& salt,
                                                    const std::vector<std::uint8_t>& ikm) {
    // RFC 5869 §2.2：salt 缺省 = HashLen 个 0；HMAC 的零填充密钥与空密钥等价，故空 salt
    // 直接走空 key，向量 A.3 即此路径
    return hmacSha256(salt, ikm);
}

core::Result<std::vector<std::uint8_t>> hkdfExpand(const std::vector<std::uint8_t>& prk,
                                                    const std::string& info,
                                                    std::size_t outLen) {
    constexpr std::size_t kHashLen = 32;
    if (outLen > 255 * kHashLen) return fail("HKDF 输出超过 255×HashLen");
    Bytes okm;
    okm.reserve(outLen);
    Bytes block;
    for (std::uint8_t counter = 1; okm.size() < outLen; ++counter) {
        Bytes input = block;
        input.insert(input.end(), info.begin(), info.end());
        input.push_back(counter);
        auto mac = hmacSha256(prk, input);
        if (mac.isFail()) return mac;
        block = mac.value();
        const std::size_t take = std::min(block.size(), outLen - okm.size());
        okm.insert(okm.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(take));
    }
    return core::Result<Bytes>::ok(std::move(okm));
}

core::Result<X25519KeyPair> x25519Generate() {
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
    if (ctx == nullptr) return core::Result<X25519KeyPair>::fail(core::ErrorCode::Internal, "X25519 ctx 创建失败");
    EVP_PKEY* pkey = nullptr;
    bool ok = EVP_PKEY_keygen_init(ctx) == 1 && EVP_PKEY_keygen(ctx, &pkey) == 1;
    EVP_PKEY_CTX_free(ctx);
    if (!ok || pkey == nullptr) {
        EVP_PKEY_free(pkey);
        return core::Result<X25519KeyPair>::fail(core::ErrorCode::Internal, "X25519 密钥生成失败");
    }

    X25519KeyPair pair;
    pair.privateKey.resize(kKeySize);
    pair.publicKey.resize(kKeySize);
    std::size_t privateLen = kKeySize;
    std::size_t publicLen = kKeySize;
    ok = EVP_PKEY_get_raw_private_key(pkey, pair.privateKey.data(), &privateLen) == 1 &&
         EVP_PKEY_get_raw_public_key(pkey, pair.publicKey.data(), &publicLen) == 1;
    EVP_PKEY_free(pkey);
    if (!ok) return core::Result<X25519KeyPair>::fail(core::ErrorCode::Internal, "X25519 原始密钥导出失败");
    pair.privateKey.resize(privateLen);
    pair.publicKey.resize(publicLen);
    return core::Result<X25519KeyPair>::ok(std::move(pair));
}

core::Result<std::vector<std::uint8_t>> x25519PublicKey(
    const std::vector<std::uint8_t>& privateKey) {
    if (privateKey.size() != kKeySize) return fail("X25519 私钥必须 32B");
    EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr,
                                                  privateKey.data(), privateKey.size());
    if (pkey == nullptr) return fail("X25519 私钥载入失败");
    Bytes pub(kKeySize);
    std::size_t pubLen = kKeySize;
    const bool ok = EVP_PKEY_get_raw_public_key(pkey, pub.data(), &pubLen) == 1;
    EVP_PKEY_free(pkey);
    if (!ok) return fail("X25519 公钥导出失败");
    pub.resize(pubLen);
    return core::Result<Bytes>::ok(std::move(pub));
}

core::Result<std::vector<std::uint8_t>> x25519SharedSecret(
    const std::vector<std::uint8_t>& privateKey, const std::vector<std::uint8_t>& peerPublicKey) {
    if (privateKey.size() != kKeySize || peerPublicKey.size() != kKeySize) {
        return fail("X25519 密钥必须 32B");
    }
    EVP_PKEY* own = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr,
                                                 privateKey.data(), privateKey.size());
    EVP_PKEY* peer = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr,
                                                 peerPublicKey.data(), peerPublicKey.size());
    if (own == nullptr || peer == nullptr) {
        EVP_PKEY_free(own);
        EVP_PKEY_free(peer);
        return fail("X25519 密钥载入失败");
    }

    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(own, nullptr);
    bool ok = ctx != nullptr && EVP_PKEY_derive_init(ctx) == 1 &&
              EVP_PKEY_derive_set_peer(ctx, peer) == 1;
    Bytes shared(kKeySize);
    std::size_t sharedLen = kKeySize;
    ok = ok && EVP_PKEY_derive(ctx, shared.data(), &sharedLen) == 1;
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(own);
    EVP_PKEY_free(peer);
    if (!ok) return fail("X25519 共享密钥推导失败");
    shared.resize(sharedLen);

    // RFC 7748 §6.1：低阶公钥产出全零密钥必须拒绝
    const bool allZero = std::all_of(shared.begin(), shared.end(),
                                     [](std::uint8_t b) { return b == 0; });
    if (shared.empty() || allZero) return fail("X25519 共享密钥为全零（低阶公钥）");
    return core::Result<Bytes>::ok(std::move(shared));
}

core::Result<std::vector<std::uint8_t>> aeadSeal(const std::vector<std::uint8_t>& key,
                                                 const std::vector<std::uint8_t>& nonce,
                                                 const std::vector<std::uint8_t>& plaintext,
                                                 const std::vector<std::uint8_t>& aad) {
    if (key.size() != kKeySize) return fail("AEAD 密钥必须 32B");
    if (nonce.size() != kNonceSize) return fail("AEAD nonce 必须 12B");

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) return fail("AEAD ctx 创建失败");
    Bytes out(plaintext.size() + kTagSize);
    int len = 0;
    bool ok = EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN,
                                  static_cast<int>(kNonceSize), nullptr) == 1 &&
              EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce.data()) == 1;
    if (ok && !aad.empty()) {
        ok = EVP_EncryptUpdate(ctx, nullptr, &len, aad.data(), static_cast<int>(aad.size())) == 1;
    }
    if (ok && !plaintext.empty()) {
        ok = EVP_EncryptUpdate(ctx, out.data(), &len, plaintext.data(),
                               static_cast<int>(plaintext.size())) == 1;
    }
    int finalLen = 0;
    ok = ok && EVP_EncryptFinal_ex(ctx, out.data() + len, &finalLen) == 1;
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, static_cast<int>(kTagSize),
                                   out.data() + plaintext.size()) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return fail("AEAD 封装失败");
    return core::Result<Bytes>::ok(std::move(out));
}

core::Result<std::vector<std::uint8_t>> aeadOpen(const std::vector<std::uint8_t>& key,
                                                 const std::vector<std::uint8_t>& nonce,
                                                 const std::vector<std::uint8_t>& sealed,
                                                 const std::vector<std::uint8_t>& aad) {
    if (key.size() != kKeySize) return fail("AEAD 密钥必须 32B");
    if (nonce.size() != kNonceSize) return fail("AEAD nonce 必须 12B");
    if (sealed.size() < kTagSize) return fail("AEAD 密文短于 tag");

    const std::size_t cipherLen = sealed.size() - kTagSize;
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) return fail("AEAD ctx 创建失败");
    Bytes out(cipherLen);
    int len = 0;
    bool ok = EVP_DecryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN,
                                  static_cast<int>(kNonceSize), nullptr) == 1 &&
              EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce.data()) == 1;
    if (ok && !aad.empty()) {
        ok = EVP_DecryptUpdate(ctx, nullptr, &len, aad.data(), static_cast<int>(aad.size())) == 1;
    }
    if (ok && cipherLen > 0) {
        ok = EVP_DecryptUpdate(ctx, out.data(), &len, sealed.data(),
                               static_cast<int>(cipherLen)) == 1;
    }
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, static_cast<int>(kTagSize),
                                   const_cast<std::uint8_t*>(sealed.data() + cipherLen)) == 1;
    int finalLen = 0;
    ok = ok && EVP_DecryptFinal_ex(ctx, out.data() + len, &finalLen) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return fail("AEAD 认证失败");
    out.resize(cipherLen);
    return core::Result<Bytes>::ok(std::move(out));
}

core::Result<std::vector<std::uint8_t>> sealEnvelope(const std::vector<std::uint8_t>& key,
                                                     const std::vector<std::uint8_t>& nonce,
                                                     const std::vector<std::uint8_t>& plaintext,
                                                     const std::vector<std::uint8_t>& aad) {
    auto sealed = aeadSeal(key, nonce, plaintext, aad);
    if (sealed.isFail()) return sealed;
    Bytes envelope;
    envelope.reserve(kEnvelopeOverhead + plaintext.size());
    envelope.push_back(kEnvelopeVersion);
    envelope.insert(envelope.end(), nonce.begin(), nonce.end());
    envelope.insert(envelope.end(), sealed.value().begin(), sealed.value().end());
    return core::Result<Bytes>::ok(std::move(envelope));
}

core::Result<std::vector<std::uint8_t>> openEnvelope(const std::vector<std::uint8_t>& key,
                                                     const std::vector<std::uint8_t>& envelope,
                                                     const std::vector<std::uint8_t>& aad,
                                                     std::vector<std::uint8_t>* nonceOut) {
    if (envelope.size() < kEnvelopeOverhead) return fail("信封短于固定开销");
    if (envelope[0] != kEnvelopeVersion) return fail("信封版本不支持");
    Bytes nonce(envelope.begin() + 1, envelope.begin() + 1 + kNonceSize);
    Bytes sealed(envelope.begin() + 1 + kNonceSize, envelope.end());
    auto plaintext = aeadOpen(key, nonce, sealed, aad);
    if (plaintext.isOk() && nonceOut != nullptr) *nonceOut = nonce;
    return plaintext;
}

namespace {

constexpr char kBase64Url[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
constexpr char kBase32[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

int base64UrlValue(char ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '-') return 62;
    if (ch == '_') return 63;
    return -1;
}

int base32Value(char ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a';
    if (ch >= '2' && ch <= '7') return ch - '2' + 26;
    return -1;
}

} // namespace

std::string base64UrlEncode(const std::vector<std::uint8_t>& data) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= data.size(); i += 3) {
        const std::uint32_t v = (static_cast<std::uint32_t>(data[i]) << 16) |
                                (static_cast<std::uint32_t>(data[i + 1]) << 8) |
                                static_cast<std::uint32_t>(data[i + 2]);
        out.push_back(kBase64Url[(v >> 18) & 0x3F]);
        out.push_back(kBase64Url[(v >> 12) & 0x3F]);
        out.push_back(kBase64Url[(v >> 6) & 0x3F]);
        out.push_back(kBase64Url[v & 0x3F]);
    }
    const std::size_t rest = data.size() - i;
    if (rest == 1) {
        const std::uint32_t v = static_cast<std::uint32_t>(data[i]) << 16;
        out.push_back(kBase64Url[(v >> 18) & 0x3F]);
        out.push_back(kBase64Url[(v >> 12) & 0x3F]);
    } else if (rest == 2) {
        const std::uint32_t v = (static_cast<std::uint32_t>(data[i]) << 16) |
                                (static_cast<std::uint32_t>(data[i + 1]) << 8);
        out.push_back(kBase64Url[(v >> 18) & 0x3F]);
        out.push_back(kBase64Url[(v >> 12) & 0x3F]);
        out.push_back(kBase64Url[(v >> 6) & 0x3F]);
    }
    return out;
}

core::Result<std::vector<std::uint8_t>> base64UrlDecode(const std::string& text) {
    if (text.size() % 4 == 1) return fail("base64url 长度非法");
    Bytes out;
    out.reserve(text.size() / 4 * 3);
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const char ch : text) {
        const int value = base64UrlValue(ch);
        if (value < 0) return fail("base64url 含非法字符");
        buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xFF));
        }
    }
    // 剩余位只能是编码补位（2 或 4 位），且必须为零
    if (bits >= 6 || (buffer & ((1u << bits) - 1)) != 0) return fail("base64url 尾位非零");
    return core::Result<Bytes>::ok(std::move(out));
}

std::string base32Encode(const std::vector<std::uint8_t>& data) {
    std::string out;
    out.reserve((data.size() * 8 + 4) / 5);
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const std::uint8_t byte : data) {
        buffer = (buffer << 8) | byte;
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            out.push_back(kBase32[(buffer >> bits) & 0x1F]);
        }
    }
    if (bits > 0) out.push_back(kBase32[(buffer << (5 - bits)) & 0x1F]);
    return out;
}

core::Result<std::vector<std::uint8_t>> base32Decode(const std::string& text) {
    Bytes out;
    out.reserve(text.size() * 5 / 8);
    std::uint32_t buffer = 0;
    int bits = 0;
    bool paddingSeen = false;
    for (const char ch : text) {
        if (ch == '=') {
            paddingSeen = true;
            continue;
        }
        if (paddingSeen) return fail("base32 填充后仍有字符");
        const int value = base32Value(ch);
        if (value < 0) return fail("base32 含非法字符");
        buffer = (buffer << 5) | static_cast<std::uint32_t>(value);
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xFF));
        }
    }
    if (bits >= 5 || (buffer & ((1u << bits) - 1)) != 0) return fail("base32 尾位非零");
    return core::Result<Bytes>::ok(std::move(out));
}

} // namespace platform::web_crypto
