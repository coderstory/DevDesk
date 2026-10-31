// src/platform/credential_protection.h
// 分平台凭据保护：profile 的 API key 进 SQLite 的 claude_profiles.api_key_encrypted 列。
// macOS/POSIX 存明文（保密靠文件权限 0600，core/file_perms.h）；Windows 存 DPAPI 密文 hex，不得降级为明文。
// 本头平台无关，实现拆 credential_protection_{win,posix}.cpp，业务零 #ifdef。
#pragma once

#include <atomic>
#include <string>

#include "core/result.h"

namespace platform::credential {

// 平台实现（按平台拆文件：credential_protection_win / _posix）
core::Result<std::string> realProtect(const std::string& plaintext);
core::Result<std::string> realUnprotect(const std::string& stored);

// 本平台是否直接把明文写进列；UI 文案与测试共用。
bool realStoresPlaintext();

// 面向用户的保护级别说明（UI 文案直出）。
const char* realProtectionDescription();

// Windows 写出的 DPAPI 密文带此前缀（后接 hex）。用途：库经双机同步搬到另一平台时，
// macOS 能认出「这不是明文密钥」并明确报错，而不是把密文当 API Key 发出去吃 401。
inline constexpr const char* kDpapiBlobPrefix = "dpapi:";

inline bool isDpapiBlob(const std::string& stored) {
    return stored.rfind(kDpapiBlobPrefix, 0) == 0;
}

// 明文形态启发判定：可打印 ASCII、无空白、8..512 字节、含字母数字 —— 典型 API Key 明文。
// 只在 unprotect 已失败之后使用（hex + DPAPI 都解不开），不会把密文误判成明文；
// 目的是接住「macOS 存的明文库同步到 Windows」这条跨平台迁移路径。
inline bool looksLikePlaintextCredential(const std::string& stored) {
    if (stored.size() < 8 || stored.size() > 512) return false;
    bool hasAlnum = false;
    for (const char ch : stored) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c < 0x21 || c > 0x7E) return false;  // 控制符/空白/非 ASCII 一律不算
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z')) {
            hasAlnum = true;
        }
    }
    return hasAlnum;
}

// 注入缝：函数指针表，供单测替换 protect/unprotect；生产只读槽位、从不写。
struct Backend {
    core::Result<std::string> (*protect)(const std::string& plaintext);
    core::Result<std::string> (*unprotect)(const std::string& stored);
};

inline const Backend& defaultBackend() {
    static const Backend b{&realProtect, &realUnprotect};
    return b;
}

// 原子指针：后台线程可能正在调 protect，测试换后端不能是数据竞争。
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

// 明文 → 存储形态：空串→空串；macOS/POSIX 原样返回（靠文件权限），Windows → DPAPI 加密 + hex。
inline core::Result<std::string> protect(const std::string& plaintext) {
    if (plaintext.empty()) return core::Result<std::string>::ok(std::string());
    return backend().protect(plaintext);
}

// 存储形态 → 明文。解密失败/形态损坏 → fail(Internal)，不静默回落空串。
inline core::Result<std::string> unprotect(const std::string& stored) {
    if (stored.empty()) return core::Result<std::string>::ok(std::string());
    return backend().unprotect(stored);
}

inline bool storesPlaintext() { return realStoresPlaintext(); }
inline const char* protectionDescription() { return realProtectionDescription(); }

} // namespace platform::credential
