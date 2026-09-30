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

// 真实实现（CPX-14 起经 keychain.h 的函数指针表转发，见那里的「注入缝」段）
core::Result<void> realStoreSecret(const std::string& service, const std::string& account, const std::string& secret) {
    auto path = getKeyFilePath(service, account);
    std::ofstream f(path, std::ios::binary);
    if (!f) return core::Result<void>::fail(core::ErrorCode::FileWriteFailed, "cannot write key file");
    f << secret;
    f.close();
    // 写完收紧为 0600（审计 2026-09-23 [29]）：默认 umask 022 下新建文件为
    // 0644，多用户机器上其他本地用户可直读明文 API key。perm_options::replace
    // 将权限整体覆盖为 rw-------。用 error_code 重载（项目约定不抛异常）；
    // chmod 失败时删掉已落盘的密钥并按失败返回——宁可保存失败，也不留下
    // 他人可读的明文密钥（调用方收到失败后重试会重新写入，不会丢失状态）。
    std::error_code ec;
    std::filesystem::permissions(
        path,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, ec);
    if (ec) {
        std::error_code rmEc;
        std::filesystem::remove(path, rmEc);
        return core::Result<void>::fail(
            core::ErrorCode::FilePermissionDenied,
            "cannot chmod key file to 0600: " + ec.message());
    }
    return core::Result<void>::ok();
}

core::Result<std::string> realLoadSecret(const std::string& service, const std::string& account) {
    auto path = getKeyFilePath(service, account);
    std::ifstream f(path);
    if (!f) return core::Result<std::string>::fail(core::ErrorCode::NotFound, "key file not found");
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return core::Result<std::string>::ok(std::move(s));
}

core::Result<void> realDeleteSecret(const std::string& service, const std::string& account) {
    auto path = getKeyFilePath(service, account);
    // tech-debt D：原先 ec 被吞、无条件返回 ok，删除失败（权限/只读挂载/IO）
    // 调用方无从感知。对齐 keychain_win 语义：不存在 → remove 返回 false 且
    // 不置 ec → Ok（幂等删除）；真实失败 → Internal 带上 error_code 消息。
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (ec) {
        return core::Result<void>::fail(
            core::ErrorCode::Internal,
            "delete key file failed: " + ec.message());
    }
    return core::Result<void>::ok();
}

core::Result<std::vector<std::string>> realDeleteSecretsNotIn(
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
        // P0 护栏②：白名单前缀（claude:）之外的 account 一律保留——
        // shouldDeleteOrphan 统一决策，见 keychain.h 说明
        if (!shouldDeleteOrphan(account, keep)) continue;
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
