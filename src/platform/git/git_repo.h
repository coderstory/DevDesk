// src/platform/git/git_repo.h
// libgit2 Repo RAII 包装（PIMPL）：open / status / diff / isIgnored / blame
// （git 管理器只读 v1，池 need:8 批次一；方案 §5 API 草案落地）。
// 头文件零 git2 类型（Impl 前置声明），git2.h 只在 git_repo.cpp。
#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/result.h"

namespace platform::git {

// status/diff 口径（方案 §2.1）：untracked 目录整体一个条目不递归、排除 submodule、
// 开 rename 检测；ignored 不进 status（树模式逐条目走 isIgnored）。
enum class StatusKind { New, Modified, Deleted, Renamed, Untracked, Ignored };

struct StatusEntry {
    std::string path;   // 相对 workdir，POSIX '/' 分隔，UTF-8
    StatusKind kind;
    bool staged;        // 分类着色用：true = head→index 侧（已暂存）；同一文件双侧有变
                        // 时出两条（staged/unstaged 各一）
    // Renamed 条目的旧路径（delta old_file，UTF-8 POSIX）；非重命名恒空串
    //（need:7 批次二 F-06② 旧名→新名展示通道）
    std::string oldPath;
};

struct StatusSnapshot {
    std::vector<StatusEntry> entries;
};

// v1 用 WorkdirToHead（worktree↔HEAD 单基准，穿过 index；§2.1 落定）。
// 增量批次 staged/unstaged 二分时改传 TreeToIndex + IndexToWorkdir 双次。
enum class DiffBasis { WorkdirToHead, TreeToIndex, IndexToWorkdir };

struct DiffLine {
    char origin;                        // '+' 增 / '-' 删 / ' ' 上下文
    std::optional<int> oldNo;           // 1 起；该侧无行时 nullopt
    std::optional<int> newNo;
    std::string content;                // 不含行尾换行（\r\n 一并剥）
};

struct DiffHunk {
    std::string header;                 // @@ -a,b +c,d @@ 原文
    std::vector<DiffLine> lines;
};

struct DiffPatch {
    std::string oldPath;
    std::string newPath;
    bool binary = false;                // need:10 批次三补默认值（原裸 bool 读不确定值）
    bool isUntracked = false;           // untracked 文件 = 全增行；目录整体一条不递归
    std::vector<DiffHunk> hunks;
};

struct DiffResult {
    std::vector<DiffPatch> patches;
};

class Repo {
public:
    // 严格按 dir 开仓（GIT_REPOSITORY_OPEN_NO_SEARCH，不向上发现）：非 git 仓 →
    // fail(ValidationError)；目录不存在 → fail(FileNotFound)；无权限 →
    // fail(FilePermissionDenied)。上层需「选了仓内子目录」语义时自行用
    // git_repository_discover 预解析父仓再进本接口。
    static core::Result<Repo> open(const std::filesystem::path& dir);

    ~Repo();
    Repo(Repo&& other) noexcept;
    Repo& operator=(Repo&& other) noexcept;
    Repo(const Repo&) = delete;
    Repo& operator=(const Repo&) = delete;

    const std::filesystem::path& workdir() const;

    // flags 见 StatusKind 注释（untracked 不递归、EXCLUDE_SUBMODULES、双侧 rename）
    core::Result<StatusSnapshot> status();

    // basis 见 DiffBasis 注释；pathFilter 非空时按该 pathspec 过滤（相对 workdir）
    core::Result<DiffResult> diff(DiffBasis basis, const std::string& pathFilter = {});

    // git2/ignore.h 路径判定（相对 workdir 的 POSIX 路径）
    core::Result<bool> isIgnored(const std::string& path);

    // v1 留接口（PRD 非目标 2）
    core::Result<DiffResult> blame(const std::string& path);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit Repo(std::unique_ptr<Impl> impl);
};

} // namespace platform::git
