// src/ui_kit/metrics.h
// presentation 能力层：跨模块设计数值具名常量单一来源（kHeaderH / DialogWidth /
// kDialogInsetX / kDialogBodyInsetX / kNavItemH）。本层被 module_* 与 app 层链接，
// core / platform 不得反向依赖。纯常量（零 include、零 EUI-NEO 依赖）；
// 已 token 化的量（spacing* / radius* / fontSize*）不在本头重复声明。
#pragma once

namespace ui_kit {

// 页头行高：各模块页头一律 40（UAT5 全局顶部收紧：todo 看板顶边 / markdown
// 大纲几何自它推导；settings 页头同档——kSettingsHeaderH 已并入本常量，不再双源）。
inline constexpr float kHeaderH = 40.0f;

// 侧边栏导航项高：命中区 / 图标 / 文字三处高度与逐项步进共用。
inline constexpr float kNavItemH = 40.0f;

// 标准对话框宽度取值集合，避免同一个 460 各写一遍字面量。
enum class DialogWidth {
    Compact,    // 420 —— claude 删除确认
    Standard,   // 460 —— todo 全部 / markdown 全部
    Wide,       // 640 —— todo 详情 / 逾期
    ExtraWide,  // 720 —— 超宽档
    Form,       // 760 —— claude Profile 表单
};

inline float dialogWidthPx(DialogWidth width) {
    switch (width) {
        case DialogWidth::Compact:
            return 420.0f;
        case DialogWidth::Standard:
            return 460.0f;
        case DialogWidth::Wide:
            return 640.0f;
        case DialogWidth::ExtraWide:
            return 720.0f;
        case DialogWidth::Form:
            return 760.0f;
    }
    return 460.0f;  // 兜底：枚举穷举，仅为满足「所有路径有返回」
}

// 对话框内缩：标题 / 正文 / 按钮一律自 x = 24 起；正文行宽 = 弹窗宽 - 48。
inline constexpr float kDialogInsetX = 24.0f;
inline constexpr float kDialogBodyInsetX = 48.0f;

// 标准弹窗壳三段几何（2026-11-07 弹窗规范 need156，docs/design/
// 2026-11-07-dialog-standard-design.md §3）：头区固定 69（16+28+8+1+16）、
// 底区固定 73（1+16+40+16）、内容区 = 高 - 158（唯一内滚区）。
inline constexpr float kDialogHeaderH = 69.0f;
inline constexpr float kDialogFooterH = 73.0f;

// 弹窗最小尺寸下限（规范 §3.4）：防内容挤扁；视口极窄时框架 clamp 优先（可低于下限）。
inline constexpr float kDialogMinW = 420.0f;
inline constexpr float kDialogMinH = 240.0f;

// 弹窗按钮：高 40、最小宽 96（文字超出自适应加宽）、间距 8、右缘内缩 24（规范 §3.3）。
inline constexpr float kDialogButtonMinW = 96.0f;
inline constexpr float kDialogButtonGap = 8.0f;

} // namespace ui_kit
