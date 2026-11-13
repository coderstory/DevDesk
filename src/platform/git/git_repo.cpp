// src/platform/git/git_repo.cpp
// 见 git_repo.h（need:8 批次一；方案 §5 API 草案 + §2.1 口径）。
#include "platform/git/git_repo.h"

#include <git2.h>

#include "platform/git/git_libgit2.h"

namespace platform::git {

namespace {

struct RepoHandle {
    git_repository* repo = nullptr;
    ~RepoHandle() {
        if (repo != nullptr) git_repository_free(repo);
    }
};

// HEAD tree（WorkdirToHead / TreeToIndex 基准）。unborn 分支（空仓无 commit）→
// ok(nullptr) = 空树基准（全文件为新增）；其余 head 解析失败走错误链。
core::Result<git_tree*> resolveHeadTree(git_repository* repo) {
    git_reference* head = nullptr;
    const int rc = git_repository_head(&head, repo);
    if (rc == GIT_EUNBORNBRANCH) {
        return core::Result<git_tree*>::ok(nullptr);
    }
    if (rc < 0) {
        return core::Result<git_tree*>::fail(libgit2ToErrorCode(rc), libgit2ToMessage(rc));
    }
    git_commit* commit = nullptr;
    if (git_commit_lookup(&commit, repo, git_reference_target(head)) != 0) {
        git_reference_free(head);
        return core::Result<git_tree*>::fail(libgit2ToErrorCode(-1), libgit2ToMessage(-1));
    }
    git_tree* tree = nullptr;
    if (git_commit_tree(&tree, commit) != 0) {
        git_commit_free(commit);
        git_reference_free(head);
        return core::Result<git_tree*>::fail(libgit2ToErrorCode(-1), libgit2ToMessage(-1));
    }
    // tree 是独立对象（按 oid 引用 commit），commit 可先释放
    git_commit_free(commit);
    git_reference_free(head);
    return core::Result<git_tree*>::ok(tree);
}

// 侧别掩码（v1.9.7 status.h 无聚合宏，实读逐位拼）：index 侧五种 + workdir 侧六种
constexpr unsigned int kStagedSideMask =
    GIT_STATUS_INDEX_NEW | GIT_STATUS_INDEX_MODIFIED | GIT_STATUS_INDEX_DELETED |
    GIT_STATUS_INDEX_RENAMED | GIT_STATUS_INDEX_TYPECHANGE;
constexpr unsigned int kWorkdirSideMask =
    GIT_STATUS_WT_NEW | GIT_STATUS_WT_MODIFIED | GIT_STATUS_WT_DELETED |
    GIT_STATUS_WT_TYPECHANGE | GIT_STATUS_WT_RENAMED | GIT_STATUS_WT_UNREADABLE;

StatusKind kindFromIndexSide(unsigned int bits) {
    if (bits & GIT_STATUS_INDEX_NEW) return StatusKind::New;
    if (bits & GIT_STATUS_INDEX_DELETED) return StatusKind::Deleted;
    if (bits & GIT_STATUS_INDEX_RENAMED) return StatusKind::Renamed;
    return StatusKind::Modified;  // MODIFIED / TYPECHANGE
}

StatusKind kindFromWorkdirSide(unsigned int bits) {
    if (bits & GIT_STATUS_WT_NEW) return StatusKind::Untracked;
    if (bits & GIT_STATUS_WT_DELETED) return StatusKind::Deleted;
    if (bits & GIT_STATUS_WT_RENAMED) return StatusKind::Renamed;
    return StatusKind::Modified;  // MODIFIED / TYPECHANGE
}

// delta 侧文件路径：rename 用新名，删除时 new_file.path 可空退回 old_file.path
const char* deltaPath(const git_diff_delta* delta) {
    if (delta == nullptr) return nullptr;
    if (delta->new_file.path != nullptr) return delta->new_file.path;
    return delta->old_file.path;
}

std::string trimEol(const char* data, size_t len) {
    std::string s(data, len);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
        s.pop_back();
    }
    return s;
}

core::Result<DiffResult> buildDiff(git_repository* repo, DiffBasis basis,
                                   const std::string& pathFilter) {
    git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
    // §2.1：untracked 目录整体一个条目不递归（防 GitAhead #161 式大目录 hang）；
    // 二进制探测不跳过（patch.binary 标记供 F-11 占位判定）。
    opts.flags = GIT_DIFF_INCLUDE_UNTRACKED;
    std::string filter = pathFilter;
    if (!filter.empty()) {
        char* pathspec = filter.data();
        opts.pathspec.strings = &pathspec;
        opts.pathspec.count = 1;
    }

    git_tree* tree = nullptr;
    bool treeOwned = false;
    if (basis != DiffBasis::IndexToWorkdir) {
        auto head = resolveHeadTree(repo);
        if (head.isFail()) {
            return core::Result<DiffResult>::fail(head.errorCode(), head.errorMessage());
        }
        tree = head.valueOr(nullptr);
        treeOwned = tree != nullptr;
    }

    git_diff* diff = nullptr;
    int rc = -1;
    if (basis == DiffBasis::WorkdirToHead) {
        rc = git_diff_tree_to_workdir_with_index(&diff, repo, tree, &opts);
    } else if (basis == DiffBasis::TreeToIndex) {
        rc = git_diff_tree_to_index(&diff, repo, tree, nullptr, &opts);
    } else {
        rc = git_diff_index_to_workdir(&diff, repo, nullptr, &opts);
    }
    if (treeOwned) git_tree_free(tree);
    if (rc != 0 || diff == nullptr) {
        return core::Result<DiffResult>::fail(libgit2ToErrorCode(rc), libgit2ToMessage(rc));
    }

    // rename 检测（F-06② 旧名→新名）；显式 flags 不读 diff.renames 配置（确定性）
    git_diff_find_options findOpts = GIT_DIFF_FIND_OPTIONS_INIT;
    findOpts.flags = GIT_DIFF_FIND_RENAMES;
    git_diff_find_similar(diff, &findOpts);  // 失败不致命：退化为无 rename 标注的 diff

    DiffResult result;
    const size_t nDeltas = git_diff_num_deltas(diff);
    result.patches.reserve(nDeltas);
    for (size_t i = 0; i < nDeltas; ++i) {
        git_patch* patch = nullptr;
        if (git_patch_from_diff(&patch, diff, i) != 0 || patch == nullptr) {
            continue;  // 单 patch 失败不吞整个 diff（其余 patch 照常返回）
        }
        const git_diff_delta* delta = git_patch_get_delta(patch);
        DiffPatch p;
        p.oldPath = delta->old_file.path != nullptr ? delta->old_file.path : "";
        p.newPath = delta->new_file.path != nullptr ? delta->new_file.path : "";
        p.binary = (delta->flags & GIT_DIFF_FLAG_BINARY) != 0;
        p.isUntracked = delta->status == GIT_DELTA_UNTRACKED;

        const size_t nHunks = git_patch_num_hunks(patch);
        p.hunks.reserve(nHunks);
        for (size_t h = 0; h < nHunks; ++h) {
            const git_diff_hunk* hunk = nullptr;
            size_t nLines = 0;
            if (git_patch_get_hunk(&hunk, &nLines, patch, h) != 0 || hunk == nullptr) {
                continue;
            }
            DiffHunk outHunk;
            outHunk.header = hunk->header;
            outHunk.lines.reserve(nLines);
            for (size_t l = 0; l < nLines; ++l) {
                const git_diff_line* line = nullptr;
                if (git_patch_get_line_in_hunk(&line, patch, h, l) != 0 || line == nullptr) {
                    continue;
                }
                DiffLine outLine;
                outLine.origin = static_cast<char>(line->origin);
                if (line->old_lineno >= 0) outLine.oldNo = line->old_lineno;
                if (line->new_lineno >= 0) outLine.newNo = line->new_lineno;
                outLine.content = trimEol(line->content, line->content_len);
                outHunk.lines.push_back(std::move(outLine));
            }
            p.hunks.push_back(std::move(outHunk));
        }
        git_patch_free(patch);
        result.patches.push_back(std::move(p));
    }
    git_diff_free(diff);
    return core::Result<DiffResult>::ok(std::move(result));
}

} // namespace

