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

// ── 状态 → 动作（唯一语义源：push/pull 都只经 classifyNote 判态后查这两张表）──
// push 动作：remoteExists = 远端树里有该名；remoteShaMatches = 远端 sha == 登记 sha。
enum class PushAction { Skip, Put, Delete, Conflict };

PushAction pushActionForState(NoteSyncState state, bool remoteExists,
                              bool remoteShaMatches);

// pull 判定：远端名逐个判态。remoteShaMatches = 远端 sha == 登记 sha（对
// LocalDeleted 是判据的关键输入：远端没动 → Skip 待 push 传播；远端也变过 →
// Download 恢复本地——不让位会丢远端新内容，且 push 对「本地删+远端改」判
// Conflict 并中止整轮，409 弹窗「拉取后再推」会原地打转成永久死锁）。
// 登记有而远端无的（远端已删）不走本表，由调用方归 KeepLocal + 清登记。
enum class PullDecision { Download, Skip };

PullDecision pullDecisionForState(NoteSyncState state, bool remoteShaMatches);

// 本地内容哈希：FNV-1a 64bit → 16 位小写 hex。纯件零依赖（不引 OpenSSL 进 logic），
// 同内容恒同值 —— 状态判定用内容而非 mtime（spec F1：mtime 跨设备/备份恢复不可靠）。
std::string contentHash(const std::string& bytes);

// ── 上传体积上限（spec F9）──
// Contents API PUT 单请求上限 100MB，base64 后体积 ×1.37 → 原始上限 ≈73MB
//（取十进制保守值）。不做分块：超限明确报错，等用户手动清理数据库。
inline constexpr uint64_t kMaxDumpRawBytes = 73'000'000;

bool dumpTooLargeForRemote(uint64_t rawBytes,
                           uint64_t limitBytes = kMaxDumpRawBytes);

// ── commit 节流（决策 #14 / #17）──
// 自动同步 5 分钟内最多一次；手动「立即同步」绕开窗口（调用方传 manual=true）。
inline constexpr int64_t kCommitThrottleSec = 5 * 60;

// 距上次 commit 是否已过节流窗口。lastCommitSec <= 0（从未 commit）→ 到点。
bool throttleDue(int64_t lastCommitSec, int64_t nowSec);

// ── 退出推送（spec F5）──
// dirty 且距上次 commit ≥ 60s 才在退出时补推：刚同步过的退出不必再推
//（改动已在远端），退出前的窗口期改动等下轮自动同步。
inline constexpr int64_t kExitPushMinGapSec = 60;

bool exitPushDue(int64_t lastCommitSec, int64_t nowSec);

// ── 备份（决策 #11 / F6）──
// 备份目录名（数据根下；该目录不进同步）。
inline constexpr const char* kBackupDirName = "backups";
inline constexpr const char* kBackupPrefix = "devdesk-";
// 笔记备份目录前缀（F6：拉取覆盖前整目录备份；与库备份同目录、前缀隔离、独立计数）。
inline constexpr const char* kNotesBackupPrefix = "notes-";
inline constexpr int kBackupKeep = 5;

// 备份文件名 `devdesk-<ts>.db`（ts = epoch 秒）。
std::string backupFileName(int64_t tsSec);

// 笔记备份目录名 `notes-<ts>`（ts = epoch 秒）。
std::string notesBackupDirName(int64_t tsSec);

// 保留最近 keep 份（按文件名时间序 = 名字序），返回删除数。
// 只处理 <prefix>* 且以 suffix 结尾的条目（suffix 空串 = 只要前缀匹配即可，
// 文件与目录都算，目录走 remove_all）；目录不存在 → 0（不是错误）。
int pruneBackups(const std::filesystem::path& dir, const std::string& prefix,
                 int keep, const std::string& suffix = ".db");

// 本地笔记快照条目：内容 + 其 contentHash（调用方算好传入，logic 不读盘）。
struct LocalNote {
    std::string content;
    std::string hash;   // contentHash(content)
};

// 同步登记条目（app_config 两键合成）：上次同步成功后的远端 sha + 当时本地内容 hash。
// hash 为空 = T3 时代的旧记录（只有 sha）→ 按本地改动处理（宁可重推一次，不漏推）。
struct SyncedNoteRecord {
    std::string sha;
    std::string contentHash;
};

// ── pull 侧动作（F7 笔记拉取）──
struct PullAction {
    enum class Kind { Download, KeepLocal };
    Kind kind = Kind::Download;
    std::string fileName;   // notes/ 下的文件名
};

// remoteShas = 远端树 notes/*.md 文件名 → sha；local = 本地笔记（名 → 内容+hash）；
// synced = 基线登记。远端名逐个经 classifyNote 判态 → pullDecisionForState：
// 远端变过（RemoteModified/Diverged）或无基线（New/RemoteNew）→ Download 覆盖本地；
// InSync/LocalModified（远端没变的本地改动保留，push 侧负责推）与 LocalDeleted
//（本地已删待传播，远端也变过时由 push 冲突路径让用户裁决）→ Skip。
// 登记有而远端树没有 = 远端已删 → KeepLocal（决策 #19 保守语义，调用方同时清登记）。
// 输出顺序稳定：名序 Download 在前，名序 KeepLocal 在后。
std::vector<PullAction> pullNoteActions(
    const std::map<std::string, std::string>& remoteShas,
    const std::map<std::string, LocalNote>& local,
    const std::map<std::string, SyncedNoteRecord>& synced);

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
