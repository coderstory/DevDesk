// src/module_settings/github_sync_logic.cpp — 纯件实现（边界见 .h）。
#include "github_sync_logic.h"

#include <algorithm>
#include <system_error>

namespace module::settings::gsync {

NoteSyncState classifyNote(bool hasRecord, bool localExists,
                           bool localHashMatches, bool remoteShaMatches) {
    if (!hasRecord) {
        return localExists ? NoteSyncState::New : NoteSyncState::RemoteNew;
    }
    if (!localExists) return NoteSyncState::LocalDeleted;
    if (localHashMatches) {
        return remoteShaMatches ? NoteSyncState::InSync
                                : NoteSyncState::RemoteModified;
    }
    return remoteShaMatches ? NoteSyncState::LocalModified
                            : NoteSyncState::Diverged;
}

std::string contentHash(const std::string& bytes) {
    uint64_t h = 14695981039346656037ull;   // FNV-1a 64 offset basis
    for (const unsigned char c : bytes) {
        h ^= c;
        h *= 1099511628211ull;              // FNV-1a 64 prime
    }
    static const char* kHex = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<size_t>(i)] = kHex[h & 0xF];
        h >>= 4;
    }
    return out;
}

bool dumpTooLargeForRemote(uint64_t rawBytes, uint64_t limitBytes) {
    return rawBytes > limitBytes;
}

bool throttleDue(int64_t lastCommitSec, int64_t nowSec) {
    if (lastCommitSec <= 0) return true;   // 从未 commit → 到点
    if (nowSec < lastCommitSec) return true;  // 时钟回拨：不把用户锁死在窗口里
    return nowSec - lastCommitSec >= kCommitThrottleSec;
}

std::string backupFileName(int64_t tsSec) {
    return std::string(kBackupPrefix) + std::to_string(tsSec) + ".db";
}

int pruneBackups(const std::filesystem::path& dir, const std::string& prefix,
                 int keep) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return 0;   // 无目录 = 无备份可删
    if (keep < 0) keep = 0;

    std::vector<std::filesystem::path> found;
    for (std::filesystem::directory_iterator it(dir, ec), end; it != end;
         it.increment(ec)) {
        if (ec) break;
        const auto& p = it->path();
        const std::string name = p.filename().string();
        if (name.size() <= prefix.size()) continue;
        if (name.compare(0, prefix.size(), prefix) != 0) continue;
        if (name.size() < 3 || name.compare(name.size() - 3, 3, ".db") != 0) {
            continue;
        }
        found.push_back(p);
    }
    // 名字序 = 时间序（devdesk-<epoch>.db 定宽前缀后即时间戳）；同名宽度不同也不影响相对序
    std::sort(found.begin(), found.end(),
              [](const std::filesystem::path& a,
                 const std::filesystem::path& b) {
                  return a.filename().string() < b.filename().string();
              });
    int removed = 0;
    const int excess = static_cast<int>(found.size()) - keep;
    for (int i = 0; i < excess; ++i) {
        std::error_code rmEc;
        if (std::filesystem::remove(found[static_cast<size_t>(i)], rmEc)) {
            ++removed;
        }
    }
    return removed;
}

std::string remoteNotePath(const std::string& fileName) {
    return std::string(kRemoteNotesDir) + fileName;
}

std::vector<FileChange> diffNotes(
    const std::map<std::string, std::string>& local,
    const std::set<std::string>& remote) {
    std::vector<FileChange> out;
    // 新增 / 修改（本地有 → 一律 PUT；内容是否变化由调用方按 sha 判定，这里不做内容比较）
    for (const auto& [name, content] : local) {
        out.push_back(FileChange{FileChange::Kind::Put, remoteNotePath(name),
                                 content});
    }
    // 删除传播：远端有、本地没有 → DELETE
    for (const auto& name : remote) {
        if (local.find(name) != local.end()) continue;
        out.push_back(FileChange{FileChange::Kind::Delete, remoteNotePath(name),
                                 {}});
    }
    return out;
}

std::vector<PullAction> pullNoteActions(
    const std::map<std::string, std::string>& remoteShas,
    const std::map<std::string, std::string>& syncedShas) {
    std::vector<PullAction> out;
    for (const auto& [name, sha] : remoteShas) {
        const auto it = syncedShas.find(name);
        if (it != syncedShas.end() && it->second == sha) continue;   // 远端未变
        out.push_back(PullAction{PullAction::Kind::Download, name});
    }
    for (const auto& [name, sha] : syncedShas) {
        (void)sha;
        if (remoteShas.find(name) != remoteShas.end()) continue;
        out.push_back(PullAction{PullAction::Kind::KeepLocal, name});
    }
    return out;
}

FirstSyncChoice firstSyncChoice(bool sqlExists, bool sqlMissing) {
    if (sqlExists) return FirstSyncChoice::PullThenPush;
    if (sqlMissing) return FirstSyncChoice::Push;
    return FirstSyncChoice::Fail;
}

std::string branchCreatedHint(const std::string& branch) {
    return "分支 " + branch + " 不存在，已按仓库默认分支自动创建";
}

} // namespace module::settings::gsync
