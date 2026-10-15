// src/module_settings/github_sync_logic.h
// GitHub Sync 编排层的纯件：不碰 Ui / 不碰网络 / 不碰 DB，判据全部可单测。
// 网络与 DB 的编排在 settings_module.cpp；本头只放「给定输入必然唯一输出」的规则。
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace module::settings::gsync {

// ── 远端布局（决策 #18）──
inline constexpr const char* kRemoteSqlPath = "data/database.sql";
inline constexpr const char* kRemoteNotesDir = "notes/";

// ── commit 节流（决策 #14 / #17）──
// 自动同步 5 分钟内最多一次；手动「立即同步」绕开窗口（调用方传 manual=true）。
inline constexpr int64_t kCommitThrottleSec = 5 * 60;

// 距上次 commit 是否已过节流窗口。lastCommitSec <= 0（从未 commit）→ 到点。
bool throttleDue(int64_t lastCommitSec, int64_t nowSec);

// ── 备份（决策 #11）──
// 备份目录名（数据根下；该目录不进同步）。
inline constexpr const char* kBackupDirName = "backups";
inline constexpr const char* kBackupPrefix = "devdesk-";
inline constexpr int kBackupKeep = 5;

// 备份文件名 `devdesk-<ts>.db`（ts = epoch 秒）。
std::string backupFileName(int64_t tsSec);

// 保留最近 keep 份（按文件名时间序 = 名字序），返回删除数。
// 只处理 <prefix>* 且以 .db 结尾的文件；目录不存在 → 0（不是错误）。
int pruneBackups(const std::filesystem::path& dir, const std::string& prefix,
                 int keep);

// ── 变更集（决策 #19）──
// 笔记同步的变更集：Put = 本地新增/修改；Delete = 本地已删 → 删远端同路径。
// 重命名天然表达为「删旧 + 建新」。
struct FileChange {
    enum class Kind { Put, Delete };
    Kind kind = Kind::Put;
    std::string path;     // 远端路径（notes/<name>）
    std::string content;  // 仅 Put 有值
};

// local: 文件名 → 内容；remote: 远端已有文件名集合。
// 输出顺序稳定（先按文件名字典序 Put，再按文件名字典序 Delete），便于断言与日志。
std::vector<FileChange> diffNotes(
    const std::map<std::string, std::string>& local,
    const std::set<std::string>& remote);

// 远端笔记文件名 → 远端路径。
std::string remoteNotePath(const std::string& fileName);

// ── 首次方向（决策 #16）──
// 首次启用同步时的方向判定：远端已有 database.sql → 先拉取覆盖（含备份）；
// 远端没有（分支或文件缺失）→ 以仓库默认分支为基点创建并首次上传；探测本身
// 出错（网络/认证）→ 不动作，把错误交给用户。
enum class FirstSyncChoice { PullThenPush, Push, Fail };

// sqlExists = 探测 database.sql 成功且存在；sqlMissing = 探测返回 NotFound。
FirstSyncChoice firstSyncChoice(bool sqlExists, bool sqlMissing);

// ── 提示文案（决策 #10 / #15）──
std::string branchCreatedHint(const std::string& branch);
inline constexpr const char* kLocalGitUnavailableHint = "本地版本历史：不可用";

} // namespace module::settings::gsync
