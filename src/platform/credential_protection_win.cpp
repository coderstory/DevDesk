// src/platform/credential_protection_win.cpp
// Windows：与 POSIX 同口径——明文进列，保密靠文件权限（数据根在 %USERPROFILE% 下，
// NTFS 默认 ACL 仅本用户 + Administrators/SYSTEM，见 core/file_perms.h 的 no-op 说明）。
// 历史 DPAPI 加密已整体移除（2026-11-03 老板拍板）：旧库里的 dpapi: 密文不再支持解密，
// 读取时明确报错要求重填，不做兼容。
#include "credential_protection.h"

namespace platform::credential {

core::Result<std::string> realProtect(const std::string& plaintext) {
    // 本平台不做内容级加密；若改成明文之外的形态，必须同步改 realStoresPlaintext()。
    return core::Result<std::string>::ok(plaintext);
}

core::Result<std::string> realUnprotect(const std::string& stored) {
    // 旧版 Windows DPAPI 密文：已不再支持本机解密，明确报错让用户重填，
    // 不静默回落空串（否则「密钥不可用」伪装成「没配过密钥」）。
    if (isDpapiBlob(stored)) {
        return core::Result<std::string>::fail(
            core::ErrorCode::Internal,
            "该 Key 是旧版 Windows DPAPI 密文，已不再支持本机解密；请重新填写 API Key");
    }
    return core::Result<std::string>::ok(stored);
}

bool realStoresPlaintext() { return true; }

const char* realProtectionDescription() {
    return "本机数据库存储（NTFS 权限仅本用户可读）";
}

} // namespace platform::credential
