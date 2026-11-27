// src/ui_kit/dialog_shell.h
// 标准弹窗壳（2026-11-07 弹窗规范 need156，设计权威
// docs/design/2026-11-07-dialog-standard-design.md）：
// 头区（标题 20/700 + 分隔线）+ 内容区（唯一内滚区）+ 底区（细线 + 右锚按钮对）
// + 最小尺寸下限。参数全取 theme token，几何单一来源见 ui_kit/metrics.h。
//
// 与 dialog_form（DialogFrame/drawDialogButtons）的关系：本组件是规范落地的
// 新壳，供新迁移弹窗使用；旧 drawDialogButtons（左锚半宽）在存量弹窗迁完前保留。
#pragma once

#include <components/theme.h>
#include <core/theme.h>
#include <eui/dsl.h>
#include <ui_kit/dialog_form.h>  // DialogWidth / dialogWidthPx / kDialogZIndexUnset / kDialogTitleFontSize
#include <ui_kit/metrics.h>

#include <functional>
#include <optional>
#include <string>

namespace ui_kit {

// 内容区几何（坐标相对弹窗面板左上角）：x=24、y=69、宽=w-48、高=h-158。
struct DialogContentArea {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

// 壳底按钮：label 空 = 不渲染该按钮。enabled=false 时主钮灰底禁用字
//（primarySoftBg/textDisabled）、副钮维持底边但字置灰（规范 §4.3 禁用态）。
struct DialogShellButton {
    std::string label;
    std::function<void()> onClick;
    bool enabled = true;
};

struct DialogShellSpec {
    std::string id;
    DialogWidth width = DialogWidth::Standard;
    float height = 0.0f;  // 请求高；低于 dialogMinH() 抬到下限（框架再做视口 clamp）
    bool open = false;
    int zIndex = kDialogZIndexUnset;  // kDialogZIndexUnset = 不调 .zIndex()（默认 1000）
    std::string title;                // 头区标题（20/700，色见 titleColor）
    // 头区可选图标（FontAwesome codepoint；0 = 不画）。画在标题左侧 x=24，标题
    // 随之右移让位（x=50、inset=78，与 todo 逾期弹窗原自绘一致）。
    unsigned int titleIcon = 0;
    // 标题色（缺省 textPrimary）。警示类弹窗传 errorText 提强度。
    std::optional<eui::Color> titleColor;
    DialogShellButton cancel;         // 副按钮（左）：关闭/取消
    DialogShellButton confirm;        // 主按钮（右）：应用/确认
    // 第三动作（可选）：按钮行左缘锚定（x=24，与右锚组同基线），副按钮样式。
    // 低频旁路入口（如未安装向导）专用，禁放主流程动作。
    DialogShellButton thirdAction;
    // need338：第三动作危险变体（左槽删除类）：底 error 三态 + onPrimary 字，
    // 与 dangerConfirm 主钮同款视觉；默认 false = 副钮样式。
    bool dangerThird = false;
    // need337：内容自然高供给（可选）：给定内容区宽（w−48）返回自然内容高。
    // 提供时请求高 = 头 + provider + 底+隙（spec.height 不参与），仍过 dialogMinH()
    // 下限与视口 clamp；内容被压缩（area.h < 自然高）时由调用方 content 内自包
    // 滚动，内容底 = area.y + area.h = h−89。
    std::function<float(float contentW)> contentHeightProvider;
    bool dangerConfirm = false;       // 危险变体：confirm 底 error（删除确认类，规范 §3.3）
    // ⚠ need531（批次 B 步骤 2）：本字段已无作用——高度下限统一降到 200，
    // 「紧凑档」成为默认行为，不再需要 opt-in 开关。字段保留仅为不打断既有
    // 调用点（quit_save / wallpaper_delete 仍写 true，语义等同于默认，无害；
    // 不用 [[deprecated]] 是因为 -Werror 会把那两处变成硬编译错）。
    // 新调用点不要再依赖它：需要更矮的弹窗请用 contentHeightProvider 由内容驱动。
    bool compact = false;
    std::function<void(bool)> onOpenChange;
};

// 绘制标准壳 + 执行 content(area)（content 只画内容区，头/底区由壳负责）。
// 节点 id 约定：<id>.title / <id>.title.line / <id>.footer.line / <id>.cancel /
// <id>.confirm / <id>.third（button 子节点 <id>.bg / <id>.text 由 components::button 生成）。
void drawDialogShell(eui::Ui& ui, const eui::Screen& screen, const DialogShellSpec& spec,
                     const core::theme::Tokens& t,
                     const components::theme::ThemeColorTokens& themeTokens,
                     const std::function<void(const DialogContentArea&)>& content);

} // namespace ui_kit
