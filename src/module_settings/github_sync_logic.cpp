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

bool exitPushDue(int64_t lastCommitSec, int64_t nowSec) {
    if (lastCommitSec <= 0) return true;
    if (nowSec < lastCommitSec) return true;   // 时钟回拨：视为到点
    return nowSec - lastCommitSec >= kExitPushMinGapSec;
}

std::string backupFileName(int64_t tsSec) {
    return std::string(kBackupPrefix) + std::to_string(tsSec) + ".db";
}

std::string notesBackupDirName(int64_t tsSec) {
    return std::string(kNotesBackupPrefix) + std::to_string(tsSec);
}

int pruneBackups(const std::filesystem::path& dir, const std::string& prefix,
                 int keep, const std::string& suffix) {
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
        if (!suffix.empty()) {
            if (name.size() < suffix.size() ||
                name.compare(name.size() - suffix.size(), suffix.size(),
                             suffix) != 0) {
                continue;
            }
        }
        found.push_back(p);
    }
    // 名字序 = 时间序（<prefix><epoch> 定宽前缀后即时间戳）；同名宽度不同也不影响相对序
    std::sort(found.begin(), found.end(),
              [](const std::filesystem::path& a,
                 const std::filesystem::path& b) {
                  return a.filename().string() < b.filename().string();
              });
    int removed = 0;
    const int excess = static_cast<int>(found.size()) - keep;
    for (int i = 0; i < excess; ++i) {
        // remove_all：文件删自身，目录连内容一起删（笔记备份是目录）
        std::error_code rmEc;
        if (std::filesystem::remove_all(found[static_cast<size_t>(i)], rmEc) >
            0) {
            ++removed;
        }
    }
    return removed;
}

std::string remoteNotePath(const std::string& fileName) {
    return std::string(kRemoteNotesDir) + fileName;
}

PushAction pushActionForState(NoteSyncState state, bool remoteExists,
                              bool remoteShaMatches) {
    switch (state) {
        case NoteSyncState::InSync:         return PushAction::Skip;
        case NoteSyncState::New:            return PushAction::Put;
        case NoteSyncState::LocalModified:  return PushAction::Put;
        case NoteSyncState::RemoteModified: return PushAction::Skip;  // 远端领先，等 pull
        case NoteSyncState::Diverged:       return PushAction::Conflict;
        case NoteSyncState::LocalDeleted:
            // 远端没动（或无此文件）→ 删除传播；远端被改过 → 冲突，不静默删他人内容
            if (!remoteExists || remoteShaMatches) return PushAction::Delete;
            return PushAction::Conflict;
        case NoteSyncState::RemoteNew:
            return PushAction::Skip;   // push 枚举域（本地 ∪ 登记）不产生，防御性归 Skip
    }
    return PushAction::Skip;
}

PullDecision pullDecisionForState(NoteSyncState state, bool remoteShaMatches) {
    switch (state) {
        case NoteSyncState::New:
        case NoteSyncState::RemoteNew:
        case NoteSyncState::RemoteModified:
        case NoteSyncState::Diverged:
            return PullDecision::Download;
        case NoteSyncState::InSync:
        case NoteSyncState::LocalModified:   // 远端没变：本地改动保留，push 侧推
            return PullDecision::Skip;
        case NoteSyncState::LocalDeleted:
            // 远端没动 → 本地删除待 push 传播；远端也变过 → 下载恢复（死锁闭环
            // 的断点：恢复并更新登记后重判 InSync，push 不再撞 Conflict）。
            return remoteShaMatches ? PullDecision::Skip
                                    : PullDecision::Download;
    }
    return PullDecision::Skip;
}

std::vector<PullAction> pullNoteActions(
    const std::map<std::string, std::string>& remoteShas,
    const std::map<std::string, LocalNote>& local,
    const std::map<std::string, SyncedNoteRecord>& synced) {
    std::vector<PullAction> out;
    for (const auto& [name, sha] : remoteShas) {
        const auto rec = synced.find(name);
        const auto loc = local.find(name);
        const bool hasRecord = rec != synced.end();
        const bool localExists = loc != local.end();
        const bool localHashMatches = hasRecord && localExists &&
                                      !rec->second.contentHash.empty() &&
                                      rec->second.contentHash == loc->second.hash;
        const bool remoteShaMatches = hasRecord && rec->second.sha == sha;
        const auto state = classifyNote(hasRecord, localExists,
                                        localHashMatches, remoteShaMatches);
        if (pullDecisionForState(state, remoteShaMatches) ==
            PullDecision::Download) {
            out.push_back(PullAction{PullAction::Kind::Download, name});
        }
    }
    for (const auto& [name, rec] : synced) {
        (void)rec;
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
