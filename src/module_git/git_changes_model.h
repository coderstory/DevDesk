// src/module_git/git_changes_model.h
// 中栏变更列表行模型（need:7 批次二；技术方案 §1 决策⑤ / UI 方案 A §5.3）：
// status 快照 → 「变更 / 未跟踪」两组行。四态着色徽标（A/M/D/R + U 灰徽标）、
// 重命名两行式（主行新名 + 副行「← 旧名」）。
// 渲染纪律（clue#22）：组行/条目行数据 = 快照派生纯函数，视图按（行, 宽度）现算
// 行盒，两跑幂等。
#pragma once

#include <string>
#include <vector>

#include "platform/git/git_repo.h"

namespace module::git {

enum class ChangeGroup { Changed, Untracked };

struct ChangeRow {
    std::string path;      // 相对 workdir POSIX 路径（显示 = 全相对路径，IDEA 惯例）
    std::string oldPath;   // 重命名旧路径；其余行空
    platform::git::StatusKind kind;
    ChangeGroup group;
};

class GitChangesModel {
public:
    // 快照 → 两组行（路径去重合并双侧；未跟踪独立组；组内按路径字节序）。
    // 纯函数便于 doctest 直接打快照。
    static std::vector<ChangeRow> build(const platform::git::StatusSnapshot& snapshot);

    // 组头文案：「变更 (5)」/「未跟踪 (2)」
    static std::string groupHeader(ChangeGroup group, size_t count);
};

} // namespace module::git
