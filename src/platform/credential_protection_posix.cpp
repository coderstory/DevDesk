// src/platform/credential_protection_posix.cpp
// macOS / Linux / 其它 POSIX：明文进列，保护级别 = 文件权限 0600
//（core/file_perms.h 收紧 devdesk.db + -wal + -shm）
#include "credential_protection.h"

namespace platform::credential {

core::Result<std::string> realProtect(const std::string& plaintext) {
    // 本平台不做内容级加密：列的保密性由承载它的文件权限保证。
    // 这条不是「先留后补」——设计 §6.2 的表格明确本平台就是文件权限级；
    // 谁若把这里改成明文之外的形态，必须同步改 realStoresPlaintext()。
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
