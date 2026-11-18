// src/platform/credential_protection_posix.cpp
// macOS / Linux：明文进列，保密靠文件权限 0600（core/file_perms.h 收紧 db + wal + shm）
#include "credential_protection.h"

namespace platform::credential {

core::Result<std::string> realProtect(const std::string& plaintext) {
    // 本平台不做内容级加密；若改成明文之外的形态，必须同步改 realStoresPlaintext()。
    return core::Result<std::string>::ok(plaintext);
}

core::Result<std::string> realUnprotect(const std::string& stored) {
    // 双机同步：库里可能是 Windows 写入的 DPAPI 密文，本机解不了。
    // 明确报错而不是把密文当 API Key 发出去（那会变成难查的 401）。
    if (isDpapiBlob(stored)) {
        // need318：解不开约定 InvalidApiKey（与 secrets_store 测试约定一致）——
        // 下游 wecom 据此走「数据异常请重录」而非「瞬时故障」
        return core::Result<std::string>::fail(
            core::ErrorCode::InvalidApiKey,
            "该 Profile 的 API Key 是 Windows 机器用 DPAPI 加密的，本机无法解密；"
            "请在本机重新填写 API Key");
    }
    return core::Result<std::string>::ok(stored);
}

bool realStoresPlaintext() { return true; }

const char* realProtectionDescription() {
    return "本机数据库存储（文件权限 0600，仅本用户可读）";
}

} // namespace platform::credential
