// src/platform/credential_protection_win.cpp
// Windows：DPAPI 加密后的 blob **hex 编码**进列，不是明文
//（设计 §6.2：「不用 Keychain」≠「明文落盘」）
//
// DPAPI 调用形态与 keychain_win.cpp:90-178 一字同款（CryptProtectData /
// CryptUnprotectData + CRYPTPROTECT_UI_FORBIDDEN，用户级、禁止弹 UI）；区别只在
// 落点：那里是 %APPDATA%/DevDesk/keychain/**/*.key 文件，这里是
// claude_profiles.api_key_encrypted 文本列，故选 hex 编码（列类型 TEXT，
// 二进制直接入库会被截断 / 编码歧义）。列里存的一律是密文。
#include "credential_protection.h"

#include <cstddef>
#include <limits>
#include <string>

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

// hex 编解码（小写输出；解码大小写都收）。自实现而非引 base64：DPAPI blob 全字节
// 随机，hex 体积 2 倍对几百字节无感，换来零依赖与肉眼可核。
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
    // DATA_BLOB::cbData 是 DWORD（32 位），std::string::size() 是 size_t
    //（Win64 上 64 位）——窄化会**静默截断**，把「输入超长」变成「一条被截短的
    // 密钥」而不是一次失败，且 DPAPI 照样加密成功、用的时候才解不出原值。
    // API 密钥只有几十字节，这条分支理论不可达；但按本批纪律（凡是需要保证的
    // 东西用代码保证，不许用注释保证），显式判一次，别写一句「反正不会发生」。
    // 用 numeric_limits 而不是 MAXDWORD 宏：后者是 windows.h 里的宏，本文件
    // 未在本机编译验证过（无 MinGW 交叉链），少一个外部符号依赖少一分风险。
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
        // 形态不认识就说出来（设计 §7 教训③：不静默回落空串——那会把
        // 「密钥不可用」伪装成「没配过密钥」）
        return core::Result<std::string>::fail(
            core::ErrorCode::Internal,
            "凭据列形态损坏（DPAPI blob 的 hex 形态不成立）");
    }

    // 同 protect 侧：cbData 是 DWORD，blob.size() 是 size_t —— 窄化前显式判一次。
    // 正常路径下 blob 是库里 hex 列解出来的（远小于 4 GB），但「形态损坏」这条
    // 分支正是要防的东西，不该在这里留一个静默截断的口子。
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
    // 与实现一致：列里是 DPAPI 密文（同机同用户可解），不是明文
    return "本机数据库存储（DPAPI 加密，仅本机本用户可解）";
}

} // namespace platform::credential
