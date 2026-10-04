// 启动期主窗口尺寸决策实现（纯计算，规则见 window_layout.h）。平台差异经 frameInsetsBaseline() /
// windowUnitName() 注入，本文件零平台宏、可无头单测
#include "window_layout.h"

#include <algorithm>
#include <cmath>

namespace core::window_layout {

namespace {

// 缩放系数归一化：非有限/非正 → 1.0（与 windowScaleForPoint 查询失败的降级口径一致）
float normalizedScale(float scale) {
    return std::isfinite(scale) && scale > 0.0f ? scale : 1.0f;
}

// 逻辑尺寸 → 窗口单位：与最小尺寸取大（0=不约束），至少 1 单位
int physicalSize(int logical, int minLogical, float scale) {
    const int effectiveLogical = std::max(std::max(logical, minLogical), 1);
    const float physical = static_cast<float>(effectiveLogical) * scale;
    return static_cast<int>(std::lround(physical));
}

// 最小尺寸换算：0 = 不约束（保持透传，与 physicalSize 的「至少 1」不同），>0 时同公式换算
int minSizeInWindowUnits(int minLogical, float scale) {
    if (minLogical <= 0) {
        return 0;
    }
    return static_cast<int>(std::lround(static_cast<float>(minLogical) * scale));
}

// 与「工作区−外框」取 min：0 保持原样；可用空间 <=0 返回 0（交回后端无约束语义，不给负值/伪下限）
int clampMinToWorkArea(int minSize, int available) {
    if (minSize <= 0) {
        return 0;
    }
    return available > 0 ? std::min(minSize, available) : 0;
}

} // namespace

FrameInsets frameInsetsForScale(float scale) {
    return frameInsetsForScale(scale, frameInsetsBaseline());
}

FrameInsets frameInsetsForScale(float scale, const FrameInsetsBaseline& baselineAt96Dpi) {
    const float s = normalizedScale(scale);
    // 向上取整：高估最多多触发一次最大化（用户可还原），低估则窗口再被任务栏吃掉
    return FrameInsets{
        static_cast<int>(std::ceil(baselineAt96Dpi.width * s)),
        static_cast<int>(std::ceil(baselineAt96Dpi.height * s)),
    };
}

InitialWindowDecision decideInitialWindow(const InitialWindowRequest& request,
                                         const WorkArea& workArea,
                                         float scale,
                                         const FrameInsets& frameInsets) {
    const float s = normalizedScale(scale);
    InitialWindowDecision decision;
    decision.width = physicalSize(request.logicalWidth, request.minLogicalWidth, s);
    decision.height = physicalSize(request.logicalHeight, request.minLogicalHeight, s);

    // 工作区不可用（无头/glfwInit 之前/无显示器）：不擅自改窗口形态，按请求尺寸创建
    if (workArea.width <= 0 || workArea.height <= 0) {
        return decision;
    }

    // 外框与工作区比较：严格超出才最大化，正好等于算放得下
    const bool tooWide = decision.width + frameInsets.width > workArea.width;
    const bool tooTall = decision.height + frameInsets.height > workArea.height;
    decision.maximize = tooWide || tooTall;
    return decision;
}

WindowSize minWindowSizeForWorkArea(const InitialWindowRequest& request,
                                    const WorkArea& workArea,
                                    float scale,
                                    const FrameInsets& frameInsets) {
    const float s = normalizedScale(scale);

    // 换算（与 windowSize 同公式；漏这步会两端收不进屏/缩过头）
    WindowSize minSize;
    minSize.width = minSizeInWindowUnits(request.minLogicalWidth, s);
    minSize.height = minSizeInWindowUnits(request.minLogicalHeight, s);

    // 工作区不可用（无头/查询失败/无显示器）：不擅自加约束（与 decideInitialWindow 同口径）
    if (workArea.width <= 0 || workArea.height <= 0) {
        return minSize;
    }

    // 与「工作区−外框」取 min；必须先换算出窗口单位再以同一单位钳（换算与钳制须成对）
    minSize.width = clampMinToWorkArea(minSize.width, workArea.width - frameInsets.width);
    minSize.height = clampMinToWorkArea(minSize.height, workArea.height - frameInsets.height);
    return minSize;
}

} // namespace core::window_layout
