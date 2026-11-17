// src/module_git/git_fs_walk.cpp
// walkChildren 实现：自 git_tree_model.cpp:258-328 逐行迁出（语义零变），
// 仅 TreeRow/WalkResult → FsEntry/WalkResult 类型替换。
#include "git_fs_walk.h"

#include "platform/git/git_libgit2.h"

#include <algorithm>
#include <system_error>
#include <utility>

namespace module::git::fs_walk {

namespace {

// 展示排序：目录类（Directory/Subrepo）在前，其余（File）在后；
// 各段内按 name 字节序（对中文 UTF-8 稳定确定，两跑幂等——clue#22）。
int kindRank(EntryKind kind) {
    switch (kind) {
        case EntryKind::Directory:
        case EntryKind::Subrepo:
            return 0;
        default:
            return 1;
    }
}

bool entryLess(const FsEntry& a, const FsEntry& b) {
    const int ra = kindRank(a.kind);
    const int rb = kindRank(b.kind);
    if (ra != rb) return ra < rb;
    return a.name < b.name;
}

} // namespace

WalkResult walkChildren(platform::git::Repo* repo,
                        const std::filesystem::path& workdir,
                        const std::string& dirRelUtf8) {
    WalkResult result;
    std::vector<FsEntry>& out = result.entries;
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

        FsEntry row;
        row.name = name;
        row.relPath = rel;

        if (isDir) {
            // symlink 目录不跟随（spec §7 树·子层边界）：按叶节点展示
            std::error_code symEc;
            const bool isSymlink = std::filesystem::is_symlink(
                std::filesystem::symlink_status(entryPath, symEc));
            if (isSymlink) {
                row.kind = EntryKind::File;
                out.push_back(std::move(row));
                continue;
            }
            // 子仓库：目录内含 .git（目录或 worktree 文件均算）→ 禁展开降级（F-12）
            std::error_code subEc;
            const bool isSubrepo =
                std::filesystem::exists(entryPath / ".git", subEc);
            if (isSubrepo) {
                row.kind = EntryKind::Subrepo;
                out.push_back(std::move(row));
                continue;
            }
            row.kind = EntryKind::Directory;
        } else {
            row.kind = EntryKind::File;
        }
        // ignore 过滤（git 权威判定；判定失败按未忽略放行——宁可多显不少显）
        if (repo != nullptr) {
            auto ignored = repo->isIgnored(rel);
            if (ignored.isOk() && ignored.value()) continue;
        }
        out.push_back(std::move(row));
    }
    std::sort(out.begin(), out.end(), entryLess);
    result.ok = true;
    return result;
}

} // namespace module::git::fs_walk
