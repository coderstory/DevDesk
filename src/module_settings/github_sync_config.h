// src/module_settings/github_sync_config.h
// GitHub Sync 配置模型 + 持久化（settings 模块内部）。
// 配置存储在 app_config 表中，键前缀 "github_sync:"。
// Token 存在本机专属表 github_sync_state（行 key="token"，value = credential::protect
// 后形态；不进 dump，见 schema.h）。存储零钥匙串触达。
#pragma once

#include <cstdlib>
#include <map>
#include <string>

#include "core/result.h"
#include "core/sqlite_db.h"
#include "github_sync_logic.h"

namespace module::settings {

// ── 测试注入缝（真机验收用，见 tests/live/github_sync_live.cpp）──
// 同步层读 token 走数据库，而测试的 DEVDESK_DATA_ROOT 已隔离数据库 → 写入不会
// 污染用户真实数据。但真机验收仍可能想在库里无 token 时注入：这条缝让测试改由
// 环境变量供 token。
// **双变量门**：只有 DEVDESK_GITHUB_TEST_MODE=1 在场时才认 DEVDESK_GITHUB_TOKEN，
// 单有后者（用户环境里可能本来就有同名变量）不改变任何行为 —— 防生产误用。
inline bool githubTestModeEnabled() {
    const char* m = std::getenv("DEVDESK_GITHUB_TEST_MODE");
    return m != nullptr && std::string(m) == "1";
}

// 注入的 token；门未开或变量缺失 → 空串（= 未注入，走数据库）。
inline std::string githubTestToken() {
    if (!githubTestModeEnabled()) return {};
    const char* t = std::getenv("DEVDESK_GITHUB_TOKEN");
    return t == nullptr ? std::string() : std::string(t);
}

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

    // Token 三件套：存取删都落在 github_sync_state 的 token 行（protect 后形态）。
    // 测试注入缝见 githubTestToken / githubTestModeEnabled（saveToken / loadToken 各自先过缝）。
    core::Result<void> saveToken(const std::string& token);
    // 未存过 / 已删除 → fail(NotFound)，与「读了空串」可分辨（hasToken 判据靠它）
    core::Result<std::string> loadToken();
    core::Result<void> deleteToken();

    // ── 同步状态键（本机专属表 github_sync_state，键不带前缀；不进 dump / 备份）──
    // 首次方向：首次同步成功后置位；repoUrl/branch 变更即清除。
    core::Result<bool> isFirstSyncDone() const;
    core::Result<void> setFirstSyncDone();
    core::Result<void> clearFirstSyncDone();

    // 已推送笔记登记（删除传播 + 状态机基线）：github_sync_state 表内键
    // `sha:notes/<名>` 存远端 sha、`hash:notes/<名>` 存当时本地内容 hash（原样字符串）。
    // 本地无记录的远端笔记不动（保守：首次接管既有仓库时不误删别人的文件）。
    // 注意：这些方法可能从同步后台线程调用，锁内触碰 db_ 句柄。
    core::Result<std::map<std::string, gsync::SyncedNoteRecord>>
    syncedNoteRecords() const;
    core::Result<void> recordSyncedNote(const std::string& fileName,
                                        const std::string& sha,
                                        const std::string& contentHash);
    // 清 sha + hash 两键。
    core::Result<void> forgetSyncedNote(const std::string& fileName);

    // database.sql 的远端 sha 登记：github_sync_state 表内键 `sha:data/database.sql`。
    // 拉取导入成功后登记；远端树 sha 与登记相等 → 远端库未变，跳过导入
    //（本地未推改动不被旧数据覆盖）。空串 = 无登记（首次/远端无此文件）。
    core::Result<std::string> syncedSqlSha() const;
    core::Result<void> recordSyncedSqlSha(const std::string& sha);

    // 退出推送未完成的提示（下次启动卡片黄字「有未同步改动」；
    // 同步成功即清除）。github_sync_state 表内键 unsynced_notice。
    core::Result<bool> hasUnsyncedNotice() const;
    core::Result<void> setUnsyncedNotice();
    core::Result<void> clearUnsyncedNotice();

    // 导入远端 dump 覆盖本地库。导入前广播 db.import_begin（订阅方暂停
    // 后台写者），结束（无论成败）广播 db.import_end（订阅方恢复后台任务）；
    // 成功再广播 db.imported（全局重载）+ 重执行 PRAGMA foreign_keys=ON 与
    // integrity_check（dump 的 PRAGMA 是会话级的，恢复值不保证 = 应用不变量 ON）。
    // 失败回滚由 importFromString 的事务原子性承担（任一句失败整体回滚），
    // 不另做内存副本。
    core::Result<void> importDatabase(const std::string& sql);

private:
    core::data::SqliteDb* db_;
};

} // namespace module::settings