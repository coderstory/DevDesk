// src/module_git/git_fs_walk.h
// 单层目录枚举纯函数（need291 Git 管理器重设计：自 GitTreeModel::walkChildren
// 迁出独立成文件——技术方案卡 doc25 §4「新增」行；F-03/F-04 复用通道）。
// 语义零变（迁移防回归口径，卡 §5）：目录类在前、按 name 字节序；.git 跳过；
// ignored 过滤（git 权威判定，判定失败按未忽略放行）；symlink 目录不跟随
// （按叶节点展示）；子仓库目录（含 .git）→ Subrepo。worker 侧跑，UI 线程零磁盘 IO。
// 注：git_tree_model.{h,cpp} 已随 need291 合并轮删除（walkChildren 本体在本文件）。
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "platform/git/git_repo.h"

namespace module::git::fs_walk {

// 条目类别 = 原 TreeRowKind 枚举输出子集（NoAccess/EmptyDir/Error 是渲染派生行，
// 不出自磁盘枚举）。Subrepo = 目录内含 .git（目录或 worktree 文件均算），禁展开降级。
enum class EntryKind { Directory, File, Subrepo };

struct FsEntry {
    std::string name;     // 路径末段显示名（UTF-8）
    std::string relPath;  // 相对 workdir 的 POSIX 路径（不带尾 '/'，UTF-8）
    EntryKind kind = EntryKind::File;
};

// 单层枚举结果（worker → model 通道）：权限失败单列（目录行降级），
// 其余失败带原因（行内错误行 + 重试）。
struct WalkResult {
    std::vector<FsEntry> entries;  // ok 时有效
    bool ok = false;
    bool permission = false;
    std::string message;
};

// 单层目录枚举（worker 侧跑，Repo 提供 isIgnored；repo == nullptr 时不过滤）。
// 结果按展示序排好（目录类在前）。POC①（need291）：单层枚举线性 ~1.6µs/条目，
// 展开任一层 <20ms——数据面「untracked 目录整体一条不递归」硬约束不破。
WalkResult walkChildren(platform::git::Repo* repo,
                        const std::filesystem::path& workdir,
                        const std::string& dirRelUtf8);

} // namespace module::git::fs_walk
