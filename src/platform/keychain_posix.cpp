// src/platform/keychain_posix.cpp
// 其他平台（Linux 等）实现：本地文件 fallback（开发用）
#include "keychain.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace platform::keychain {

namespace {
    std::filesystem::path getKeyFilePath(const std::string& service, const std::string& account) {
        auto home = std::getenv("HOME");
        std::filesystem::path dir = home ? std::filesystem::path(home) / ".devdesk" : std::filesystem::temp_directory_path();
        std::filesystem::create_directories(dir);
        return dir / (service + "_" + account + ".key");
    }
}

core::Result<void> storeSecret(const std::string& service, const std::string& account, const std::string& secret) {
    auto path = getKeyFilePath(service, account);
    std::ofstream f(path, std::ios::binary);
    if (!f) return core::Result<void>::fail(core::ErrorCode::FileWriteFailed, "cannot write key file");
    f << secret;
    return core::Result<void>::ok();
}

core::Result<std::string> loadSecret(const std::string& service, const std::string& account) {
    auto path = getKeyFilePath(service, account);
    std::ifstream f(path);
    if (!f) return core::Result<std::string>::fail(core::ErrorCode::NotFound, "key file not found");
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return core::Result<std::string>::ok(std::move(s));
}

core::Result<void> deleteSecret(const std::string& service, const std::string& account) {
    auto path = getKeyFilePath(service, account);
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return core::Result<void>::ok();
}

} // namespace platform::keychain
