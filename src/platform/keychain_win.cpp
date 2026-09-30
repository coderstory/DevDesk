// src/platform/keychain_win.cpp
// Windows 实现：DPAPI（CryptProtectData/CryptUnprotectData）+ 文件存储
//
// 存储设计：DPAPI 只负责加密（用户级，密钥派生自当前 Windows 用户凭据），
// 密文 blob 落盘到 %APPDATA%/DevDesk/keychain/<service>/<account>.key（二进制）。
// service/account 做可逆文件名编码（白名单 [A-Za-z0-9-_] 原样，其余字节 %XX
// 转义，见 encodeName）——不同 account 必落不同文件；落盘走 tmp + rename
// 原子写（core::atomicWriteFile）。
// CP-11 之前的命名规则（非白名单字符一律折成 '_'，a:b 与 a/b 同文件）与
// ofstream(trunc) 原地覆盖写保留读/删兼容：load 在新编码路径缺失时回退读
// 旧命名文件；delete 两侧都删（见 legacyKeyFilePath）。不做文件迁移。
// 与 macOS 实现语义对齐：load 不存在 → NotFound；delete 不存在 → Ok（幂等）。
#include "keychain.h"

#include <core/atomic_write.h>
#include <core/paths.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>

namespace fs = std::filesystem;

namespace platform::keychain {
namespace {

// 文件名编码（CP-11）：白名单 [A-Za-z0-9-_] 原样保留，其余字节转义成 %XX
// （'%' 自身 → %25，它不在白名单里）。逐字节编码 + 逐字节解码互逆，故映射
// 单射：不同 account / service 必落不同文件名（旧规则把所有非白名单字符折成
// '_'，a:b 与 a/b 落同一个文件——互相覆盖、delete 误伤，审计条目 CP-11）。
std::string encodeName(const std::string& name) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(name.size());
    for (char ch : name) {
        const unsigned char c = static_cast<unsigned char>(ch);
        const bool allowed =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (allowed) {
            out += static_cast<char>(c);
            continue;
        }
        out += '%';
        out += kHex[c >> 4];
        out += kHex[c & 0x0F];
    }
    // 空名 → 裸 '%'：任何非空名的编码都不会是裸 '%'（'%' 必带两位十六进制），
    // 单射性保持（旧规则的 "_" 会与字面名 "_" 撞车）
    return out.empty() ? std::string("%") : out;
}

// 旧命名规则（CP-11 之前）：非白名单字符统一替换为 '_'（防路径穿越/非法字符）。
// 只用于读/删历史文件与旧文件识别，不再用于新写入。
std::string legacySanitize(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        const bool allowed =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_';
        out += allowed ? c : '_';
    }
    if (out.empty()) out = "_";
    return out;
}

// <appData>/keychain/<encode(service)>/<encode(account)>.key
fs::path keyFilePath(const std::string& service, const std::string& account) {
    return core::paths::appDataDir() / "keychain" / encodeName(service) /
           (encodeName(account) + ".key");
}

// 旧命名规则下的路径：双读兼容用（只读/删，不写）。account 全在白名单内时
// 与 keyFilePath 同值（调用方据此跳过回退，避免多余的系统调用）。
fs::path legacyKeyFilePath(const std::string& service,
                           const std::string& account) {
    return core::paths::appDataDir() / "keychain" / legacySanitize(service) /
           (legacySanitize(account) + ".key");
}

std::string dpapiError(const char* what) {
    return std::string(what) + " failed: GetLastError=" +
           std::to_string(GetLastError());
}

} // namespace

// 真实实现（CPX-14 起经 keychain.h 的函数指针表转发，见那里的「注入缝」段）
core::Result<void> realStoreSecret(const std::string& service,
                                   const std::string& account,
                                   const std::string& secret) {
    // DPAPI 加密（用户级，禁止弹 UI）
    DATA_BLOB input{};
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(secret.data()));
    input.cbData = static_cast<DWORD>(secret.size());

    DATA_BLOB output{};
    if (!CryptProtectData(&input, nullptr, nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        return core::Result<void>::fail(core::ErrorCode::Internal,
                                        dpapiError("CryptProtectData"));
    }

    // 目录惰性创建 + 原子落盘（同 service/account 直接覆盖旧值）
    const fs::path path = keyFilePath(service, account);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        LocalFree(output.pbData);
        return core::Result<void>::fail(
            core::ErrorCode::FileWriteFailed,
            "create keychain dir failed: " + ec.message());
    }

    // 旧实现是 ofstream(binary|trunc) 原地覆盖写：崩溃/断电会留下半截文件，
    // loadSecret 只能报 Internal，分不清「没配」与「损坏」（审计 CP-11）。
    // 改为 core::atomicWriteFile（同目录 tmp → 覆盖式替换），失败时目标保持
    // 原有内容。错误码保持历史语义：落盘失败一律 FileWriteFailed。
    const std::string blob(reinterpret_cast<const char*>(output.pbData),
                           output.cbData);
    LocalFree(output.pbData);

    auto written = core::atomicWriteFile(path, blob, "密钥文件");
    if (written.isFail()) {
        return core::Result<void>::fail(core::ErrorCode::FileWriteFailed,
                                        written.errorMessage());
    }

    // 不清理旧命名残留文件：旧规则下它可能与字面同名 account 共用（a:b 的
    // 旧文件就叫 a_b），删除有误伤活数据的风险；读路径恒优先新编码文件，
    // 残留不影响取值（见 loadSecret）
    return core::Result<void>::ok();
}

