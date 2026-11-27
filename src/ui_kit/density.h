// src/ui_kit/density.h
// 页面布局密度档（need534 / 批次 D）：Windows 1080P@125% 下 bodyScreen 只有
// 1510×774，而设计基线是 1600×1000 → 高度少 226px（18.6%），显得挤。
//
// 平台拆分纪律（AGENTS §三）：本头只声明，取值在平台实现里——
//   density_posix.cpp（mac/Linux，与改动前逐字相同）
//   density_win.cpp（Windows，紧凑一档）
// CMake 按平台选源（同 core 的 theme_deepen / platform_info 模式）。业务代码
// 零 `#ifdef`。
//
// 取值纪律：所有密度值取 4 的倍数——125% 缩放下 4×1.25=5 为整数，零半像素。
#pragma once

namespace ui_kit::density {

// 页面垂直内边距（页上下留白 / 页内块间距基准）。
float pageInsetV();
// 卡片之间的间距（网格 gap）。
float cardGap();
// 卡片内垂直内边距。
float cardPadV();
// 列表行高（设置卡内行 / 列表项）。
float listRowH();
// 页头行高（跨页统一，见 metrics.h kHeaderH 的运行时版本）。
float headerH();

} // namespace ui_kit::density
