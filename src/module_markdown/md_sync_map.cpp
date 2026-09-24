// src/module_markdown/md_sync_map.cpp
// MdSyncMap 实现——见头文件注释（Phase 3 ④ 同步滚动块模型精确锚）。
#include "md_sync_map.h"

#include <algorithm>

namespace module::markdown {

namespace {

// 块占编辑器行数：span 内 '\n' 个数 +（span 未以 '\n' 结尾时）1。
// 与 md_line_scanner::countLines 的行定义一致（CRLF 的行由 '\n' 界定）。
int blockLineCount(const std::string& text, const MdBlock& b) {
    int nl = 0;
    for (size_t i = b.start; i < b.end && i < text.size(); ++i) {
        if (text[i] == '\n') ++nl;
    }
    const bool endsWithNewline = b.end > b.start && b.end <= text.size() &&
                                 text[b.end - 1] == '\n';
    const int lines = nl + (endsWithNewline ? 0 : 1);
    return lines > 0 ? lines : 1;
}

} // namespace

void MdSyncMap::rebuild(const std::string& text,
                        const std::vector<MdBlock>& blocks,
                        uint64_t rev,
                        float width,
                        float blockGap,
                        const HeightFn& heightFn) {
    // rev 与文本内容一一对应（setEditorText 唯一写点 +1），同 rev ⇒ 同
    // blocks；(rev, width, gap, 块数) 全等 → 几何表未失效，零重算。
    if (built_ && rev_ == rev && width_ == width && gap_ == blockGap &&
        n_ == blocks.size()) {
        return;
    }
    built_ = true;
    rev_ = rev;
    width_ = width;
    gap_ = blockGap;
    n_ = blocks.size();
    ++rebuildCount_;

    firstLine_.assign(n_, 0);
    lineCount_.assign(n_, 1);
    height_.assign(n_, 0.0f);
    topY_.assign(n_, 0.0f);

    // stableId ↔ 内容一一对应（MdBlockModel 前/后缀对齐继承，新块取新 id），
    // 按 (stableId, width) 备忘块高度：未变块跨 rebuild 零重测。重建后 memo
    // 只保留本代仍存活的 id（死亡 id 随 swap 淘汰，不无界增长）。
    std::unordered_map<int, MemoEntry> nextMemo;
    nextMemo.reserve(n_ * 2 + 1);

    int curLine = 0;
    float curY = 0.0f;
    for (size_t k = 0; k < n_; ++k) {
        const MdBlock& b = blocks[k];
        firstLine_[k] = curLine;
        lineCount_[k] = blockLineCount(text, b);
        curLine += lineCount_[k];

        float h = 0.0f;
        bool have = false;
        if (b.stableId >= 0) {
            auto it = memo_.find(b.stableId);
            if (it != memo_.end() && it->second.width == width) {
                h = it->second.height;
                have = true;
            }
        }
        if (!have) {
            h = heightFn(b);
            ++heightComputes_;
            if (!(h > 0.0f)) h = 0.001f;  // 防御：除零/负高不可入表
            if (b.stableId >= 0) {
                nextMemo[b.stableId] = MemoEntry{width, h};
            }
        } else if (b.stableId >= 0) {
            nextMemo[b.stableId] = MemoEntry{width, h};
        }
        height_[k] = h;
        topY_[k] = curY;
        curY += h + blockGap;  // k 与 k+1 之间 = 外层 column 的 blockGap
    }
    memo_.swap(nextMemo);
}

float MdSyncMap::totalHeight() const {
    if (n_ == 0) return 0.0f;
    return topY_[n_ - 1] + height_[n_ - 1];
}

float MdSyncMap::blockTopY(size_t k) const {
    return k < n_ ? topY_[k] : 0.0f;
}

int MdSyncMap::blockFirstLine(size_t k) const {
    return k < n_ ? firstLine_[k] : 0;
}

float MdSyncMap::previewYForEditorLine(double editorLine) const {
    if (n_ == 0) return 0.0f;
    if (editorLine <= 0.0) return 0.0f;
    // firstLine_ 严格递增（每块 ≥1 行）→ 二分找「首行 ≤ editorLine 的最后一块」
    auto it = std::upper_bound(firstLine_.begin(), firstLine_.end(), editorLine);
    size_t k = static_cast<size_t>(it - firstLine_.begin());
    if (k > 0) --k;
    if (k >= n_) k = n_ - 1;
    double frac = (editorLine - static_cast<double>(firstLine_[k])) /
                  static_cast<double>(lineCount_[k]);
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;  // 超出末行 → 钳到末块底部
    return topY_[k] + static_cast<float>(frac) * height_[k];
}

double MdSyncMap::editorLineForPreviewY(float previewY) const {
    if (n_ == 0) return 0.0;
    if (previewY <= 0.0f) return 0.0;
    auto it = std::upper_bound(topY_.begin(), topY_.end(), previewY);
    size_t k = static_cast<size_t>(it - topY_.begin());
    if (k > 0) --k;
    if (k >= n_) k = n_ - 1;
    // y 落在块间隙（k 底部与 k+1 顶部之间）→ frac 钳 1 → 行号 = 下一块首行
    double frac = static_cast<double>(previewY - topY_[k]) /
                  static_cast<double>(height_[k]);
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    return static_cast<double>(firstLine_[k]) +
           frac * static_cast<double>(lineCount_[k]);
}

} // namespace module::markdown
