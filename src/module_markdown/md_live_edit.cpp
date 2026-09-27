// src/module_markdown/md_live_edit.cpp
// Markdown Phase 3 · M2 纯函数层实现（说明见 md_live_edit.h 头注释）。
#include "md_live_edit.h"

#include <algorithm>

namespace module::markdown {

size_t mdLiveActiveBlockIndex(const MdBlockModel& model, size_t cursorByte) {
    if (model.blocks().empty()) return 0;
    // 二分查找已在 blockIndexAt 内处理两端（< 首块 end → 首块；≥ 末块 end → 末块）
    return model.blockIndexAt(cursorByte);
}

size_t mdLiveBlockLocalOffset(const MdBlock& block, size_t cursorByte) {
    if (cursorByte <= block.start) return 0;
    const size_t len = block.end > block.start ? block.end - block.start : 0;
    return std::min(cursorByte - block.start, len);
}

bool mdLiveCompositionFrozen(const std::string& compositionText) {
    return !compositionText.empty();
}

float mdLiveInputHeight(const std::string& blockText, float textWidth,
                        float fontSize, float inset,
                        const MdLiveLineCountFn& lineCount) {
    // 测量函数缺失/返回 0 一律按 1 行算：input 无论如何都有一行 caret 的高度
    int lines = lineCount ? lineCount(blockText, textWidth) : 1;
    if (lines < 1) lines = 1;
    if (inset < 0.0f) inset = 0.0f;
    return inset * 2.0f + static_cast<float>(lines) * fontSize *
                              kInputLineHeightFactor + 1.0f;
}

// ---- M4：块边界键（说明见头注释）----

namespace {

// pos 所在行的行首字节偏移（无前导 '\n' → 0）。调用侧保证 pos < text.size()。
size_t lineStartAt(const std::string& text, size_t pos) {
    if (pos == 0) return 0;
    const size_t nl = text.rfind('\n', pos - 1);
    return nl == std::string::npos ? 0 : nl + 1;
}

// 块下标（与 MdBlockModel::blockIndexAt 同口径：块边界字节归后一块）。
// 本层不持模型实例，故本地复刻同一二分；blocks 非空由调用侧保证。
size_t blockIndexIn(const std::vector<MdBlock>& blocks, size_t offset) {
    size_t lo = 0, hi = blocks.size() - 1;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (offset < blocks[mid].end) hi = mid;
        else lo = mid + 1;
    }
    return lo;
}

// 自 lineStart 起的行【内容】末（行终结符不计：'\r\n' 的 '\r' 也算终结符）。
size_t lineContentEnd(const std::string& text, size_t lineStart) {
    const size_t nl = text.find('\n', lineStart);
    size_t end = (nl == std::string::npos) ? text.size() : nl;
    if (end > lineStart && text[end - 1] == '\r') --end;
    return end;
}

} // namespace

size_t mdLiveVerticalBlockTarget(const std::string& text,
                                 const std::vector<MdBlock>& blocks,
                                 size_t cursorByte, int direction) {
    if (blocks.empty() || direction == 0 || text.empty()) return kMdLiveNoConsume;
    if (cursorByte >= text.size()) cursorByte = text.size() - 1;   // 文末光标
    const size_t k = blockIndexIn(blocks, cursorByte);
    const MdBlock& b = blocks[k];
    const size_t ls = lineStartAt(text, cursorByte);
    const size_t col = cursorByte - ls;   // 行内字节列（列保持用的就是它）

    if (direction < 0) {
        if (k == 0) return kMdLiveNoConsume;          // 首块 ↑：交回内置
        if (ls != b.start) return kMdLiveNoConsume;   // 不在块首行 → 块内移动
        // 上一行 = 光标前那个行终结符所属行。块首恒为行首（分块器按行切），
        // 故 text[b.start-1] 必是 '\n'（或 '\r\n' 的 '\n'）。
        size_t prevEnd = b.start - 1;                 // 内容末（含 '\r' 时再退一）
        if (prevEnd > 0 && text[prevEnd - 1] == '\r') --prevEnd;
        const size_t prevStart = lineStartAt(text, prevEnd > 0 ? prevEnd - 1 : 0);
        const size_t prevLen = prevEnd > prevStart ? prevEnd - prevStart : 0;
        return prevStart + std::min(col, prevLen);
    }

    if (k + 1 >= blocks.size()) return kMdLiveNoConsume;   // 末块 ↓：交回内置
    // 块末行 = 块内最后一个字节所在行（块尾字节可能是换行 → 归它终结的那行）
    if (ls != lineStartAt(text, b.end - 1)) return kMdLiveNoConsume;
    const size_t nextStart = blocks[k + 1].start;          // 下一块首行行首
    const size_t nextContentEnd = lineContentEnd(text, nextStart);
    const size_t nextLen = nextContentEnd > nextStart ? nextContentEnd - nextStart : 0;
    return nextStart + std::min(col, nextLen);
}

