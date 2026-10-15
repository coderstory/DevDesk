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

// ── 笔记同步状态机（spec F1）──
// 状态 = f(登记记录, 本地现状, 远端现状)；纯件判定，编排层按状态选动作。
enum class NoteSyncState {
    InSync,          // 记录有 && 本地 hash==记录 && 远端 sha==记录 → 跳过
    New,             // 记录无 && 本地有 → 本地领先，推送 PUT
    LocalModified,   // 记录有 && 本地 hash≠记录 && 远端 sha==记录 → PUT（shaIfMatch=记录 sha）
    RemoteModified,  // 记录有 && 本地 hash==记录 && 远端 sha≠记录 → 拉取覆盖
    Diverged,        // 记录有 && 本地 hash≠记录 && 远端 sha≠记录 → 409 弹窗路径
    LocalDeleted,    // 记录有 && 本地无 → 删除传播 DELETE
    RemoteNew,       // 记录无 && 本地无 → 仅 pull 场景有意义（远端树里有该名）
};

// 跨文件不变量：调用方枚举域保证「hasRecord=false && localExists=false」的输入
// 必然来自远端树枚举（pull 遍历 远端树 ∪ 登记名），push 侧枚举（本地文件 ∪ 登记）
// 不产生该组合；全假输入不在契约内，归 RemoteNew 仅为全定义域。
NoteSyncState classifyNote(bool hasRecord, bool localExists,
                           bool localHashMatches, bool remoteShaMatches);

// 本地内容哈希：FNV-1a 64bit → 16 位小写 hex。纯件零依赖（不引 OpenSSL 进 logic），
// 同内容恒同值 —— 状态判定用内容而非 mtime（spec F1：mtime 跨设备/备份恢复不可靠）。
std::string contentHash(const std::string& bytes);

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
