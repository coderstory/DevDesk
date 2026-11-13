// src/module_git/git_changes_model.cpp
#include "git_changes_model.h"

#include <algorithm>
#include <map>

namespace module::git {

namespace {

ChangeGroup groupOf(platform::git::StatusKind kind) {
    return kind == platform::git::StatusKind::Untracked ? ChangeGroup::Untracked
                                                        : ChangeGroup::Changed;
}

// 同路径多条目（staged+unstaged 双侧）合并保一条：类别按「重命名 > 删除 > 新增 >
// 修改」取最重；与 diff 基准（worktree↔HEAD 单基准，§2.1）同口径不双计数。
int severityOf(platform::git::StatusKind kind) {
    switch (kind) {
        case platform::git::StatusKind::Renamed: return 4;
        case platform::git::StatusKind::Deleted: return 3;
        case platform::git::StatusKind::New: return 2;
        case platform::git::StatusKind::Modified: return 1;
        default: return 0;  // Untracked/Ignored 不参与合并竞争
    }
}

} // namespace

std::vector<ChangeRow> GitChangesModel::build(
    const platform::git::StatusSnapshot& snapshot) {
    std::map<std::string, ChangeRow> byPath;  // map = 路径字节序输出
    for (const auto& entry : snapshot.entries) {
        if (entry.path.empty()) continue;
        ChangeRow row;
        row.path = entry.path;
        row.oldPath = entry.oldPath;
        row.kind = entry.kind;
        row.group = groupOf(entry.kind);
        const auto it = byPath.find(entry.path);
        if (it == byPath.end()) {
            byPath.emplace(entry.path, std::move(row));
            continue;
        }
        if (severityOf(entry.kind) > severityOf(it->second.kind)) {
            ChangeRow merged = row;
            if (merged.oldPath.empty()) merged.oldPath = it->second.oldPath;
            it->second = std::move(merged);
        } else if (it->second.oldPath.empty()) {
            it->second.oldPath = row.oldPath;
        }
    }
    std::vector<ChangeRow> out;
    out.reserve(byPath.size());
    for (auto& [path, row] : byPath) {
        (void)path;
        out.push_back(std::move(row));
    }
    return out;
}

std::string GitChangesModel::groupHeader(ChangeGroup group, size_t count) {
    const char* label = group == ChangeGroup::Changed ? "变更" : "未跟踪";
    return std::string(label) + " (" + std::to_string(count) + ")";
}

} // namespace module::git
