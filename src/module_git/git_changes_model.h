// src/module_git/git_changes_model.h
// 中栏变更列表行模型（need:7 批次二；技术方案 §1 决策⑤ / UI 方案 A §5.3）：
// status 快照 → 变更行（路径去重合并双侧）。四态着色徽标（A/M/D/R + U 灰徽标）、
// 重命名两行式（主行新名 + 副行「← 旧名」）。
// bug#280②（老板口径 2026-11-16）：未跟踪非平行组——单一列表内联，ChangeGroup
// 仅存于行分类（视图单一组头，U 徽标承担状态区分；GitHub Desktop 同款惯例）。
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
    // 快照 → 变更行（路径去重合并双侧；按路径字节序）。纯函数便于 doctest 直接打快照。
    static std::vector<ChangeRow> build(const platform::git::StatusSnapshot& snapshot);

    // 组头文案：「变更 (5)」/「未跟踪 (2)」（Untracked 文案=防御位，单一组头下
    // 视图不再消费）
    static std::string groupHeader(ChangeGroup group, size_t count);
};

} // namespace module::git