core::Result<std::string> realLoadSecret(const std::string& service,
                                         const std::string& account) {
    // 双读兼容（CP-11）：先找新编码路径，缺失时回退旧命名路径——升级前写入的
    // 密钥继续可读（不做文件迁移，读路径代价最小）
    fs::path path = keyFilePath(service, account);

    std::error_code ec;
    if (!fs::exists(path, ec)) {
        const fs::path legacy = legacyKeyFilePath(service, account);
        if (legacy != path && fs::exists(legacy, ec)) {
            path = legacy;
        } else {
            return core::Result<std::string>::fail(core::ErrorCode::NotFound,
                                                   "Keychain item not found");
        }
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return core::Result<std::string>::fail(
            core::ErrorCode::FileReadFailed,
            "open key file failed: " + path.string());
    }
    std::string blob((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
    if (blob.empty()) {
        return core::Result<std::string>::fail(
            core::ErrorCode::Internal, "key file empty: " + path.string());
    }

    // DPAPI 解密（仅同用户同机可解，与 macOS Keychain 默认行为同级）
    DATA_BLOB input{};
    input.pbData = reinterpret_cast<BYTE*>(blob.data());
    input.cbData = static_cast<DWORD>(blob.size());

    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        return core::Result<std::string>::fail(
            core::ErrorCode::Internal, dpapiError("CryptUnprotectData"));
    }

    std::string secret(reinterpret_cast<char*>(output.pbData), output.cbData);
    LocalFree(output.pbData);
    return core::Result<std::string>::ok(std::move(secret));
}

core::Result<void> realDeleteSecret(const std::string& service,
                                    const std::string& account) {
    const fs::path path = keyFilePath(service, account);

    // 不存在返回 false 不报错（幂等删除，与 macOS 实现语义一致）
    std::error_code ec;
    fs::remove(path, ec);
    if (ec) {
        return core::Result<void>::fail(
            core::ErrorCode::Internal,
            "delete key file failed: " + ec.message());
    }

    // 旧命名文件一并删（CP-11 双读兼容的配套）：否则 load 会经回退把它读回来，
    // delete → load 就不是 NotFound 了。account 全在白名单内时两侧同路径，
    // 上面已删过 → 跳过。
    // 已知代价：旧规则下 a:b 的遗留文件叫 a_b，若同时有字面名 a_b 的条目
    // （旧规则本就无法区分这两者，属升级前的固有歧义），删 a:b 会连带删掉它。
    // 取「delete 后 load 必须 NotFound」的一致性优先，且本项目 account 形态
    // 固定为 claude:<id>，不存在字面名 claude_<id> 的条目。
    const fs::path legacy = legacyKeyFilePath(service, account);
    if (legacy != path) {
        std::error_code legacyEc;
        fs::remove(legacy, legacyEc);
        if (legacyEc) {
            return core::Result<void>::fail(
                core::ErrorCode::Internal,
                "delete legacy key file failed: " + legacyEc.message());
        }
    }
    return core::Result<void>::ok();
}

core::Result<std::vector<std::string>> realDeleteSecretsNotIn(
    const std::string& service,
    const std::vector<std::string>& keepAccounts) {
    std::vector<std::string> removed;
    const fs::path dir = core::paths::appDataDir() / "keychain" /
                         encodeName(service);

    std::error_code ec;
    if (!fs::exists(dir, ec) || ec) {
        return core::Result<std::vector<std::string>>::ok(std::move(removed));
    }

    // 存储形态是文件名编码后的 account（claude:5 → claude%3A5），keep 集合同样
    // 按编码后比较；旧命名形态（claude_5）也放进 keep 集合，否则升级后第一次
    // 孤儿清理就会把「账号仍在用、只是升级前写入」的旧文件当孤儿删掉。
    // 服务的两个目录名只在 service 含非白名单字符时不同（本项目 service 固定为
    // DevDesk*，两者同值），一次扫描即覆盖新旧两种文件。
    std::set<std::string> keep;
    for (const auto& a : keepAccounts) {
        keep.insert(encodeName(a));
        keep.insert(legacySanitize(a));
    }

    // P0 护栏②（白名单前缀，2026-09-30）：account 约定 claude:<id>，落盘
    // stem 是 encodeName(account)（新命名）或 legacySanitize(account)（旧命名
    // 升级前写入的残留）。两种形态各自比对 claude 前缀，都不命中的一律保留
    //（同 keychain.h shouldDeleteOrphan 的白名单语义——比对发生在编码后的
    // 文件名形态上，故此处不能直调该函数）。
    const std::string sweepPrefixNew = encodeName("claude:");
    const std::string sweepPrefixLegacy = legacySanitize("claude:");
    auto sweepableStem = [&sweepPrefixNew, &sweepPrefixLegacy](
                             const std::string& stem) {
        return stem.compare(0, sweepPrefixNew.size(), sweepPrefixNew) == 0 ||
               stem.compare(0, sweepPrefixLegacy.size(), sweepPrefixLegacy) == 0;
    };

    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file(ec) || ec) { ec.clear(); continue; }
        // 只认 <name>.key：跳过原子写的临时残留（<name>.key.devdesk-tmp，
        // 见 core::atomicWriteFile）等非密钥条目，别把它们当孤儿计数
        if (entry.path().extension() != ".key") continue;
        const std::string stem = entry.path().stem().string();
        if (keep.count(stem)) continue;
        if (!sweepableStem(stem)) continue;
        std::error_code rmEc;
        fs::remove(entry.path(), rmEc);
        if (rmEc) {
            return core::Result<std::vector<std::string>>::fail(
                core::ErrorCode::Internal,
                "delete key file failed: " + rmEc.message());
        }
        removed.push_back(stem);
    }
    return core::Result<std::vector<std::string>>::ok(std::move(removed));
}

} // namespace platform::keychain
