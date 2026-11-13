// src/module_git/git_diff_model.h
// diff unified 行模型（need:10 批次三；技术方案 §6 + UI 方案 A §5.6）：
// DiffPatch → 「hunk 头 / 折叠条 / diff 行」三类行的扁平序列（virtualList 定高槽）。
// 折叠状态在模型内（内容锚 = hunk 下标，clue#14：折叠后滚动恢复用内容锚不用偏移量）；
// 行序列仅 patch/折叠态变更时重建一次，virtualList 按下标取 = 行模型分块喂
//（巨 diff >5000 行不产生整帧重算，clue#22）。
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "platform/git/git_repo.h"

namespace module::git {

struct DiffRow {
    enum class Kind { HunkHeader, FoldBar, Line };
    Kind kind;
    size_t hunkIndex;       // 三类行都携带（折叠交互 / 内容锚）
    size_t lineIndex = 0;   // kind==Line 时 = hunk.lines 下标；其余 0
};

class GitDiffModel {
public:
    // 拷贝入模并重置折叠态。binary patch 不产行序列（视图出占位）。
    // placeholder 非空 = 占位文案（bug#18 untracked 超大档），视图优先渲染、不出行序列。
    void setPatch(platform::git::DiffPatch patch, std::string placeholder = "");
    void clear();
    void toggleHunk(size_t hunkIndex);              // I-06：hunk 头/折叠条单击翻转
    bool folded(size_t hunkIndex) const;

    const platform::git::DiffPatch& patch() const { return patch_; }
    const std::vector<DiffRow>& rows() const { return rows_; }
    bool binary() const { return patch_.binary; }
    bool isEmpty() const { return patch_.hunks.empty(); }  // 「无内容变更」占位判据
    const std::string& placeholder() const { return placeholder_; }
    // 最长行显示列数（setPatch 一次算好；0=无行。视图换算等宽字宽得横滚内容宽；
    // 非 ASCII 起始序列按 2 列——与 git_content_model 同口径，need:14 P1-1）
    size_t maxLineLen() const { return maxLineLen_; }

    // 文件头文案（UI-A §5.6 文件头）：重命名「old → new」，其余显新路径
    std::string headerText() const;

private:
    void rebuildRows();

    platform::git::DiffPatch patch_;
    std::vector<bool> folded_;
    std::vector<DiffRow> rows_;
    size_t maxLineLen_ = 0;
    std::string placeholder_;
};

// bug#18：untracked 条目 diff 模型层合成（platform/git 零改动；禁加
// GIT_DIFF_INCLUDE_UNTRACKED_CONTENT——大 untracked 全文入 patch = GitAhead #161
// 内存爆场景，qa12 夹具2 实测 95MB 基线依赖「不读内容」行为）。
// 仅当 patch.isUntracked 且无 hunks 且非目录条目（路径尾 '/'，§2.1 不递归）时：
// 复用 FileContent::readFromDisk 判据（fs::file_size 预判 + 1MiB 阈值 + 二进制探测）——
//   ≤1MiB 文本 → patch 内合成单 hunk 全增行（origin='+'，行号 1..n，既有
//   DiffPatch/DiffHunk 结构，clue#22 幂等行盒）；
//   二进制 → patch.binary=true（视图既有「二进制文件」占位）；
//   >1MiB → 返回占位文案「文件过大，未加载内容（>1 MiB）」（F-11③ 同口径，不读内容）；
//   空文件/BadEncoding/ReadError → 原样返回空串（保持「无内容变更」空态）。
// 其余情况原样返回空串、patch 不动。worker 线程调用（readFromDisk 同约束）。
std::string synthesizeUntrackedDiff(platform::git::DiffPatch& patch,
                                    const std::filesystem::path& absPath);

} // namespace module::git
