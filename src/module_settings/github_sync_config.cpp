// src/module_settings/github_sync_config.cpp
// GitHub Sync 配置持久化实现。
#include "github_sync_config.h"

#include "core/app_config.h"
#include "core/logging.h"
#include "core/result.h"
#include "platform/json.h"
#include "platform/keychain.h"

namespace module::settings {

namespace {
constexpr const char* kKeyEnabled = "github_sync:enabled";
constexpr const char* kKeyRepoUrl = "github_sync:repo_url";
constexpr const char* kKeyBranch = "github_sync:branch";
constexpr const char* kKeyAutoPush = "github_sync:auto_push";
} // namespace

GithubSyncStore::GithubSyncStore(core::data::SqliteDb* db) : db_(db) {}

GithubSyncConfig GithubSyncStore::load() const {
    GithubSyncConfig cfg;
    if (db_ == nullptr) return cfg;

    auto rows = core::appconfig::listWithPrefix(*db_, "github_sync:");
    if (rows.isFail()) {
        LOG_WARN("GithubSync: 读取配置失败: " + rows.errorMessage());
        return cfg;
    }

    for (const auto& entry : rows.value()) {
        if (entry.key == kKeyEnabled) {
            auto v = platform::json::parse(entry.value);
            if (v.isOk() && v.value().isBool()) {
                cfg.enabled = v.value().asBool();
            }
        } else if (entry.key == kKeyRepoUrl) {
            auto v = platform::json::parse(entry.value);
            if (v.isOk() && v.value().isString()) {
                cfg.repoUrl = v.value().asString();
            }
        } else if (entry.key == kKeyBranch) {
            auto v = platform::json::parse(entry.value);
            if (v.isOk() && v.value().isString()) {
                cfg.branch = v.value().asString();
            }
        } else if (entry.key == kKeyAutoPush) {
            auto v = platform::json::parse(entry.value);
            if (v.isOk() && v.value().isBool()) {
                cfg.autoPush = v.value().asBool();
            }
        }
    }

    // 默认值
    if (cfg.branch.empty()) cfg.branch = "main";

    return cfg;
}

core::Result<void> GithubSyncStore::save(const GithubSyncConfig& cfg) {
    if (db_ == nullptr) {
        return core::Result<void>::fail(
            core::ErrorCode::NotFound,
            "应用数据库未就绪，GitHub Sync 配置未保存");
    }

    std::vector<core::appconfig::Entry> entries;
    entries.push_back({kKeyEnabled,
                       platform::json::stringify(
                           platform::json::makeBool(cfg.enabled)).valueOr("false"),
                       0});
    entries.push_back({kKeyRepoUrl,
                       platform::json::stringify(
                           platform::json::makeString(cfg.repoUrl)).valueOr("\"\""),
                       0});
    entries.push_back({kKeyBranch,
                       platform::json::stringify(
                           platform::json::makeString(cfg.branch)).valueOr("\"main\""),
                       0});
    entries.push_back({kKeyAutoPush,
                       platform::json::stringify(
                           platform::json::makeBool(cfg.autoPush)).valueOr("true"),
                       0});

    return db_->transaction([&]() -> core::Result<void> {
        auto wrote = core::appconfig::setMany(*db_, entries);
        if (wrote.isFail()) return wrote;

        // 清理旧的 github_sync 键
        std::vector<std::string> keep = {
            kKeyEnabled, kKeyRepoUrl, kKeyBranch, kKeyAutoPush};
        return core::appconfig::eraseWithPrefixExcept(*db_, "github_sync:", keep);
    });
}

core::Result<void> GithubSyncStore::saveToken(const std::string& token) {
    return platform::keychain::storeSecret(kKeychainService, kKeychainAccount, token);
}

core::Result<std::string> GithubSyncStore::loadToken() {
    return platform::keychain::loadSecret(kKeychainService, kKeychainAccount);
}

core::Result<void> GithubSyncStore::deleteToken() {
    return platform::keychain::deleteSecret(kKeychainService, kKeychainAccount);
}

} // namespace module::settings