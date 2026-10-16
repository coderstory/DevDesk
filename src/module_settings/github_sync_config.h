// src/module_settings/github_sync_config.h
// GitHub Sync 配置模型 + 持久化（settings 模块内部）。
// 配置存储在 app_config 表中，键前缀 "github_sync:"。
// Token 存储在 platform::keychain（service="DevDesk/github", account="token"）。
#pragma once

#include <map>
#include <set>
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

    // ── 同步状态键（与配置键同前缀，但 save() 必须保留它们，见 .cpp 的 keep 判定）──
    // 首次方向（决策 #16）：首次同步成功后置位；repoUrl/branch 变更即清除。
    core::Result<bool> isFirstSyncDone() const;
    core::Result<void> setFirstSyncDone();
    core::Result<void> clearFirstSyncDone();

    // 已推送笔记登记（删除传播，决策 #19）：键 `github_sync:sha:notes/<名>`，
    // 值 = 推送成功时的远端 sha（JSON 字符串）。本地无记录的远端笔记不动
    // （保守：首次接管既有仓库时不误删别人的文件）。
    // 注意：这三个方法可能从同步后台线程调用，锁内触碰 db_ 句柄。
    core::Result<std::set<std::string>> syncedNoteNames() const;
    core::Result<void> recordSyncedNote(const std::string& fileName,
                                        const std::string& sha);
    core::Result<void> forgetSyncedNote(const std::string& fileName);

    // 笔记名 → 登记的远端 sha（F1 拉取比对用）。无登记 → 空表。
    core::Result<std::map<std::string, std::string>> syncedNoteShas() const;

    // database.sql 的远端 sha 登记：键 `github_sync:sha:data/database.sql`。
    // 拉取导入成功后登记；远端树 sha 与登记相等 → 远端库未变，跳过导入
    //（本地未推改动不被旧数据覆盖）。空串 = 无登记（首次/远端无此文件）。
    core::Result<std::string> syncedSqlSha() const;
    core::Result<void> recordSyncedSqlSha(const std::string& sha);

private:
    core::data::SqliteDb* db_;
};

} // namespace module::settings