// need534 平台拆分的 Windows 侧：1080P@125% 逻辑高等效 774（设计基线 1000），
// 高度缺 226px，故页面垂直密度整体紧凑一档。mac 侧见 density_posix.cpp（逐字不变）。
//
// 取值全取 4 的倍数（4×1.25=5 整数，125% 缩放下零半像素）；与 mac 的对应关系：
//   pageInsetV 32→24 / cardGap 16→8 / cardPadV 20→16 / listRowH 44→40 / headerH 40→36
#include "density.h"

namespace ui_kit::density {

float pageInsetV() { return 24.0f; }
float cardGap() { return 8.0f; }
float cardPadV() { return 16.0f; }
float listRowH() { return 40.0f; }
float headerH() { return 36.0f; }

} // namespace ui_kit::density
