// 网页终端加密原语（need#490 W1）：套件 X25519（RFC 7748）/ HKDF-SHA256（RFC 5869）/
// ChaCha20-Poly1305（RFC 8439），前后端按 RFC 向量字节级互通。实现走 OpenSSL EVP/HMAC，
// 无平台分支（两端共用一份），故放共享源而非 *_posix/_win。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/result.h"

namespace platform::web_crypto {

inline constexpr std::size_t kKeySize = 32;
inline constexpr std::size_t kNonceSize = 12;
inline constexpr std::size_t kTagSize = 16;
inline constexpr std::uint8_t kEnvelopeVersion = 1;
// 信封 = 版本 1B ‖ nonce 12B ‖ 密文 ‖ tag 16B；固定开销 29B/帧（技术卡 §6.3）
inline constexpr std::size_t kEnvelopeOverhead = 1 + kNonceSize + kTagSize;

inline constexpr std::uint8_t kDirectionC2S = 1;  // 客户端 → 服务端
inline constexpr std::uint8_t kDirectionS2C = 2;  // 服务端 → 客户端

// nonce = 方向标签 4B（大端）‖ 计数器 8B（大端）；计数器只在重新握手时归零，禁止跨握手复用
std::vector<std::uint8_t> makeNonce(std::uint8_t direction, std::uint64_t counter);

// 空返回 = RNG 失败（不可恢复，调用方按启动失败处理）
std::vector<std::uint8_t> randomBytes(std::size_t n);
bool constantTimeEquals(const std::uint8_t* a, const std::uint8_t* b, std::size_t n);
bool constantTimeEquals(const std::string& a, const std::string& b);

core::Result<std::vector<std::uint8_t>> hmacSha1(const std::vector<std::uint8_t>& key,
                                                 const std::vector<std::uint8_t>& message);
core::Result<std::vector<std::uint8_t>> hmacSha256(const std::vector<std::uint8_t>& key,
                                                   const std::vector<std::uint8_t>& message);

// RFC 5869：PRK = Extract(salt, ikm)；OKM = Expand(prk, info, len)。info 为字节串。
// 选择 HMAC 组合而非 EVP_PKEY_HKDF：extract/expand 两步都要单独暴露（服务端按方向
// 复用同一 PRK），单次 API 反而要拆两次上下文；组合式与 RFC 伪代码逐行对应，向量可验。
core::Result<std::vector<std::uint8_t>> hkdfExtract(const std::vector<std::uint8_t>& salt,
                                                    const std::vector<std::uint8_t>& ikm);
core::Result<std::vector<std::uint8_t>> hkdfExpand(const std::vector<std::uint8_t>& prk,
                                                    const std::string& info,
                                                    std::size_t outLen);

struct X25519KeyPair {
    std::vector<std::uint8_t> privateKey;  // 32B
    std::vector<std::uint8_t> publicKey;   // 32B
};

core::Result<X25519KeyPair> x25519Generate();
core::Result<std::vector<std::uint8_t>> x25519PublicKey(const std::vector<std::uint8_t>& privateKey);
// 全零共享密钥（低阶公钥）返回错误，不向上返回可预测密钥
core::Result<std::vector<std::uint8_t>> x25519SharedSecret(
    const std::vector<std::uint8_t>& privateKey, const std::vector<std::uint8_t>& peerPublicKey);

// 产出 = 密文 ‖ tag（open 输入同形）。失败 = 尺寸非法 / 认证失败（篡改、错密钥、错 nonce 同码）
core::Result<std::vector<std::uint8_t>> aeadSeal(const std::vector<std::uint8_t>& key,
                                                 const std::vector<std::uint8_t>& nonce,
                                                 const std::vector<std::uint8_t>& plaintext,
                                                 const std::vector<std::uint8_t>& aad = {});
core::Result<std::vector<std::uint8_t>> aeadOpen(const std::vector<std::uint8_t>& key,
                                                 const std::vector<std::uint8_t>& nonce,
                                                 const std::vector<std::uint8_t>& sealed,
                                                 const std::vector<std::uint8_t>& aad = {});

core::Result<std::vector<std::uint8_t>> sealEnvelope(const std::vector<std::uint8_t>& key,
                                                     const std::vector<std::uint8_t>& nonce,
                                                     const std::vector<std::uint8_t>& plaintext,
                                                     const std::vector<std::uint8_t>& aad = {});
// nonceOut 非空 = 回填帧内 nonce（重放窗口记账用）
core::Result<std::vector<std::uint8_t>> openEnvelope(const std::vector<std::uint8_t>& key,
                                                     const std::vector<std::uint8_t>& envelope,
                                                     const std::vector<std::uint8_t>& aad = {},
                                                     std::vector<std::uint8_t>* nonceOut = nullptr);

// RFC 4648：base64url（§5，无填充）/ base32（§6，大写无填充，TOTP 用）
std::string base64UrlEncode(const std::vector<std::uint8_t>& data);
core::Result<std::vector<std::uint8_t>> base64UrlDecode(const std::string& text);
std::string base32Encode(const std::vector<std::uint8_t>& data);
core::Result<std::vector<std::uint8_t>> base32Decode(const std::string& text);

} // namespace platform::web_crypto
