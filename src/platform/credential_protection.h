// src/platform/credential_protection.h
// 分平台凭据保护：profile 的 API key 进 SQLite 的 claude_profiles.api_key_encrypted 列。
// 2026-11-03 起全平台统一明文口径（DPAPI 已删，不做兼容）：列里就是明文，保密靠文件权限
// ——macOS/POSIX = 0600，Windows = 数据根 %USERPROFILE% 下 NTFS 默认 ACL（仅本用户/管理员），
// 见 core/file_perms.h。本头平台无关，实现拆 credential_protection_{win,posix}.cpp，业务零 #ifdef。
#pragma once

#include <atomic>
#include <string>

#include "core/result.h"

namespace platform::credential {

// 平台实现（按平台拆文件：credential_protection_win / _posix）
core::Result<std::string> realProtect(const std::string& plaintext);
core::Result<std::string> realUnprotect(const std::string& stored);

// 本平台是否直接把明文写进列；UI 文案与测试共用（当前两平台恒 true）。
bool realStoresPlaintext();

// 面向用户的保护级别说明（UI 文案直出）。
const char* realProtectionDescription();

// 旧版 Windows 曾用 DPAPI 写库，密文带此前缀（后接 hex）。Windows 现已不再写这种形态，
// 但旧库/双机同步搬来的库里仍可能带着它：保留前缀做跨平台识别，认出即 fail，
// 不把密文当 API Key 发出去（那会变成难查的 401）。
inline constexpr const char* kDpapiBlobPrefix = "dpapi:";

inline bool isDpapiBlob(const std::string& stored) {
    return stored.rfind(kDpapiBlobPrefix, 0) == 0;
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

// 明文 → 存储形态：空串→空串；全平台原样返回（保密靠文件权限 / NTFS ACL）。
inline core::Result<std::string> protect(const std::string& plaintext) {
    if (plaintext.empty()) return core::Result<std::string>::ok(std::string());
    return backend().protect(plaintext);
}

// 存储形态 → 明文。旧 DPAPI 密文等不支持形态 → fail(Internal)，不静默回落空串。
inline core::Result<std::string> unprotect(const std::string& stored) {
    if (stored.empty()) return core::Result<std::string>::ok(std::string());
    return backend().unprotect(stored);
}

inline bool storesPlaintext() { return realStoresPlaintext(); }
inline const char* protectionDescription() { return realProtectionDescription(); }

} // namespace platform::credential
