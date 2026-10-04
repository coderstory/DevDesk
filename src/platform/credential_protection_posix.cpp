// src/platform/credential_protection_posix.cpp
// macOS / Linux：明文进列，保密靠文件权限 0600（core/file_perms.h 收紧 db + wal + shm）
#include "credential_protection.h"

namespace platform::credential {

core::Result<std::string> realProtect(const std::string& plaintext) {
    // 本平台不做内容级加密；若改成明文之外的形态，必须同步改 realStoresPlaintext()。
    return core::Result<std::string>::ok(plaintext);
}

core::Result<std::string> realUnprotect(const std::string& stored) {
    return core::Result<std::string>::ok(stored);
}

bool realStoresPlaintext() { return true; }

const char* realProtectionDescription() {
    return "本机数据库存储（文件权限 0600，仅本用户可读）";
}

} // namespace platform::credential
