// src/module_git/git_tree_model.cpp
#include "git_tree_model.h"

#include "platform/git/git_libgit2.h"

#include <algorithm>
#include <system_error>

namespace module::git {

namespace {

// 展示排序：目录类（Directory/Subrepo/NoAccess）在前，其余（File）在后；
// 各段内按 name 字节序（对中文 UTF-8 稳定确定，两跑幂等——clue#22）。
int kindRank(TreeRowKind kind) {
    switch (kind) {
        case TreeRowKind::Directory:
        case TreeRowKind::Subrepo:
        case TreeRowKind::NoAccess:
            return 0;
        default:
            return 1;
    }
}

bool rowLess(const TreeRow& a, const TreeRow& b) {
    const int ra = kindRank(a.kind);
    const int rb = kindRank(b.kind);
    if (ra != rb) return ra < rb;
    return a.name < b.name;
}

} // namespace

char statusBadgeLetter(platform::git::StatusKind kind) {
    switch (kind) {
        case platform::git::StatusKind::New: return 'A';
        case platform::git::StatusKind::Modified: return 'M';
        case platform::git::StatusKind::Deleted: return 'D';
        case platform::git::StatusKind::Renamed: return 'R';
        case platform::git::StatusKind::Untracked: return 'U';
        case platform::git::StatusKind::Ignored: return 'I';
    }
    return '?';
}

void GitTreeModel::reset() {
    children_.clear();
    expanded_.clear();
    loading_.clear();
    loadErrors_.clear();
    noAccess_.clear();
    statusByPath_.clear();
    renameOld_.clear();
    changesRows_.clear();
}

void GitTreeModel::setMode(Mode mode) {
    if (mode_ == mode) return;
    mode_ = mode;
    if (mode_ == Mode::ChangesOnly) {
        rebuildChangesRows();
    }
}

void GitTreeModel::applyChildren(const std::string& dirRel,
                                 std::vector<TreeRow> entries) {
    std::sort(entries.begin(), entries.end(), rowLess);
    children_[dirRel] = std::move(entries);
    loading_.erase(dirRel);
    loadErrors_.erase(dirRel);
    noAccess_.erase(dirRel);
}

void GitTreeModel::markLoading(const std::string& dirRel) {
    loading_.insert(dirRel);
}

void GitTreeModel::markNoAccess(const std::string& dirRel) {
    loading_.erase(dirRel);
    loadErrors_.erase(dirRel);
    noAccess_.insert(dirRel);
    expanded_.erase(dirRel);  // 无权限行无展开符
}

void GitTreeModel::markLoadFailed(const std::string& dirRel,
                                  const std::string& message) {
    loading_.erase(dirRel);
    loadErrors_[dirRel] = message;
}

void GitTreeModel::clearLoadFailed(const std::string& dirRel) {
    loadErrors_.erase(dirRel);
}

void GitTreeModel::requestRetry(const std::string& dirRel) {
    // 清错误 → 回到待加载；pendingDirs() 下次 compose 周期重新投递
    loadErrors_.erase(dirRel);
    noAccess_.erase(dirRel);
}

void GitTreeModel::setExpanded(const std::string& dirRel, bool expanded) {
    if (expanded) {
        expanded_.insert(dirRel);
    } else {
        expanded_.erase(dirRel);
    }
}

bool GitTreeModel::isExpanded(const std::string& dirRel) const {
    return expanded_.count(dirRel) > 0;
}

void GitTreeModel::applyStatus(const platform::git::StatusSnapshot& snapshot) {
    statusByPath_.clear();
    renameOld_.clear();
    for (const auto& entry : snapshot.entries) {
        // 双侧（staged+unstaged）同路径出两条 → 合并展示一条，类别保留更「重」的
        //（重命名 > 删除 > 其余保序）；diff 基准是 worktree↔HEAD 单基准（§2.1），
        // 分类展示合并不构成双计数。
        const auto it = statusByPath_.find(entry.path);
        if (it != statusByPath_.end()) {
            if (entry.kind == platform::git::StatusKind::Renamed ||
                (it->second != platform::git::StatusKind::Renamed &&
                 entry.kind == platform::git::StatusKind::Deleted)) {
                it->second = entry.kind;
                if (!entry.oldPath.empty()) renameOld_[entry.path] = entry.oldPath;
            }
            continue;
        }
        statusByPath_[entry.path] = entry.kind;
        if (!entry.oldPath.empty()) renameOld_[entry.path] = entry.oldPath;
    }
    if (mode_ == Mode::ChangesOnly) {
        rebuildChangesRows();
    }
}

void GitTreeModel::rebuildChangesRows() {
    changesRows_.clear();
    changesRows_.reserve(statusByPath_.size());
    for (const auto& [path, kind] : statusByPath_) {
        TreeRow row;
        row.relPath = path;
        row.name = path;
        row.depth = 0;
        row.kind = TreeRowKind::File;
        row.status = kind;
        const auto slash = path.rfind('/');
        if (slash != std::string::npos) {
            row.name = path.substr(slash + 1);
        }
        const auto oldIt = renameOld_.find(path);
        if (oldIt != renameOld_.end()) row.oldPath = oldIt->second;
        changesRows_.push_back(std::move(row));
    }
}

std::vector<TreeRow> GitTreeModel::rows() const {
    if (mode_ == Mode::ChangesOnly) {
        return changesRows_;  // 平铺，无展开状态机
    }
    std::vector<TreeRow> out;
    // 递归 flatten（深度 = 目录深度，量级安全）：每行 = 模型状态派生，零缓存矩形
    //（clue#22 compose measure 两跑幂等）
    const auto emit = [this, &out](const auto& self, const std::string& dirRel,
                                   int depth) -> void {
        const auto it = children_.find(dirRel);
        if (it == children_.end()) return;
        for (const auto& entry : it->second) {
            TreeRow row = entry;
            row.depth = depth;
            const auto stIt = statusByPath_.find(row.relPath);
            if (stIt != statusByPath_.end()) row.status = stIt->second;
            const bool isDir = row.kind == TreeRowKind::Directory;
            // 无权限降级：目录行改 NoAccess（无展开符、单击无反应）
            if (isDir && noAccess_.count(row.relPath) > 0) {
                row.kind = TreeRowKind::NoAccess;
            }
            if (row.kind == TreeRowKind::Directory) {
                row.expanded = isExpanded(row.relPath);
                row.loading = loading_.count(row.relPath) > 0;
                row.childrenLoaded = children_.count(row.relPath) > 0;
            }
            // 深层分支展开前的行状态快照（row 随后 move 出去，不可再读）
            const std::string rel = row.relPath;
            const bool expanded = row.expanded;
            const bool loaded = row.childrenLoaded;
            const bool dirEmpty =
                row.kind == TreeRowKind::Directory && loaded &&
                children_.find(rel) != children_.end() &&
                children_.at(rel).empty();
            const auto errIt = loadErrors_.find(rel);
            const bool hasError =
                row.kind == TreeRowKind::Directory && expanded &&
                errIt != loadErrors_.end();

            out.push_back(std::move(row));
            if (!isDir || !expanded) continue;
            if (dirEmpty) {
                // 派生行 ①：展开后空目录提示行
                TreeRow hint;
                hint.kind = TreeRowKind::EmptyDir;
                hint.relPath = rel;
                hint.depth = depth + 1;
                out.push_back(std::move(hint));
            } else if (hasError) {
                // 派生行 ②：懒加载失败错误行（行下方插入，父级其余分支不受影响）
                TreeRow err;
                err.kind = TreeRowKind::Error;
                err.relPath = rel;
                err.depth = depth + 1;
                err.errorText = errIt->second;
                out.push_back(std::move(err));
            } else if (loaded) {
                self(self, rel, depth + 1);
            }
        }
    };
    if (children_.count(std::string()) > 0) {
        emit(emit, std::string(), 0);
    }
    return out;
}

std::vector<std::string> GitTreeModel::pendingDirs() const {
    std::vector<std::string> out;
    for (const auto& dirRel : expanded_) {
        if (children_.count(dirRel) == 0 && loading_.count(dirRel) == 0 &&
            loadErrors_.count(dirRel) == 0 && noAccess_.count(dirRel) == 0) {
            out.push_back(dirRel);
        }
    }
    // 根层未加载也算待投（首屏）
    if (children_.count(std::string()) == 0 &&
        loading_.count(std::string()) == 0) {
        out.push_back(std::string());
    }
    // 浅路径在前 → 首屏根层先回
    std::sort(out.begin(), out.end(),
              [](const std::string& a, const std::string& b) {
                  return a.size() < b.size();
              });
    return out;
}

WalkResult GitTreeModel::walkChildren(
    platform::git::Repo* repo, const std::filesystem::path& workdir,
    const std::string& dirRelUtf8) {
    WalkResult result;
    std::vector<TreeRow>& out = result.entries;
    const std::filesystem::path dir =
        dirRelUtf8.empty() ? workdir
                           : workdir / platform::git::utf8ToPath(dirRelUtf8);
    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec);
    if (ec) {
        result.ok = false;
        result.permission = ec == std::errc::permission_denied ||
                            ec == std::errc::operation_not_permitted;
        result.message = ec.message();
        return result;
    }
    result.ok = true;
    for (; it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (ec) break;  // 迭代中途失败：已收条目照常返回，剩余靠重试补
        ec.clear();
        const std::filesystem::path entryPath = it->path();
        const std::string name = platform::git::pathToUtf8(entryPath.filename());
        // 相对 workdir POSIX 路径（UTF-8 通道，F-10③ 禁窄构造）
        std::string rel = dirRelUtf8.empty() ? name : dirRelUtf8 + "/" + name;
        const bool isDir = it->is_directory(ec);
        if (ec) {
            ec.clear();
            continue;  // 竞态删除等：跳过该条目
        }
        // 根层 .git 管理目录恒隐藏
        if (dirRelUtf8.empty() && name == ".git") continue;

        TreeRow row;
        row.name = name;
        row.relPath = rel;

        if (isDir) {
            // symlink 目录不跟随（spec §7 树·子层边界）：按叶节点展示
            std::error_code symEc;
            const bool isSymlink = std::filesystem::is_symlink(
                std::filesystem::symlink_status(entryPath, symEc));
            if (isSymlink) {
                row.kind = TreeRowKind::File;
                out.push_back(std::move(row));
                continue;
            }
            // 子仓库：目录内含 .git（目录或 worktree 文件均算）→ 禁展开降级（F-12）
            std::error_code subEc;
            const bool isSubrepo =
                std::filesystem::exists(entryPath / ".git", subEc);
            if (isSubrepo) {
                row.kind = TreeRowKind::Subrepo;
                out.push_back(std::move(row));
                continue;
            }
            row.kind = TreeRowKind::Directory;
        } else {
            row.kind = TreeRowKind::File;
        }
        // ignore 过滤（git 权威判定；判定失败按未忽略放行——宁可多显不少显）
        if (repo != nullptr) {
            auto ignored = repo->isIgnored(rel);
            if (ignored.isOk() && ignored.value()) continue;
        }
        out.push_back(std::move(row));
    }
    std::sort(out.begin(), out.end(), rowLess);
    result.ok = true;
    return result;
}

} // namespace module::git
