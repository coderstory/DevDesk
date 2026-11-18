// src/platform/credential_protection.h
// 分平台凭据保护：profile 的 API key 进 SQLite 的 claude_profiles.api_key_encrypted 列。
// 本头是实现侧（need305 审计 P0 依赖倒置）：core::secrets_store 经自己的
// Backend 缝消费 protect/unprotect，由 main.cpp 启动装配——core 不依赖
// platform（AGENTS §六）。
// 2026-11-03 起全平台统一明文口径：列里就是明文，保密靠文件权限
// ——macOS/POSIX = 0600，Windows = 数据根 %USERPROFILE% 下 NTFS 默认 ACL（仅本用户/管理员），
// 见 core/file_perms.h。旧 DPAPI 密文（dpapi: 前缀 / bug #126 裸 hex 残角）不再写入；
// 读取：Windows 本机只读迁移尝试解密（bug #126），解不开与 POSIX 一律要求重填。
// 本头平台无关，实现拆 credential_protection_{win,posix}.cpp，业务零 #ifdef。
#pragma once

#include <atomic>
#include <cstddef>
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

// 旧版 Windows 曾用 DPAPI 写库，两种历史形态（bug #126 补认裸 hex）：dpapi: 前缀（后接
// hex）与裸 hex（直接把 CryptProtectData 产物转 hex 落列）。Windows 现已不再写这两种形态，
// 但旧库/双机同步搬来的库里仍可能带着它们：认出后 Windows 只读迁移尝试解密、解不开与
// POSIX 直接 fail 带重填文案，不把密文当 API Key 发出去（那会变成难查的 401）。
inline constexpr const char* kDpapiBlobPrefix = "dpapi:";

// DPAPI blob 固定开头：版本 01 00 00 00 + magic D0 8C 9D DF（CryptProtectData 产物首 8 字节）。
inline constexpr unsigned char kDpapiMagic[8] = {0x01, 0x00, 0x00, 0x00,
                                                 0xD0, 0x8C, 0x9D, 0xDF};
// 真实 DPAPI blob 定长头就 60 字节，再短必不是；短 hex 明文 key（16/32 字符）在这关被一票否决。
inline constexpr std::size_t kDpapiMinBlobBytes = 60;

// 整串 hex → 字节串（大小写 hex 均可）；空串/奇数长度/非 hex 字符 → false（out 清空，不留半截脏数据）。
inline bool tryDecodeHex(const std::string& hex, std::string& out) {
    const auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    out.clear();
    if (hex.empty() || hex.size() % 2 != 0) return false;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int hi = nib(hex[i]);
        const int lo = nib(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            out.clear();
            return false;
        }
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return true;
}

// DPAPI 密文识别（bug #126 三重判据防误杀）：dpapi: 前缀直认；裸 hex 须同时满足
// 整串可 hex 解码、解码 ≥60 字节、首 8 字节 = DPAPI magic——三条缺一即按明文放行。
inline bool isDpapiBlob(const std::string& stored) {
    if (stored.rfind(kDpapiBlobPrefix, 0) == 0) return true;
    if (stored.size() < kDpapiMinBlobBytes * 2) return false;   // 预筛：hex 长度不足则解码必 <60 字节
    std::string decoded;
    if (!tryDecodeHex(stored, decoded)) return false;
    if (decoded.size() < kDpapiMinBlobBytes) return false;
    for (std::size_t i = 0; i < 8; ++i) {
        if (static_cast<unsigned char>(decoded[i]) != kDpapiMagic[i]) return false;
    }
    return true;
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

// 存储形态 → 明文。旧 DPAPI 密文按平台实现处理（Windows 只读迁移尝试解密，
// 解不开 fail(Internal) 带重填文案），不静默回落空串。
inline core::Result<std::string> unprotect(const std::string& stored) {
    if (stored.empty()) return core::Result<std::string>::ok(std::string());
    return backend().unprotect(stored);
}

inline bool storesPlaintext() { return realStoresPlaintext(); }
inline const char* protectionDescription() { return realProtectionDescription(); }

} // namespace platform::credential
