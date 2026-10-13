// src/platform/credential_protection_win.cpp
// Windows：DPAPI 密文 hex 编码进文本列（TEXT 列直接存二进制会被截断/编码歧义），列里一律是密文。
// CryptProtectData/CryptUnprotectData + CRYPTPROTECT_UI_FORBIDDEN，用户级、禁弹 UI。
#include "credential_protection.h"

#include <cstddef>
#include <limits>
#include <string>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>

namespace platform::credential {
namespace {

// hex 编解码（小写输出，解码大小写都收）。
std::string hexEncode(const std::string& raw) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(raw.size() * 2);
    for (char ch : raw) {
        const unsigned char c = static_cast<unsigned char>(ch);
        out += kHex[c >> 4];
        out += kHex[c & 0x0F];
    }
    return out;
}

bool hexDecode(const std::string& hex, std::string& out) {
    if (hex.empty() || hex.size() % 2 != 0) return false;
    out.clear();
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        unsigned char v[2] = {0, 0};
        for (int k = 0; k < 2; ++k) {
            const char ch = hex[i + static_cast<std::size_t>(k)];
            if (ch >= '0' && ch <= '9') {
                v[k] = static_cast<unsigned char>(ch - '0');
            } else if (ch >= 'a' && ch <= 'f') {
                v[k] = static_cast<unsigned char>(ch - 'a' + 10);
            } else if (ch >= 'A' && ch <= 'F') {
                v[k] = static_cast<unsigned char>(ch - 'A' + 10);
            } else {
                return false;
            }
        }
        out += static_cast<char>((v[0] << 4) | v[1]);
    }
    return true;
}

std::string dpapiError(const char* what) {
    return std::string(what) + " failed: GetLastError=" +
           std::to_string(GetLastError());
}

} // namespace

core::Result<std::string> realProtect(const std::string& plaintext) {
    // cbData 是 DWORD：size_t 窄化超限会静默截断成短密钥而非失败，显式判一次。
    if (plaintext.size() >
        static_cast<std::size_t>(std::numeric_limits<DWORD>::max())) {
        return core::Result<std::string>::fail(
            core::ErrorCode::Internal,
            "待加密凭据超过 DPAPI 单块长度上限（DWORD 32 位）");
    }
    DATA_BLOB input{};
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.data()));
    input.cbData = static_cast<DWORD>(plaintext.size());

    DATA_BLOB output{};
    if (!CryptProtectData(&input, nullptr, nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        return core::Result<std::string>::fail(core::ErrorCode::Internal,
                                               dpapiError("CryptProtectData"));
    }
    const std::string blob(reinterpret_cast<const char*>(output.pbData),
                           output.cbData);
    LocalFree(output.pbData);
    return core::Result<std::string>::ok(hexEncode(blob));
}

core::Result<std::string> realUnprotect(const std::string& stored) {
    std::string blob;
    if (!hexDecode(stored, blob) || blob.empty()) {
        // 形态不认识就报错：不静默回落空串（否则「密钥不可用」伪装成「没配过密钥」）
        return core::Result<std::string>::fail(
            core::ErrorCode::Internal,
            "凭据列形态损坏（DPAPI blob 的 hex 形态不成立）");
    }

    // 同上：size_t 窄化 DWORD 前显式判一次，不留截断口
    if (blob.size() > static_cast<std::size_t>(std::numeric_limits<DWORD>::max())) {
        return core::Result<std::string>::fail(
            core::ErrorCode::Internal,
            "凭据列解出的 blob 超过 DPAPI 单块长度上限（DWORD 32 位）");
    }
    DATA_BLOB input{};
    input.pbData = reinterpret_cast<BYTE*>(blob.data());
    input.cbData = static_cast<DWORD>(blob.size());

    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        return core::Result<std::string>::fail(core::ErrorCode::Internal,
                                               dpapiError("CryptUnprotectData"));
    }
    std::string plain(reinterpret_cast<char*>(output.pbData), output.cbData);
    LocalFree(output.pbData);
    return core::Result<std::string>::ok(std::move(plain));
}

bool realStoresPlaintext() { return false; }

const char* realProtectionDescription() {
    return "本机数据库存储（DPAPI 加密，仅本机本用户可解）";
}

} // namespace platform::credential
