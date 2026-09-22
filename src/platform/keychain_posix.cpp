// src/platform/keychain_posix.cpp
// 其他平台（Linux 等）实现：本地文件 fallback（开发用）
#include "keychain.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

namespace platform::keychain {

namespace {
    std::filesystem::path getKeyFilePath(const std::string& service, const std::string& account) {
        auto home = std::getenv("HOME");
        std::filesystem::path dir = home ? std::filesystem::path(home) / ".devdesk" : std::filesystem::temp_directory_path();
        std::filesystem::create_directories(dir);
        return dir / (service + "_" + account + ".key");
    }

    std::filesystem::path getKeyDir() {
        auto home = std::getenv("HOME");
        return home ? std::filesystem::path(home) / ".devdesk" : std::filesystem::temp_directory_path();
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

core::Result<std::vector<std::string>> deleteSecretsNotIn(
    const std::string& service,
    const std::vector<std::string>& keepAccounts) {
    std::vector<std::string> removed;
    const std::string prefix = service + "_";
    const std::string suffix = ".key";
    std::set<std::string> keep(keepAccounts.begin(), keepAccounts.end());

    std::error_code ec;
    if (!std::filesystem::exists(getKeyDir(), ec) || ec) {
        return core::Result<std::vector<std::string>>::ok(std::move(removed));
    }
    for (const auto& entry : std::filesystem::directory_iterator(getKeyDir())) {
        if (!entry.is_regular_file(ec) || ec) { ec.clear(); continue; }
        const std::string name = entry.path().filename().string();
        if (name.size() <= prefix.size() + suffix.size() ||
            name.compare(0, prefix.size(), prefix) != 0 ||
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
            continue;
        }
        const std::string account =
            name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
        if (keep.count(account)) continue;
        std::error_code rmEc;
        std::filesystem::remove(entry.path(), rmEc);
        if (rmEc) {
            return core::Result<std::vector<std::string>>::fail(
                core::ErrorCode::Internal,
                "delete key file failed: " + rmEc.message());
        }
        removed.push_back(account);
    }
    return core::Result<std::vector<std::string>>::ok(std::move(removed));
}

} // namespace platform::keychain
