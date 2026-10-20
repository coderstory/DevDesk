// src/ui_kit/columns.h
// 双列分栏几何单一来源（UAT3 设置页多列分栏）：半宽配对列、卡内双列共用同一公式
// colW = (totalW - gap) / 2、col2X = colW + gap，防止各消费方各写一份后漂移。
// 同步卡（github_sync_card）的 colGap=8 是卡内另一套既有口径，不迁移不同步。
#pragma once

namespace ui_kit {

// 双列几何：totalW = 可用总宽；出参 colW = 单列宽（两列等宽）、col2X = 右列 x
// （相对 totalW 左缘，已含间隙）。出参允许为 nullptr（只取其一）。
// totalW ≤ gap 时列宽兜底为 0（不产生负宽，绘制层自然退化为空区）。
void columnX(float totalW, float gap, float* colW, float* col2X);

} // namespace ui_kit
