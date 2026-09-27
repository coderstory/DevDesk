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

} // namespace module::markdown
