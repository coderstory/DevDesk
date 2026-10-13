// src/platform/keychain_win.cpp
// Windows：DPAPI（CryptProtectData/CryptUnprotectData，用户级、禁弹 UI）+ 文件存储，密文 blob 落盘
// %APPDATA%/DevDesk/keychain/<service>/<account>.key。service/account 做可逆文件名编码（白名单
// [A-Za-z0-9-_] 原样、其余 %XX，见 encodeName）→ 不同 account 必落不同文件；落盘走 tmp+rename 原子写。
// 旧命名文件仍兼容：load 缺失时回退读旧命名、delete 两侧都清、sweep 两形态都认。load 不存在 → NotFound；delete 不存在 → Ok。
#include "keychain.h"

#include <core/atomic_write.h>
#include <core/paths.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <utility>

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

// 文件名编码：白名单 [A-Za-z0-9-_] 原样，其余字节转义 %XX（'%' 自身 → %25）。逐字节编码/解码互逆，
// 映射单射：不同 account/service 必落不同文件名。
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
    // 空名 → 裸 '%'（任何非空名的编码都不会是裸 '%'，单射性保持）
    return out.empty() ? std::string("%") : out;
}

// 旧命名（不可逆）：白名单外替换 '_'。仅用于读取/清理旧文件，新写入一律 encodeName。
std::string legacyName(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (char ch : name) {
        const unsigned char c = static_cast<unsigned char>(ch);
        const bool allowed =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_';
        out += allowed ? static_cast<char>(c) : '_';
    }
    return out;
}

// <appData>/keychain/<encode(service)>/<encode(account)>.key
fs::path keyFilePath(const std::string& service, const std::string& account) {
    return core::paths::appDataDir() / "keychain" / encodeName(service) /
           (encodeName(account) + ".key");
}

std::string dpapiError(const char* what) {
    return std::string(what) + " failed: GetLastError=" +
           std::to_string(GetLastError());
}

} // namespace

// 真实实现
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

    // 原子写保证崩溃/断电不留半截文件（否则 loadSecret 分不清「没配」与「损坏」），失败时目标保持原内容。
    const std::string blob(reinterpret_cast<const char*>(output.pbData),
                           output.cbData);
    LocalFree(output.pbData);

    auto written = core::atomicWriteFile(path, blob, "密钥文件");
    if (written.isFail()) {
        return core::Result<void>::fail(core::ErrorCode::FileWriteFailed,
                                        written.errorMessage());
    }

    return core::Result<void>::ok();
}

core::Result<std::string> realLoadSecret(const std::string& service,
                                         const std::string& account) {
    const fs::path path = keyFilePath(service, account);

    // 旧命名文件兼容：新编码缺失时回退读旧文件（内容同样 DPAPI 解密）；
    // 读成功不回写 encoded（读取路径无写副作用）。
    std::error_code ec;
    fs::path target = path;
    if (!fs::exists(path, ec)) {
        target = path.parent_path() / (legacyName(account) + ".key");
        if (!fs::exists(target, ec)) {
            return core::Result<std::string>::fail(core::ErrorCode::NotFound,
                                                   "Keychain item not found");
        }
    }

    std::ifstream file(target, std::ios::binary);
    if (!file) {
        return core::Result<std::string>::fail(
            core::ErrorCode::FileReadFailed,
            "open key file failed: " + target.string());
    }
    std::string blob((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
    if (blob.empty()) {
        return core::Result<std::string>::fail(
            core::ErrorCode::Internal, "key file empty: " + target.string());
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
    const fs::path legacyPath =
        path.parent_path() / (legacyName(account) + ".key");

    // 新编码与旧命名两侧都清，否则 load 会经旧命名回退把已删密钥读回来；任一不存在不算错（幂等）。
    std::error_code ec;
    fs::remove(path, ec);
    if (ec) {
        return core::Result<void>::fail(
            core::ErrorCode::Internal,
            "delete key file failed: " + ec.message());
    }
    ec.clear();
    fs::remove(legacyPath, ec);
    if (ec) {
        return core::Result<void>::fail(
            core::ErrorCode::Internal,
            "delete legacy key file failed: " + ec.message());
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

    // 存储形态是文件名编码后的 account（claude:5 → claude%3A5），keep 按编码后比较；旧命名两形态
    // 都进 keep，否则 sweep 会把仍在用的旧命名条目当孤儿清掉。
    std::set<std::string> keep;
    for (const auto& a : keepAccounts) {
        keep.insert(encodeName(a));
        keep.insert(legacyName(a));
    }

    // 白名单前缀：落盘 stem 是 encodeName(account) 或 legacyName(account)（旧命名），不命中
    // 一律保留。比对发生在编码后的文件名形态上，故不能直调 shouldDeleteOrphan。
    const std::string sweepPrefix = encodeName("claude:");
    auto sweepableStem = [&sweepPrefix](const std::string& stem) {
        if (stem.compare(0, sweepPrefix.size(), sweepPrefix) == 0) {
            return true;
        }
        // 旧形态：legacyName("claude:<id>") = "claude_<id>"。字面 claude_<n> 账号也会被判可清，可接受
        //——真实写入口只有 claude:<id>，从不产生字面 claude_<n>。
        if (stem.size() > 7 && stem.compare(0, 7, "claude_") == 0) {
            bool allDigits = true;
            for (char ch : stem.substr(7)) {
                if (ch < '0' || ch > '9') {
                    allDigits = false;
                    break;
                }
            }
            if (allDigits) {
                return true;
            }
        }
        return false;
    };

    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file(ec) || ec) { ec.clear(); continue; }
        // 只认 <name>.key：跳过原子写的临时残留（<name>.key.devdesk-tmp）等非密钥条目，别当孤儿计数
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
