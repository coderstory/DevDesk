// src/core/window_layout.cpp
// 启动期主窗口尺寸决策实现（纯计算，见 window_layout.h 的规则说明）。
// 平台差异（外框基准值 / 单位名）不在这里判平台，而是经 frameInsetsBaseline() /
// windowUnitName() 两个平台注入点进来（实现拆 window_layout_macos.cpp /
// _win.cpp / _posix.cpp，CMake 按平台选编）——本文件保持零平台宏、可无头单测。
#include "window_layout.h"

#include <algorithm>
#include <cmath>

namespace core::window_layout {

namespace {

// 缩放系数归一化：非有限 / 非正 → 1.0（与 core::window::windowScaleForPoint
// 查询失败返回 1.0f 的降级口径一致）
float normalizedScale(float scale) {
    return std::isfinite(scale) && scale > 0.0f ? scale : 1.0f;
}

// 逻辑尺寸 → 窗口单位：与最小尺寸语义取大（0 = 不约束），至少 1 单位；
// 非有限输入（理论上不出现）不参与比较，走 std::max 的自然结果
int physicalSize(int logical, int minLogical, float scale) {
    const int effectiveLogical = std::max(std::max(logical, minLogical), 1);
    const float physical = static_cast<float>(effectiveLogical) * scale;
    return static_cast<int>(std::lround(physical));
}

// 最小尺寸方向的换算：0 = 不约束（保持透传，与 physicalSize 的「至少 1」不同
// 语义），>0 时按同一条公式换算
int minSizeInWindowUnits(int minLogical, float scale) {
    if (minLogical <= 0) {
        return 0;
    }
    return static_cast<int>(std::lround(static_cast<float>(minLogical) * scale));
}

// 与「工作区 − 外框」取 min：0（不约束）保持原样；可用空间 <= 0 时返回 0
// （任何下限都装不进屏 → 交回后端的无约束语义，不给负值/伪下限）
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
    // 向上取整：高估外框 → 最坏是多触发一次最大化（用户可还原），
    // 低估则窗口再次被任务栏吃掉（本次要修的缺陷）
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

    // 工作区信息不可用（无头 / glfwInit 之前 / 无显示器）：不擅自改窗口形态，
    // 按请求尺寸创建（与修复前行为一致）
    if (workArea.width <= 0 || workArea.height <= 0) {
        return decision;
    }

    // 外框（客户区 + 非客户区）与工作区比较：严格超出才最大化，
    // 正好等于工作区算放得下
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

    // 第一条：换算（与 windowSize 同一条公式——这是修复的核心，"min 漏了这一步"
    // 正是 macOS 上最小窗收不进屏、Windows 上最小窗缩过头的共同病因）
    WindowSize minSize;
    minSize.width = minSizeInWindowUnits(request.minLogicalWidth, s);
    minSize.height = minSizeInWindowUnits(request.minLogicalHeight, s);

    // 工作区信息不可用（无头 / 查询失败 / 无显示器）：不擅自加约束
    //（与 decideInitialWindow 第 3 条同口径）
    if (workArea.width <= 0 || workArea.height <= 0) {
        return minSize;
    }

    // 第二条：与「工作区 − 外框」取 min。顺序不能反——必须先换算出窗口单位，
    // 再拿同一单位的可用空间钳，否则两个量不同单位、比较无意义。
    //（注释与实现同处一条：这两步是"必须同一次提交"的一对，缺一即把一端的
    //  病复制到另一端。）
    minSize.width = clampMinToWorkArea(minSize.width, workArea.width - frameInsets.width);
    minSize.height = clampMinToWorkArea(minSize.height, workArea.height - frameInsets.height);
    return minSize;
}

} // namespace core::window_layout
