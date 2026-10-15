// src/module_settings/github_sync_config.h
// GitHub Sync 配置模型 + 持久化（settings 模块内部）。
// 配置存储在 app_config 表中，键前缀 "github_sync:"。
// Token 存储在 platform::keychain（service="DevDesk/github", account="token"）。
#pragma once

#include <string>

#include "core/result.h"
#include "core/sqlite_db.h"

namespace module::settings {

// GitHub Sync 配置
struct GithubSyncConfig {
    bool enabled = false;
    std::string repoUrl;      // https://github.com/user/repo.git
    std::string branch = "main";
    bool autoPush = true;
};

// GitHub Sync 配置持久化（用 app_config 表，键前缀 "github_sync:"）
class GithubSyncStore {
public:
    explicit GithubSyncStore(core::data::SqliteDb* db);

    // 读取配置（键缺失 → 默认值）
    GithubSyncConfig load() const;

    // 保存配置
    core::Result<void> save(const GithubSyncConfig& cfg);

    // 保存 Token 到系统凭据库
    static core::Result<void> saveToken(const std::string& token);
    // 读取 Token
    static core::Result<std::string> loadToken();
    // 删除 Token
    static core::Result<void> deleteToken();

    // Keychain service/account
    static constexpr const char* kKeychainService = "DevDesk/github";
    static constexpr const char* kKeychainAccount = "token";

private:
    core::data::SqliteDb* db_;
};

} // namespace module::settings