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
    float height = 0.0f;  // 请求高；低于 kDialogMinH 抬到下限（框架再做视口 clamp）
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
    bool dangerConfirm = false;       // 危险变体：confirm 底 error（删除确认类，规范 §3.3）
    std::function<void(bool)> onOpenChange;
};

// 绘制标准壳 + 执行 content(area)（content 只画内容区，头/底区由壳负责）。
// 节点 id 约定：<id>.title / <id>.title.line / <id>.footer.line / <id>.cancel /
// <id>.confirm（button 子节点 <id>.bg / <id>.text 由 components::button 生成）。
void drawDialogShell(eui::Ui& ui, const eui::Screen& screen, const DialogShellSpec& spec,
                     const core::theme::Tokens& t,
                     const components::theme::ThemeColorTokens& themeTokens,
                     const std::function<void(const DialogContentArea&)>& content);

} // namespace ui_kit
