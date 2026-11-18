// src/platform/credential_protection_win.cpp
// Windows：与 POSIX 同口径——明文进列，保密靠文件权限（数据根在 %USERPROFILE% 下，
// NTFS 默认 ACL 仅本用户 + Administrators/SYSTEM，见 core/file_perms.h 的 no-op 说明）。
// 2026-11-03 拍板全平台明文直存；旧 DPAPI 密文（dpapi: 前缀 / bug #126 裸 hex 残角）
// 读取时只读迁移：本机 CryptUnprotectData 解得开就用解出的明文（旧数据免重填），
// 解不开（另一台机器/旧主密钥加密）→ 可执行错误要求重填，不静默回落空串
// （「密钥不可用」伪装成「没配过」更难查），更不把 blob 透传当明文发出去（那是难查的 401）。
#include "credential_protection.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <utility>
#include <windows.h>
#include <wincrypt.h>

namespace platform::credential {
namespace {

// 送解密的 hex 主体：dpapi: 前缀密文去前缀；裸 hex 残角原样。
std::string dpapiHexPart(const std::string& stored) {
    if (stored.rfind(kDpapiBlobPrefix, 0) == 0) {
        return stored.substr(std::char_traits<char>::length(kDpapiBlobPrefix));
    }
    return stored;
}

}  // namespace

core::Result<std::string> realProtect(const std::string& plaintext) {
    // 本平台不做内容级加密；若改成明文之外的形态，必须同步改 realStoresPlaintext()。
    return core::Result<std::string>::ok(plaintext);
}

core::Result<std::string> realUnprotect(const std::string& stored) {
    // 三级（bug #126）：① dpapi: 前缀或裸 hex → 尝试解密旧数据；② 解不开 → 可执行错误；
    // ③ 其余明文直返。解出的明文不回写列（本函数只读，回写归 protect 正常写路径）。
    // need318：解不开约定 InvalidApiKey（与 secrets_store 测试约定一致）——
    // 下游 wecom 据此走「数据异常请重录」而非「瞬时故障」。
    if (!isDpapiBlob(stored)) {
        return core::Result<std::string>::ok(stored);
    }
    std::string blob;
    if (!tryDecodeHex(dpapiHexPart(stored), blob) || blob.empty()) {
        // 前缀形态但 hex 主体损坏/为空：旧密文救不回来，同走「要求重填」臂。
        return core::Result<std::string>::fail(
            core::ErrorCode::InvalidApiKey,
            "该 Key 是旧版 DPAPI 密文且已损坏，无法解密；请重新填写 API Key");
    }
    DATA_BLOB input{};
    input.pbData = reinterpret_cast<BYTE*>(blob.data());
    input.cbData = static_cast<DWORD>(blob.size());
    DATA_BLOB output{};
    // 用户级解密、禁弹 UI——app_secrets 旧版 DPAPI 密文救回臂的解密口径
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        return core::Result<std::string>::fail(
            core::ErrorCode::InvalidApiKey,
            "该 Key 由另一台机器或旧主密钥加密，本机无法解密；请重新填写 API Key");
    }
    std::string plain(reinterpret_cast<const char*>(output.pbData), output.cbData);
    LocalFree(output.pbData);
    return core::Result<std::string>::ok(std::move(plain));
}

bool realStoresPlaintext() { return true; }

const char* realProtectionDescription() {
    return "本机数据库存储（NTFS 权限仅本用户可读）";
}

} // namespace platform::credential
