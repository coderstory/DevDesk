// src/platform/credential_protection.h
// 分平台凭据保护：profile 的 API key 进 SQLite 的 claude_profiles.api_key_encrypted 列。
// 「不用钥匙串」≠「明文落盘」：macOS/POSIX 存明文，保密靠文件权限 0600（core/file_perms.h）；
// Windows 存 DPAPI 密文的 hex 文本（同机同用户可解），**不得降级为明文**。
// 平台适配隔离：本头平台无关，实现拆 credential_protection_{win,posix}.cpp，业务零 #ifdef；
// macOS 与 Linux 行为一致，按仓库约定共用 _posix。
#pragma once

#include <atomic>
#include <string>

#include "core/result.h"

namespace platform::credential {

// 平台实现（按平台拆文件：credential_protection_win / _posix）
core::Result<std::string> realProtect(const std::string& plaintext);
core::Result<std::string> realUnprotect(const std::string& stored);

// 本平台是否直接把明文写进列（UI 文案与测试共用，不撒谎的描述来源）。
bool realStoresPlaintext();

// 面向用户的保护级别说明（UI 文案直出，跨平台不撒谎）。
const char* realProtectionDescription();

// 注入缝（CPX-14 同款：函数指针表 + 单一可替换槽位）：形态同 keychain.h 的
// Backend，用于单测「protect 失败」时的补偿逻辑。生产只读槽位、从不写。
struct Backend {
    core::Result<std::string> (*protect)(const std::string& plaintext);
    core::Result<std::string> (*unprotect)(const std::string& stored);
};

inline const Backend& defaultBackend() {
    static const Backend b{&realProtect, &realUnprotect};
    return b;
}

// 原子指针：代理/后台线程可能正在调 protect，测试换后端不应是数据竞争
inline std::atomic<const Backend*>& backendSlot() {
    static std::atomic<const Backend*> slot{&defaultBackend()};
    return slot;
}

inline const Backend& backend() {
    return *backendSlot().load(std::memory_order_relaxed);
}

// 安装 b（nullptr = 恢复真实实现），返回旧后端供 RAII 复位。仅测试调用。
inline const Backend* setBackendForTest(const Backend* b) {
    return backendSlot().exchange(b ? b : &defaultBackend(),
                                  std::memory_order_relaxed);
}

// 明文 → 可入库存储形态：空串→空串（「没有密钥」状态本身）；macOS/POSIX 原样返回
//（保护靠文件权限）；Windows → DPAPI 加密 + hex 编码。
inline core::Result<std::string> protect(const std::string& plaintext) {
    if (plaintext.empty()) return core::Result<std::string>::ok(std::string());
    return backend().protect(plaintext);
}

// 存储形态 → 明文。空串→空串；形态损坏/解密失败（换机器、换用户、列被篡改）→
// fail(Internal)，**不静默回落空串**——静默会把「密钥不可用」伪装成「没配过密钥」。
inline core::Result<std::string> unprotect(const std::string& stored) {
    if (stored.empty()) return core::Result<std::string>::ok(std::string());
    return backend().unprotect(stored);
}

inline bool storesPlaintext() { return realStoresPlaintext(); }
inline const char* protectionDescription() { return realProtectionDescription(); }

} // namespace platform::credential
