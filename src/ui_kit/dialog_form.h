// src/ui_kit/dialog_form.h
// presentation 能力层：对话框外框 + 表单行 + 按钮对的复用接线。
//
// 分层：dialog / button 需 eui_neo 实体符号，不放 core；module_* 之间不可互相依赖，
// 故归本层（被 module_* 与 app 层链接，core / platform 不得反向依赖）。尺寸常量单一
// 来源见 ui_kit/metrics.h。差异一律参数化、不统一取值；节点 id（`<id>` /
// `<id>.cancel` / `<id>.confirm`）不得改。
#pragma once

#include <components/theme.h>
#include <core/theme.h>
#include <eui/dsl.h>
#include <ui_kit/metrics.h>

#include <functional>
#include <string>

namespace ui_kit {

// 标准宽度（DialogWidth / dialogWidthPx）与弹窗内缩（kDialogInsetX /
// kDialogBodyInsetX）的唯一来源见 ui_kit/metrics.h。

// zIndex 哨兵：不调 .zIndex()（用组件默认 1000）；todo 侧会显式给值。
inline constexpr int kDialogZIndexUnset = -1;

// 对话框外框：跨站点统一的 dialog 接线（除 content 与 zIndex 取值外全同）。
struct DialogFrame {
    std::string id;
    DialogWidth width = DialogWidth::Standard;
    float height = 0.0f;
    bool open = false;
    int zIndex = kDialogZIndexUnset;  // kDialogZIndexUnset = 不调 .zIndex()
    std::function<void(bool)> onOpenChange;

    float widthPx() const { return dialogWidthPx(width); }
};

// 绘制外框 + 执行 content（节点 id 不变）。
void drawDialogFrame(eui::Ui& ui, const eui::Screen& screen, const DialogFrame& frame,
                     const components::theme::ThemeColorTokens& themeTokens,
                     const std::function<void()>& content);

// 一行文本（说明 / 正文 / 标签 / 校验错误行的共用接线）
// 现状排版：position + size + text + fontSize + lineHeight [+ fontWeight(700)] [+ wrap] + color。
struct DialogTextOptions {
    float x = kDialogInsetX;
    float y = 16.0f;  // 规格 §三.4：内容区内边距 24px，标题上缘距顶 ≤16px（原 24）
    float inset = kDialogBodyInsetX;  // 文本宽 = 对话框宽 - inset（D2 现状多数派）
    float width = 0.0f;        // > 0 = 直接用此宽（覆盖 inset；详情弹窗标签行现状）
    float height = 28.0f;      // 文本元素高
    float lineHeight = 28.0f;
    bool bold = false;         // fontWeight(700)
    bool wrap = false;         // .wrap()（仅在正文行现状出现过）
    bool skipIfEmpty = false;  // 空串 = 不绘制（校验错误行）
};

void drawDialogText(eui::Ui& ui, const std::string& id, float dialogWidth,
                    const std::string& text, float fontSize, const eui::Color& color,
                    const DialogTextOptions& options = {});

// 标题行（宽 w-56、高 28、行高 28、700 字重）
struct DialogTitleOptions {
    float x = kDialogInsetX;
    float y = 16.0f;  // 规格 §三.4：标题上缘距弹窗顶 ≤16px（原 24）
    float inset = kDialogBodyInsetX;  // 文本宽 = 对话框宽 - inset（逾期标题留图标位时为 78）
    float height = 28.0f;
    float lineHeight = 28.0f;
};

void drawDialogTitle(eui::Ui& ui, const std::string& id, float dialogWidth,
                     const std::string& text, float fontSize, const eui::Color& color,
                     const DialogTitleOptions& options = {});

// 标准按钮对
// 9 种样式组合逐字复刻（不合并、不近似）；Themed = 走 .theme(themeTokens, primary)。
enum class DialogButtonStyle {
    Secondary,        // .colors(surfaceElevated/hover/active) + textPrimary + border
    SecondaryThemed,  // .theme(themeTokens,false) + textPrimary + border
    Primary,          // .colors(primary/primaryHover/primaryPressed) + onPrimary
    PrimaryThemed,    // .theme(themeTokens,true) + onPrimary
    Danger,           // .colors(error,error,error) + onPrimary
    DangerThemed,     // .theme(themeTokens,false) + .colors(error,error,error) + onPrimary
    DangerOutline,    // .colors(surfaceElevated/hover/active) + textColor(error) + border
    DangerOutlineThemed,  // .theme(themeTokens,false) + textColor(error) + border
};

struct DialogButtonSpec {
    std::string label;
    float fontSize = 15.0f;
    DialogButtonStyle style = DialogButtonStyle::Secondary;
    std::function<void()> onClick;
};

// 取消 (28, h-60) + 确定 (28 + (w-68)/2 + 12, h-60)，各 (w-68)/2 × 40，圆角一律 t.sizes.radius。
void drawDialogButtons(eui::Ui& ui, const std::string& id, float dialogWidth,
                       float dialogHeight, const DialogButtonSpec& cancel,
                       const DialogButtonSpec& confirm, const core::theme::Tokens& t,
                       const components::theme::ThemeColorTokens& themeTokens);

} // namespace ui_kit