struct Repo::Impl {
    RepoHandle handle;
    std::filesystem::path workdir;
};

Repo::Repo(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Repo::~Repo() = default;

Repo::Repo(Repo&& other) noexcept = default;

Repo& Repo::operator=(Repo&& other) noexcept = default;

core::Result<Repo> Repo::open(const std::filesystem::path& dir) {
    std::error_code ec;
    const bool isDir = std::filesystem::is_directory(dir, ec);
    if (ec) {
        // GCC/MinGW 对 ENOENT 也置 ec：先按 errno 分类，其余才归权限
        if (ec == std::errc::no_such_file_or_directory) {
            return core::Result<Repo>::fail(core::ErrorCode::FileNotFound,
                                            "目录不存在: " + pathToUtf8(dir));
        }
        return core::Result<Repo>::fail(core::ErrorCode::FilePermissionDenied,
                                        "无法访问目录: " + pathToUtf8(dir));
    }
    if (!isDir) {
        // 存在但不是目录（文件等）：直接按非仓报，与 F-01① 文案同因
        return core::Result<Repo>::fail(core::ErrorCode::ValidationError,
                                        "该目录不是 git 仓库: " + pathToUtf8(dir));
    }

    RepoHandle handle;
    // NO_SEARCH：不向上发现——F-01①② 错误三因以「所选目录本体」为准
    const int rc = git_repository_open_ext(&handle.repo, pathToUtf8(dir).c_str(),
                                           GIT_REPOSITORY_OPEN_NO_SEARCH, nullptr);
    if (rc != 0) {
        if (rc == GIT_ENOTFOUND) {
            return core::Result<Repo>::fail(core::ErrorCode::ValidationError,
                                            "该目录不是 git 仓库（" + libgit2ToMessage(rc) + "）");
        }
        return core::Result<Repo>::fail(libgit2ToErrorCode(rc), libgit2ToMessage(rc));
    }

    auto impl = std::make_unique<Impl>();
    impl->handle.repo = handle.repo;
    handle.repo = nullptr;  // 所有权移交 Impl
    // workdir 契约：UTF-8、'/' 分隔、结尾带 '/'——剥掉，与调用方传入的目录形态对齐
    std::string workdirUtf8 = git_repository_workdir(impl->handle.repo);
    if (!workdirUtf8.empty() && workdirUtf8.back() == '/') {
        workdirUtf8.pop_back();
    }
    impl->workdir = utf8ToPath(workdirUtf8);
    return core::Result<Repo>::ok(Repo(std::move(impl)));
}

const std::filesystem::path& Repo::workdir() const {
    return impl_->workdir;
}

core::Result<StatusSnapshot> Repo::status() {
    git_status_options opts = GIT_STATUS_OPTIONS_INIT;
    // §2.1：untracked 不递归（不开 RECURSE_UNTRACKED_DIRS）、排除 submodule 内部
    // 改动（F-12③）、双侧 rename 检测（F-06②）；ignored 不进 status（INCLUDE_IGNORED 不开）
    opts.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_EXCLUDE_SUBMODULES |
                 GIT_STATUS_OPT_RENAMES_HEAD_TO_INDEX | GIT_STATUS_OPT_RENAMES_INDEX_TO_WORKDIR;

    git_status_list* list = nullptr;
    const int rc = git_status_list_new(&list, impl_->handle.repo, &opts);
    if (rc != 0 || list == nullptr) {
        return core::Result<StatusSnapshot>::fail(libgit2ToErrorCode(rc), libgit2ToMessage(rc));
    }

    StatusSnapshot snapshot;
    const size_t n = git_status_list_entrycount(list);
    snapshot.entries.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const git_status_entry* e = git_status_byindex(list, i);
        if (e == nullptr) continue;
        const unsigned int bits = e->status;
        if (bits & GIT_STATUS_CONFLICTED) {
            // v1 只读：冲突条目不丢，按未暂存修改呈现（细分留给批次二）
            snapshot.entries.push_back(
                {e->head_to_index != nullptr && deltaPath(e->head_to_index) != nullptr
                     ? std::string(deltaPath(e->head_to_index))
                     : (e->index_to_workdir != nullptr && deltaPath(e->index_to_workdir) != nullptr
                            ? std::string(deltaPath(e->index_to_workdir))
                            : std::string()),
                 StatusKind::Modified, false});
            continue;
        }
        if (bits & kStagedSideMask) {
            const char* path = deltaPath(e->head_to_index);
            StatusEntry entry{path != nullptr ? std::string(path) : std::string(),
                              kindFromIndexSide(bits), true, {}};
            if (entry.kind == StatusKind::Renamed && e->head_to_index != nullptr &&
                e->head_to_index->old_file.path != nullptr) {
                entry.oldPath = e->head_to_index->old_file.path;
            }
            snapshot.entries.push_back(std::move(entry));
        }
        if (bits & kWorkdirSideMask) {
            const char* path = deltaPath(e->index_to_workdir);
            StatusEntry entry{path != nullptr ? std::string(path) : std::string(),
                              kindFromWorkdirSide(bits), false, {}};
            if (entry.kind == StatusKind::Renamed && e->index_to_workdir != nullptr &&
                e->index_to_workdir->old_file.path != nullptr) {
                entry.oldPath = e->index_to_workdir->old_file.path;
            }
            snapshot.entries.push_back(std::move(entry));
        }
    }
    git_status_list_free(list);
    return core::Result<StatusSnapshot>::ok(std::move(snapshot));
}

core::Result<DiffResult> Repo::diff(DiffBasis basis, const std::string& pathFilter) {
    return buildDiff(impl_->handle.repo, basis, pathFilter);
}

core::Result<bool> Repo::isIgnored(const std::string& path) {
    int ignored = 0;
    const int rc = git_ignore_path_is_ignored(&ignored, impl_->handle.repo, path.c_str());
    if (rc != 0) {
        return core::Result<bool>::fail(libgit2ToErrorCode(rc), libgit2ToMessage(rc));
    }
    return core::Result<bool>::ok(ignored != 0);
}

core::Result<DiffResult> Repo::blame(const std::string& path) {
    (void)path;
    return core::Result<DiffResult>::fail(core::ErrorCode::NotImplemented,
                                          "blame 未实现（v1 留接口，PRD 非目标 2）");
}

} // namespace platform::git