bool mdLiveBoundaryEdit(const std::string& text,
                        const std::vector<MdBlock>& blocks, size_t cursorByte,
                        int direction, MdLiveBoundaryEdit& out) {
    if (blocks.empty() || direction == 0) return false;
    if (cursorByte >= text.size()) return false;   // 文末：后面没有可删的换行
    const size_t k = blockIndexIn(blocks, cursorByte);
    if (blocks[k].start != cursorByte) return false;   // 不在块边界字节上
    if (direction < 0) {
        if (k == 0) return false;                      // 首块块首：前面没有换行
        if (text[cursorByte - 1] != '\n') return false;  // 防御（块首前必是换行）
        size_t s = cursorByte - 1;
        if (s > 0 && text[s - 1] == '\r') --s;          // CRLF 成对删
        out.eraseStart = s;
        out.eraseEnd = cursorByte;
        out.cursor = s;
        return true;
    }
    if (text[cursorByte] == '\n') {                     // 光标处就是行终结符
        out.eraseStart = cursorByte;
        out.eraseEnd = cursorByte + 1;
        out.cursor = cursorByte;
        return true;
    }
    if (text[cursorByte] == '\r' && cursorByte + 1 < text.size() &&
        text[cursorByte + 1] == '\n') {                 // CRLF 成对删
        out.eraseStart = cursorByte;
        out.eraseEnd = cursorByte + 2;
        out.cursor = cursorByte;
        return true;
    }
    return false;   // 后面不是行终结符：普通块内字符删除，交回内置
}

// ---- M5：块级 undo / redo（语义见头注释）----

void MdLiveUndoStack::reset(std::string text, size_t cursorByte) {
    undo_.clear();
    redo_.clear();
    if (cursorByte > text.size()) cursorByte = text.size();
    undo_.push_back(Entry{std::move(text), cursorByte});
    capacity_ = kMdLiveUndoCap;
    lastPushMs_ = 0;
    groupOpen_ = false;
}

void MdLiveUndoStack::push(std::string text, size_t cursorByte, bool sameBlock,
                           int64_t nowMs) {
    // 合并：同块 + 距上次推栈 < 600ms = 同一次编辑（逐键不堆快照）。
    // lastPushMs_ 取「上一次真的记了条目的时刻」而不随合并滑动：这样一组最多
    // 覆盖 600ms 的连续输入，长时间连打仍会定期留下撤销点（不会整段只剩一栈）。
    if (groupOpen_ && sameBlock && nowMs - lastPushMs_ < kMdLiveUndoMergeMs) return;
    // 走到这里 = 新的一次编辑：旧的重做链作废（同任何编辑器的语义）
    redo_.clear();
    groupOpen_ = true;
    lastPushMs_ = nowMs;
    // 与栈顶同文本 = 重复快照（撤销到它等于什么都不做）→ 不新增条目
    if (!undo_.empty() && undo_.back().text == text) return;
    capacity_ = text.size() > kMdLiveUndoLargeDocBytes ? kMdLiveUndoCapLargeDoc
                                                       : kMdLiveUndoCap;
    undo_.push_back(Entry{std::move(text), cursorByte});
    while (undo_.size() > capacity_) undo_.pop_front();   // 挤掉最老的一条
}

void MdLiveUndoStack::markBoundary() { groupOpen_ = false; }

bool MdLiveUndoStack::undo(std::string& text, size_t& cursorByte) {
    // 与当前文本相同的快照 = 无变化步 → 跳过（不消耗用户的一次撤销）
    while (!undo_.empty() && undo_.back().text == text) undo_.pop_back();
    if (undo_.empty()) return false;
    redo_.push_back(Entry{text, cursorByte});
    Entry prev = std::move(undo_.back());
    undo_.pop_back();
    text = std::move(prev.text);
    cursorByte = prev.cursorByte;
    // 撤销后必须新开一组：否则紧接着的同块输入会被并进「已被撤销的那一次编辑」，
    // 下一次 Ctrl+Z 会连它一起撤销
    groupOpen_ = false;
    return true;
}

bool MdLiveUndoStack::redo(std::string& text, size_t& cursorByte) {
    while (!redo_.empty() && redo_.back().text == text) redo_.pop_back();
    if (redo_.empty()) return false;
    undo_.push_back(Entry{text, cursorByte});
    while (undo_.size() > capacity_) undo_.pop_front();
    Entry next = std::move(redo_.back());
    redo_.pop_back();
    text = std::move(next.text);
    cursorByte = next.cursorByte;
    groupOpen_ = false;
    return true;
}

// ---- M6：滚动锚定与光标跟随（说明见头注释）----

MdLiveFollowDecision mdLiveFollowOffset(float caretDocY, float viewportH,
                                        float currentOffset, float maxOffset) {
    MdLiveFollowDecision d;
    if (maxOffset < 0.0f) maxOffset = 0.0f;
    d.offset = std::clamp(currentOffset, 0.0f, maxOffset);
    if (viewportH <= 0.0f) return d;   // 无视口：安全带无从谈起，不判不动
    const float rel = caretDocY - d.offset;
    if (rel >= kMdLiveFollowTopRatio * viewportH &&
        rel <= kMdLiveFollowBottomRatio * viewportH) {
        return d;                      // 安全带内：不干预用户滚动
    }
    d.pull = true;
    d.offset = std::clamp(caretDocY - kMdLiveFollowAnchorRatio * viewportH,
                          0.0f, maxOffset);
    return d;
}

} // namespace module::markdown
