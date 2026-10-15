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
constexpr const char* kKeyFirstSyncDone = "github_sync:first_sync_done";
constexpr const char* kKeyShaPrefix = "github_sync:sha:";
constexpr const char* kKeyShaNotesPrefix = "github_sync:sha:notes/";

std::string jsonString(const std::string& v) {
    return platform::json::stringify(platform::json::makeString(v))
        .valueOr("\"\"");
}
} // namespace

GithubSyncStore::GithubSyncStore(core::data::SqliteDb* db) : db_(db) {}

GithubSyncConfig GithubSyncStore::load() const {
    GithubSyncConfig cfg;
    if (db_ == nullptr) return cfg;

    // 锁纪律：同步后台线程会写 github_sync:sha:notes/*，读侧同锁。
    std::lock_guard<std::recursive_mutex> lock(db_->mutex());
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
    entries.push_back({kKeyRepoUrl, jsonString(cfg.repoUrl), 0});
    entries.push_back({kKeyBranch, jsonString(cfg.branch), 0});
    entries.push_back({kKeyAutoPush,
                       platform::json::stringify(
                           platform::json::makeBool(cfg.autoPush)).valueOr("true"),
                       0});

    // 锁纪律：save 可能与同步后台线程的 record/forget 并发触碰同一连接。
    std::lock_guard<std::recursive_mutex> lock(db_->mutex());
    return db_->transaction([&]() -> core::Result<void> {
        auto wrote = core::appconfig::setMany(*db_, entries);
        if (wrote.isFail()) return wrote;

        // 只清本函数写过的 4 个配置键；sha:notes/*（删除传播登记）与
        // first_sync_done 是同步状态，config 保存绝不能清它们。
        std::vector<std::string> drop;
        auto rows = core::appconfig::listWithPrefix(*db_, "github_sync:");
        if (rows.isFail()) {
            return core::Result<void>::fail(rows.errorCode(),
                                            rows.errorMessage());
        }
        for (const auto& e : rows.value()) {
            const bool isConfigKey = e.key == kKeyEnabled ||
                                     e.key == kKeyRepoUrl ||
                                     e.key == kKeyBranch ||
                                     e.key == kKeyAutoPush;
            if (isConfigKey) continue;
            if (e.key.rfind(kKeyShaPrefix, 0) == 0) continue;
            if (e.key == kKeyFirstSyncDone) continue;
            drop.push_back(e.key);
        }
        for (const auto& key : drop) {
            auto erased = core::appconfig::erase(*db_, key);
            if (erased.isFail()) return erased;
        }
        return core::Result<void>::ok();
    });
}

core::Result<void> GithubSyncStore::saveToken(const std::string& token) {
    return platform::keychain::storeSecret(kKeychainService, kKeychainAccount, token);
}

core::Result<bool> GithubSyncStore::isFirstSyncDone() const {
    if (db_ == nullptr) return core::Result<bool>::ok(false);
    std::lock_guard<std::recursive_mutex> lock(db_->mutex());
    auto v = core::appconfig::get(*db_, kKeyFirstSyncDone);
    if (v.isFail()) return core::Result<bool>::ok(false);   // 键缺失 = 未做过
    return core::Result<bool>::ok(v.value() == "true");
}

core::Result<void> GithubSyncStore::setFirstSyncDone() {
    if (db_ == nullptr) {
        return core::Result<void>::fail(core::ErrorCode::NotFound,
                                        "应用数据库未就绪");
    }
    std::lock_guard<std::recursive_mutex> lock(db_->mutex());
    return core::appconfig::set(*db_, kKeyFirstSyncDone, "true");
}

core::Result<void> GithubSyncStore::clearFirstSyncDone() {
    if (db_ == nullptr) return core::Result<void>::ok();
    std::lock_guard<std::recursive_mutex> lock(db_->mutex());
    return core::appconfig::erase(*db_, kKeyFirstSyncDone);
}

core::Result<std::set<std::string>> GithubSyncStore::syncedNoteNames() const {
    std::set<std::string> names;
    if (db_ == nullptr) return core::Result<std::set<std::string>>::ok(names);
    std::lock_guard<std::recursive_mutex> lock(db_->mutex());
    auto rows = core::appconfig::listWithPrefix(*db_, kKeyShaNotesPrefix);
    if (rows.isFail()) {
        return core::Result<std::set<std::string>>::fail(rows.errorCode(),
                                                         rows.errorMessage());
    }
    for (const auto& e : rows.value()) {
        names.insert(e.key.substr(std::string(kKeyShaNotesPrefix).size()));
    }
    return core::Result<std::set<std::string>>::ok(std::move(names));
}

core::Result<void> GithubSyncStore::recordSyncedNote(const std::string& fileName,
                                                     const std::string& sha) {
    if (db_ == nullptr) {
        return core::Result<void>::fail(core::ErrorCode::NotFound,
                                        "应用数据库未就绪");
    }
    std::lock_guard<std::recursive_mutex> lock(db_->mutex());
    return core::appconfig::set(*db_, std::string(kKeyShaNotesPrefix) + fileName,
                                jsonString(sha));
}

core::Result<void> GithubSyncStore::forgetSyncedNote(const std::string& fileName) {
    if (db_ == nullptr) return core::Result<void>::ok();
    std::lock_guard<std::recursive_mutex> lock(db_->mutex());
    return core::appconfig::erase(*db_,
                                  std::string(kKeyShaNotesPrefix) + fileName);
}

core::Result<std::string> GithubSyncStore::loadToken() {
    return platform::keychain::loadSecret(kKeychainService, kKeychainAccount);
}

core::Result<void> GithubSyncStore::deleteToken() {
    return platform::keychain::deleteSecret(kKeychainService, kKeychainAccount);
}

} // namespace module::settings