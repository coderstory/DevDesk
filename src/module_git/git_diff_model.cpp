// src/module_git/git_diff_model.cpp
#include "git_diff_model.h"

#include "git_content_model.h"

#include <algorithm>
#include <utility>

namespace module::git {

void GitDiffModel::setPatch(platform::git::DiffPatch patch, std::string placeholder) {
    patch_ = std::move(patch);
    placeholder_ = std::move(placeholder);
    folded_.assign(patch_.hunks.size(), false);
    // 最长行码点数（F-07⑥ 横滚内容宽）：diff 行统一等宽字体，码点数即宽。
    // bug#21：按字节计（content.size()）会把 UTF-8 CJK 3 字节/字当 3 字符，
    // diff 横滚内容宽虚胖 ~3 倍——与内容模型（git_content_model）同口径改码点。
    maxLineLen_ = 0;
    for (const auto& hunk : patch_.hunks) {
        for (const auto& line : hunk.lines) {
            size_t codepoints = 0;
            for (const char c : line.content) {
                if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++codepoints;
            }
            maxLineLen_ = std::max(maxLineLen_, codepoints);
        }
    }
    rebuildRows();
}

void GitDiffModel::clear() {
    patch_ = platform::git::DiffPatch{};
    folded_.clear();
    rows_.clear();
    placeholder_.clear();
}

void GitDiffModel::toggleHunk(size_t hunkIndex) {
    if (hunkIndex >= folded_.size()) return;
    folded_[hunkIndex] = !folded_[hunkIndex];
    rebuildRows();
}

bool GitDiffModel::folded(size_t hunkIndex) const {
    return hunkIndex < folded_.size() && folded_[hunkIndex];
}

std::string GitDiffModel::headerText() const {
    if (patch_.newPath.empty()) return patch_.oldPath;
    if (patch_.oldPath.empty() || patch_.oldPath == patch_.newPath) {
        return patch_.newPath;
    }
    return patch_.oldPath + " → " + patch_.newPath;
}

void GitDiffModel::rebuildRows() {
    rows_.clear();
    rows_.reserve(patch_.hunks.size() * 4);
    for (size_t h = 0; h < patch_.hunks.size(); ++h) {
        rows_.push_back({DiffRow::Kind::HunkHeader, h, 0});
        if (folded_[h]) {
            // 折叠条（UI-A §5.6）：行数文案由视图从 patch 取（行模型只带结构）
            rows_.push_back({DiffRow::Kind::FoldBar, h, 0});
            continue;
        }
        const size_t n = patch_.hunks[h].lines.size();
        for (size_t l = 0; l < n; ++l) {
            rows_.push_back({DiffRow::Kind::Line, h, l});
        }
    }
}

std::string synthesizeUntrackedDiff(platform::git::DiffPatch& patch,
                                    const std::filesystem::path& absPath) {
    // 只处理「untracked 且无 hunk」；untracked 目录条目（libgit2 路径尾 '/'，
    // §2.1 整体一条不递归防 hang）与已有 hunk 的 patch 一律不动
    if (!patch.isUntracked || !patch.hunks.empty()) return {};
    if (!patch.newPath.empty() && patch.newPath.back() == '/') return {};

    const FileContent fc = FileContent::readFromDisk(absPath);
    switch (fc.state) {
    case ContentState::Ok: {
        if (fc.lines.empty()) return {};  // 理论不可达：0 字节走 Empty（防御）
        platform::git::DiffHunk hunk;
        hunk.header = "@@ -0,0 +1," + std::to_string(fc.lines.size()) + " @@";
        hunk.lines.reserve(fc.lines.size());
        for (size_t i = 0; i < fc.lines.size(); ++i) {
            platform::git::DiffLine l;
            l.origin = '+';
            l.newNo = static_cast<int>(i) + 1;  // 全增：oldNo 无、newNo 1 起
            l.content = fc.lines[i];
            hunk.lines.push_back(std::move(l));
        }
        patch.hunks.push_back(std::move(hunk));
        return {};
    }
    case ContentState::Binary:
        patch.binary = true;  // 视图既有「二进制文件，未显示内容差异」占位
        return {};
    case ContentState::Oversize:
        // F-11③ 同口径文案；尺寸闸已短路，零内容 I/O（qa12 内存基线依赖）
        return "文件过大，未加载内容（>1 MiB）";
    default:  // Empty / BadEncoding / ReadError → 保持「无内容变更」空态
        return {};
    }
}

} // namespace module::git
