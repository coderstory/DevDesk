// 指针空间 → 页面 compose 坐标的唯一换算。坐标空间：windowPos = 指针事件坐标（窗口客户区）；
// pageOriginPos = 页面区原点（x 取 Router::pageOriginX，y 恒 0）；dpiScale = 指针→compose 缩放
//（入参取框架值：主窗口取 app::contentScale()，子窗取 Screen::scale；onContextMenu/onMove 已
// /dpiScale 传 1.0，其余传框架值）。纯函数零 include，可无头单测
#pragma once

namespace core {

inline float windowToCompose(float windowPos, float pageOriginPos, float dpiScale) {
    // 退化输入（0/负/NaN）按同尺度处理（非有限/非正 → 1.0），与 window_layout 归一化口径一致
    if (!(dpiScale > 0.0f)) dpiScale = 1.0f;
    return windowPos / dpiScale - pageOriginPos;
}

// 右键菜单统一锚点换算：位置、宽度、Y/X 钳制交给 contextMenu/styledMenu 组件自身（组件内已有
// clampMenuY/X 兜底）。onContextMenu 派发前已 /dpiScale 且坐标是 Logical 窗口坐标。
// scrollOffsetY：仅基于非滚动几何的调用需要，否则传 0
struct MenuAnchor {
    float x = 0.0f;
    float y = 0.0f;
};

inline MenuAnchor menuAnchorForPointer(float windowX, float windowY,
                                       float pageOriginX, float scrollOffsetY,
                                       float dpiScale = 1.0f) {
    return {windowToCompose(windowX, pageOriginX, dpiScale),
            windowToCompose(windowY, 0.0f, dpiScale) - scrollOffsetY};
}

} // namespace core
