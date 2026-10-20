// src/ui_kit/columns.cpp
// 双列分栏几何实现（契约见 columns.h）。
#include "columns.h"

namespace ui_kit {

void columnX(float totalW, float gap, float* colW, float* col2X) {
    const float usable = totalW - gap;
    const float half = usable > 0.0f ? usable * 0.5f : 0.0f;
    if (colW != nullptr) *colW = half;
    if (col2X != nullptr) *col2X = half + gap;
}

} // namespace ui_kit
