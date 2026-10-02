// src/platform/credential_protection.h
// 分平台凭据保护（2026-10-02 存储统一设计 §6.2）
//
// profile 的 API key 从系统钥匙串迁进 SQLite 的 `claude_profiles.api_key_encrypted`
// 列。**「不用钥匙串」不等于「明文落盘」**——保护级别按平台各有实现：
//
//   | 平台            | 列里存什么                 | 保护级别                 |
//   |-----------------|----------------------------|--------------------------|
//   | macOS / POSIX   | 明文                       | 文件权限级（0600，见      |
//   |                 |                            | core/file_perms.h）      |
//   | Windows         | DPAPI 密文的 hex 文本      | 加密级（同机同用户可解）  |
//
// Windows 侧一律经 DPAPI（CryptProtectData / CryptUnprotectData，复用
// keychain_win.cpp 的既有调用形态与 CRYPTPROTECT_UI_FORBIDDEN 口径），
// **不得降级为明文**——评审专项核查这条。
//
// 平台适配接口隔离（铁律）：本头文件平台无关，实现按平台拆文件
// credential_protection_{win,posix}.cpp，CMake 按平台选源；业务代码零 #ifdef。
// macOS 与 Linux 行为一致（都是明文 + 文件权限），按仓库既有约定共用 _posix
// （先例：core/time_posix.cpp 与 core/atomic_write_posix.cpp 同在 APPLE 分支编译）。
#pragma once

#include <atomic>
#include <string>

#include "core/result.h"

namespace platform::credential {

// ---------- 平台实现（按平台拆文件：credential_protection_win / _posix）----------
core::Result<std::string> realProtect(const std::string& plaintext);
core::Result<std::string> realUnprotect(const std::string& stored);

// 「本平台是否直接把明文写进列」——UI 文案与测试共用（不撒谎的描述来源）。
bool realStoresPlaintext();

// 一行面向用户的保护级别说明（UI 文案直出；跨平台不撒谎）。
const char* realProtectionDescription();

// ---------- 注入缝（CPX-14 同款：函数指针表 + 单一可替换槽位）----------
// 用途：迁移前用假 Keychain 后端注入「段一写失败」来单测两段写补偿；改 DB 后
// 该失败点变成「protect 失败」，注入面随之挪到这里，补偿逻辑仍可单测。
// 形态与 keychain.h 的 Backend 一字同款（最小形态，不引入虚基类体系）。
// 生产路径只读槽位、从不写；测试经 setBackendForTest 整表替换。
struct Backend {
    core::Result<std::string> (*protect)(const std::string& plaintext);
    core::Result<std::string> (*unprotect)(const std::string& stored);
};

inline const Backend& defaultBackend() {
    static const Backend b{&realProtect, &realUnprotect};
    return b;
}

// 用原子指针：代理 / 后台线程可能正在调 protect，测试换后端不应是数据竞争
inline std::atomic<const Backend*>& backendSlot() {
    static std::atomic<const Backend*> slot{&defaultBackend()};
    return slot;
}

inline const Backend& backend() {
    return *backendSlot().load(std::memory_order_relaxed);
}

// 安装 b（传 nullptr = 恢复真实实现），返回被替换的旧后端供 RAII 复位。仅测试调用。
inline const Backend* setBackendForTest(const Backend* b) {
    return backendSlot().exchange(b ? b : &defaultBackend(),
                                  std::memory_order_relaxed);
}

// ---------- 调用入口 ----------

// 明文 → 可入库的存储形态。
//   空串 → 空串（空 = 「该 profile 没有密钥」这一状态本身，无需保护）
//   macOS/POSIX → 原样返回（保护靠文件权限，见 core/file_perms.h）
//   Windows → DPAPI 加密 + hex 编码
inline core::Result<std::string> protect(const std::string& plaintext) {
    if (plaintext.empty()) return core::Result<std::string>::ok(std::string());
    return backend().protect(plaintext);
}

// 存储形态 → 明文。protect 的逆运算。
//   空串 → 空串
//   形态损坏 / 解密失败（换机器、换用户、列被篡改）→ fail(Internal)，**不静默
//   回落空串**——静默会把「密钥不可用」伪装成「没配过密钥」，用户无从诊断。
inline core::Result<std::string> unprotect(const std::string& stored) {
    if (stored.empty()) return core::Result<std::string>::ok(std::string());
    return backend().unprotect(stored);
}

inline bool storesPlaintext() { return realStoresPlaintext(); }
inline const char* protectionDescription() { return realProtectionDescription(); }

} // namespace platform::credential
