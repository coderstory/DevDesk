// src/core/ui_geometry.h
// 指针空间 → 页面 compose 坐标的**唯一换算**（审计 M4）。
//
// 背景：此前三套并存——markdown 浮层只减页面原点 x；todo 拖拽用"锚定被拖
// 卡片"反推 dpi + 板面原点；pet 走窗口坐标 + 工作区钳制。三套各自成立、互相
// 不兼容：EUI-NEO 侧一旦改变 bounds / 指针的空间语义（物理像素 ↔ 逻辑像素），
// 只有自己推坐标的那一套会随动。现统一到本函数，各调用点只声明自己那一路
// 指针事件的尺度，换算本体只有一份。
//
// 坐标空间（调用方按此填参）：
//   windowPos     指针事件坐标（窗口客户区口径，EUI-NEO runtime 派发）
//   pageOriginPos 页面区原点（compose 空间）。x 取 core::Router::pageOriginX
//                 （唯一来源，app 层每帧写入）；y 方向页面区纵跨整窗，
//                 原点恒 0，传 0.0f
//   dpiScale      指针空间 → compose 空间的缩放（1.0 = 两空间同尺度）。
//                 **入参取框架值**，不要在本函数里猜、也不要用几何反推：
//                 主窗口（Router 页面）取 app::contentScale()；子窗取该窗
//                 compose 拿到的 core::dsl::Screen::scale（各窗可能落在缩放
//                 不同的显示器上）。U1 尺度契约定义 **物理 = 逻辑 * scale**，
//                 框架三处同源同值：app::contentScale() / Screen::scale /
//                 PointerEvent::scale（DragEvent::scale）。EUI-NEO 各指针通道
//                 口径仍不同——onContextMenu / onMove 派发前已做 /dpiScale
//                 （传 1.0），onPress / onRelease / onDrag 是原始值（传框架值）
//
// 分层：**纯函数**——零 include，不调用 EUI-NEO 运行时行为、不依赖 platform
// （core 层约束），可直接无头单测（tests/test_core/test_router.cpp）。
#pragma once

namespace core {

inline float windowToCompose(float windowPos, float pageOriginPos, float dpiScale) {
    // 退化输入（0 / 负 / NaN）按同尺度处理，与 core::window_layout 的
    // 缩放系数归一化口径一致（非有限 / 非正 → 1.0）
    if (!(dpiScale > 0.0f)) dpiScale = 1.0f;
    return windowPos / dpiScale - pageOriginPos;
}

} // namespace core
