// src/ui_kit/metrics.h
// presentation 能力层（任务 D / R3）：跨模块复制的「设计数值」的具名常量单一来源。
//
// 分层依据同 toast.h / dialog_form.h（spec 2026-09-27-refactor-incremental-design
// §4.1）：本层被 module_* 与 app 层链接，core / platform 不得反向依赖本层。
//
// 收编对象（spec §5.2 R3，对应盘点 D3 表头几何 / D2 弹窗尺寸）：
//   - kHeaderH       表头行高 64 —— claude / settings / todo / markdown 四模块各写
//                    一份字面量，todo 侧另有一处藏在看板几何公式里、markdown 侧
//                    另有 4 处（header 行、正文高、大纲面板纵向几何）；
//   - DialogWidth / dialogWidthPx
//                    标准弹窗宽度取值集合（R2 落在 dialog_form.h 的私有侧，R3 归并
//                    到本头，与其余尺寸常量同一来源；消费方仍经 dialog_form.h 可见）；
//   - kDialogInsetX  弹窗标题行 / 正文 / 按钮的左内缩 28；
//   - kDialogBodyInsetX
//                    弹窗正文行右内缩 56（行宽 = 弹窗宽 - 该值）；
//   - kNavItemH      侧边栏导航项高 40（命中区 / 图标 / 文字三处共用 + 逐项步进）。
//
// 本头是**纯常量**（零 include、零 EUI-NEO 依赖），链接 devdesk_ui_kit 的目标都能
// 零成本引入。设计 token 已覆盖的量（间距 spacing* / 圆角 radius* / 字号 fontSize*）
// 不在本头重复声明——那会造出「同值两处」，正是 D7 要消掉的东西；布局里出现
// 40 / 24 / 16 这类**间距**值时，直接写 t.sizes.spacingXXLarge / spacingXLarge /
// spacingLarge（token 是唯一来源，且 token 值不经字号缩放，替换恒等值零行为变化）。
#pragma once

namespace ui_kit {

// ---- 表头行高（D3）----
// 四个模块的表头行一律 64 高；todo 看板顶边（boardGeom）与 markdown 大纲面板的
// 纵向几何均自它推导（正文高 = 内容高 - kHeaderH - spacingLarge）。
inline constexpr float kHeaderH = 64.0f;

// ---- 侧边栏导航项高度 ----
// app_sidebar 的命中区 / 图标 / 文字三处高度与逐项步进（itemH + gap）共用。
inline constexpr float kNavItemH = 40.0f;

// ---- 标准对话框宽度（D2：13 处调用点的宽度取值集合）----
//
// 具名化的目的：宽度的唯一来源，避免同一个 460 在多个对话框里各写一遍字面量。
enum class DialogWidth {
    Compact,    // 420 —— claude 删除确认
    Standard,   // 460 —— todo 全部 / markdown 全部
    Wide,       // 560 —— todo 详情 / 逾期
    ExtraWide,  // 720 —— claude 调用记录
    Form,       // 760 —— claude Profile 表单
};

inline float dialogWidthPx(DialogWidth width) {
    switch (width) {
        case DialogWidth::Compact:
            return 420.0f;
        case DialogWidth::Standard:
            return 460.0f;
        case DialogWidth::Wide:
            return 560.0f;
        case DialogWidth::ExtraWide:
            return 720.0f;
        case DialogWidth::Form:
            return 760.0f;
    }
    return 460.0f;  // 兜底：枚举穷举，仅为满足「所有路径有返回」
}

// ---- 对话框内缩 ----
// 标题 / 正文 / 按钮一律自 x = 28 起；正文行宽 = 弹窗宽 - 56。
inline constexpr float kDialogInsetX = 28.0f;
inline constexpr float kDialogBodyInsetX = 56.0f;

} // namespace ui_kit
