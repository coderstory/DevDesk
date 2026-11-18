// window_layout 的 macOS 平台注入：本文件只返回平台事实，零计算逻辑——
// 换算/决策全在 window_layout.cpp（纯函数，可无头单测）
#include "window_layout.h"

namespace core::window_layout {

FrameInsetsBaseline frameInsetsBaseline() {
    // macOS 装饰由 AppKit 接管：无左右/下边框只有标题栏，装饰高 28、宽 0。沿用 Windows 基准会
    // 多算不存在的边框，临界屏上把本可完整显示的窗口无谓最大化
    return FrameInsetsBaseline{0.0f, 28.0f};
}

const char* windowUnitName() {
    // 单位为点：windowScaleForPoint 在 macOS 恒返 1.0（Retina 的 2.0 是
    // contentScale，不是「逻辑→窗口单位」换算，乘上去会双倍放大）
    return "pt";
}

float systemTitleBarInset() {
    // macOS 系统标题栏高度（回退态：红绿灯原生钮所在）；内容需让出它
    return 30.0f;
}

float windowCornerRadiusLogical(float contentScale) {
    // macOS 圆角 = 16 逻辑pt（2026-11-07 AppKit 内省实证：标准 titled 窗
    // [NSWindow valueForKey:@"_cornerRadius"] == 16.0，系统自报值，scale 不参与
    // 折算）。此前两轮全错：12 偏大 2pt→弧带被裁「变淡不完整」；10 源自一次误测
    // （像素扫描扫到的是旧 ring 自身弧/阴影带，非窗口裁剪弧）→ r=9 的弧带
    // 整体落在 45° 裁剪线内 → 四角黑线全消（bug145，池197）。16 → 描边弧
    // r=15 与窗口裁剪弧同心、整体内侧 1px，四角弧带完整可见。
    (void)contentScale;
    return 16.0f;
}

} // namespace core::window_layout
