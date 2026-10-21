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

// 收缩适配的呼吸边距（窗口单位：Windows=物理px、macOS=pt）：请求超出工作区时该轴钳到
// 「工作区 − 外框 − 本边距」，窗口不贴满工作区、四周留视觉余量。32 = 单轴方向各留 32
constexpr int kBreathingMargin = 32;

// 收缩适配单轴：钳到「工作区 − 外框 − 呼吸边距」；minFloor（minWindowSizeForWorkArea
// 同口径钳制值，0 = 无下限）兜极端小屏——钳完低于最小窗时以最小窗为准（贴边不留呼吸
// 边距，也不产出低于最小窗的不可用尺寸）；最后至少 1 单位（后端拿 0/负值建窗会出问题，
// 与 physicalSize 的「至少 1」口径一致）
int shrinkToFitWorkArea(int requestedUnits, int workAxisSize, int insets, int minFloor) {
    const int fit = workAxisSize - insets - kBreathingMargin;
    int shrunk = std::min(requestedUnits, fit);
    if (minFloor > 0) {
        shrunk = std::max(shrunk, minFloor);
    }
    return std::max(shrunk, 1);
}

} // namespace

FrameInsets frameInsetsForScale(float scale) {
    return frameInsetsForScale(scale, frameInsetsBaseline());
}

FrameInsets frameInsetsForScale(float scale, const FrameInsetsBaseline& baselineAt96Dpi) {
    const float s = normalizedScale(scale);
    // 向上取整：高估最多让收缩适配多收一点（用户可自行放大窗口），低估则窗口再被任务栏吃掉
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

    // 外框与工作区比较（正好等于算放得下）：严格超出的轴收缩适配，不再默认最大化
    //（UAT6 修复：1600×1000 逻辑请求在 1080p@125% 换算 2000×1250 后超出工作区，旧
    //「超出即最大化」策略导致每次启动都全屏）。maximize 恒 false，字段保留供未来
    //「手动/记住最大化」类需求，本函数不再产出默认最大化
    const bool tooWide = decision.width + frameInsets.width > workArea.width;
    const bool tooTall = decision.height + frameInsets.height > workArea.height;
    if (tooWide || tooTall) {
        // 最小窗下限与 minWindowSizeForWorkArea 同口径（换算 → 与「工作区−外框」取 min）：
        // 极端小屏收缩后低于最小窗时以它为准（宁可贴边，也不给低于最小窗的不可用尺寸）
        const WindowSize minFloor = minWindowSizeForWorkArea(request, workArea, s, frameInsets);
        if (tooWide) {
            decision.width =
                shrinkToFitWorkArea(decision.width, workArea.width, frameInsets.width, minFloor.width);
        }
        if (tooTall) {
            decision.height =
                shrinkToFitWorkArea(decision.height, workArea.height, frameInsets.height, minFloor.height);
        }
    }
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

RestoredWindowPlan restoreInitialWindow(const SavedWindowGeometry& saved,
                                        const InitialWindowDefaults& defaults,
                                        const WorkArea& workArea,
                                        float scale,
                                        const FrameInsets& frameInsets) {
    RestoredWindowPlan plan;
    // 尺寸记录判定：任一轴非正 = 无记录（SettingsStore 已把负值收敛 0，这里再防直传脏值）
    plan.fromSavedRecord = saved.logicalWidth > 0 && saved.logicalHeight > 0;
    // 记忆尺寸（或默认）+ 最小窗下限 → 同一条 decideInitialWindow 钳制链
    plan.requestUsed = InitialWindowRequest{
        plan.fromSavedRecord ? saved.logicalWidth : defaults.logicalWidth,
        plan.fromSavedRecord ? saved.logicalHeight : defaults.logicalHeight,
        defaults.minLogicalWidth, defaults.minLogicalHeight};
    plan.initial =
        decideInitialWindow(plan.requestUsed, workArea, scale, frameInsets);
    // 最大化标志独立透传（不参与尺寸钳制链）：恢复为最大化时 windowSize 即还原态
    // 尺寸（GLFW 用它当退出最大化后的回落尺寸），尺寸缺失仍可最大化（回退默认尺寸）
    plan.initial.maximize = saved.maximized;
    return plan;
}

} // namespace core::window_layout
