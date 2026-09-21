// src/platform/keychain_win.cpp
// Windows 实现：DPAPI（CryptProtectData/CryptUnprotectData）+ 文件存储
//
// 存储设计：DPAPI 只负责加密（用户级，密钥派生自当前 Windows 用户凭据），
// 密文 blob 落盘到 %APPDATA%/DevDesk/keychain/<service>/<account>.key（二进制）。
// service/account 做文件名安全化（白名单 [A-Za-z0-9-_]，其余替换为 '_'），
// 与 macOS 实现语义对齐：load 不存在 → NotFound；delete 不存在 → Ok（幂等）。
#include "keychain.h"

#include <core/paths.h>

#include <filesystem>
#include <fstream>
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

// 文件名安全化：只保留字母数字和 -_，其余替换为 '_'（防路径穿越/非法字符）
std::string sanitize(const std::string& name) {
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

// <appData>/keychain/<service>/<account>.key
fs::path keyFilePath(const std::string& service, const std::string& account) {
    return core::paths::appDataDir() / "keychain" / sanitize(service) /
           (sanitize(account) + ".key");
}

std::string dpapiError(const char* what) {
    return std::string(what) + " failed: GetLastError=" +
           std::to_string(GetLastError());
}

} // namespace

core::Result<void> storeSecret(const std::string& service,
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

    // 目录惰性创建 + 二进制覆盖写（同 service/account 直接覆盖旧值）
    const fs::path path = keyFilePath(service, account);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        LocalFree(output.pbData);
        return core::Result<void>::fail(
            core::ErrorCode::FileWriteFailed,
            "create keychain dir failed: " + ec.message());
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        LocalFree(output.pbData);
        return core::Result<void>::fail(
            core::ErrorCode::FileWriteFailed,
            "open key file failed: " + path.string());
    }
    file.write(reinterpret_cast<const char*>(output.pbData),
               static_cast<std::streamsize>(output.cbData));
    file.close();
    LocalFree(output.pbData);
    if (!file) {
        return core::Result<void>::fail(
            core::ErrorCode::FileWriteFailed,
            "write key file failed: " + path.string());
    }
    return core::Result<void>::ok();
}

core::Result<std::string> loadSecret(const std::string& service,
                                     const std::string& account) {
    const fs::path path = keyFilePath(service, account);

    std::error_code ec;
    if (!fs::exists(path, ec)) {
        return core::Result<std::string>::fail(core::ErrorCode::NotFound,
                                               "Keychain item not found");
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

core::Result<void> deleteSecret(const std::string& service,
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
    return core::Result<void>::ok();
}

} // namespace platform::keychain
