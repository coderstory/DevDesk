// src/module_markdown/md_block_interact.cpp —— MdSyncMap 反函数（editorLineForPreviewY）消费侧。
#include "md_block_interact.h"

#include "md_line_scanner.h"

#include <algorithm>
#include <cmath>

namespace module::markdown {

MdPreviewJump mdResolvePreviewJump(const std::string& text,
                                   const std::vector<MdBlock>& blocks,
                                   const MdSyncMap& map,
                                   size_t blockIndex,
                                   float localY) {
    MdPreviewJump jump;
    if (map.empty() || blockIndex >= map.blockCount()) return jump;

    const float h = map.blockHeight(blockIndex);
    const float y = std::clamp(localY, 0.0f, std::max(0.0f, h));
    const double line = map.editorLineForPreviewY(map.blockTopY(blockIndex) + y);

    // floor 后钳回本块行区间：localY==H_k 时反函数落到下一块首行，末行才是正确落点。
    const int first = map.blockFirstLine(blockIndex);
    const int count = std::max(1, map.blockLineCount(blockIndex));
    // +1e-4 容差再 floor：与 ④ round-trip 回归同界——y 经 float 正/反向混合累计可
    // 反查为 8.9999996，裸 floor 差一行（真实点击抖动 ≥0.5px ≫ 1e-4，容差不误合并）。
    int target = static_cast<int>(std::floor(line + 1e-4));
    target = std::clamp(target, first, first + count - 1);

    jump.ok = true;
    jump.blockIndex = blockIndex;
    jump.stableId = blockIndex < blocks.size() ? blocks[blockIndex].stableId : -1;
    jump.editorLine = line;
    jump.targetLine = target;
    jump.cursorByte = lineStartOffset(text, target);
    return jump;
}

eui::Color mdHoverMix(eui::Color base, eui::Color tint, float amount) {
    const float a = std::clamp(amount, 0.0f, 1.0f);
    eui::Color out = base;
    out.r = base.r + (tint.r - base.r) * a;
    out.g = base.g + (tint.g - base.g) * a;
    out.b = base.b + (tint.b - base.b) * a;
    return out;
}

} // namespace module::markdown
