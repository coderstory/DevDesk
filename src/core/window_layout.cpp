// src/core/window_layout.cpp
// 启动期主窗口尺寸决策实现（纯计算，见 window_layout.h 的规则说明）
#include "window_layout.h"

#include <algorithm>
#include <cmath>

namespace core::window_layout {

namespace {

// 非客户区外框的 96 DPI 基准（物理像素）：本机 125% 实测 +18 宽 / +47 高
//（1618-1600 / 1047-1000）折算回 96 DPI 得 14.4 / 37.6。取浮点基准 + 向上
// 取整，保证 125% 下正好还原实测值（14.4×1.25 = 18.0、37.6×1.25 = 47.0），
// 其余缩放档按比例放大（Windows 标题栏/边框随 DPI 等比变高变宽）
constexpr float kFrameBaseWidthAt96Dpi = 14.4f;
constexpr float kFrameBaseHeightAt96Dpi = 37.6f;

// 缩放系数归一化：非有限 / 非正 → 1.0（与 core::window::windowScaleForPoint
// 查询失败返回 1.0f 的降级口径一致）
float normalizedScale(float scale) {
    return std::isfinite(scale) && scale > 0.0f ? scale : 1.0f;
}

// 逻辑尺寸 → 物理像素：与最小尺寸语义取大（0 = 不约束），至少 1 像素；
// 非有限输入（理论上不出现）不参与比较，走 std::max 的自然结果
int physicalSize(int logical, int minLogical, float scale) {
    const int effectiveLogical = std::max(std::max(logical, minLogical), 1);
    const float physical = static_cast<float>(effectiveLogical) * scale;
    return static_cast<int>(std::lround(physical));
}

} // namespace

FrameInsets frameInsetsForScale(float scale) {
    const float s = normalizedScale(scale);
    // 向上取整：高估外框 → 最坏是多触发一次最大化（用户可还原），
    // 低估则窗口再次被任务栏吃掉（本次要修的缺陷）
    return FrameInsets{
        static_cast<int>(std::ceil(kFrameBaseWidthAt96Dpi * s)),
        static_cast<int>(std::ceil(kFrameBaseHeightAt96Dpi * s)),
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

} // namespace core::window_layout
