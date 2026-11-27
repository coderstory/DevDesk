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

// 一行文本（说明 / 正文 / 标签 / 校验错误行的共用接线）
// 现状排版：position + size + text + fontSize + lineHeight [+ fontWeight(700)] [+ wrap] + color。
struct DialogTextOptions {
    float x = kDialogInsetX;
    float y = 16.0f;  // 规格 §三.4：内容区内边距 24px，标题上缘距顶 ≤16px（原 24）
    float inset = kDialogBodyInsetX;  // 文本宽 = 对话框宽 - inset（D2 现状多数派）
    float width = 0.0f;        // > 0 = 直接用此宽（覆盖 inset；详情弹窗标签行现状）
    float height = 28.0f;      // 文本元素高
    float lineHeight = 28.0f;
    bool bold = false;         // fontWeight(600)（need322 弹窗标题档）
    bool wrap = false;         // .wrap()（仅在正文行现状出现过）
    bool skipIfEmpty = false;  // 空串 = 不绘制（校验错误行）
};

void drawDialogText(eui::Ui& ui, const std::string& id, float dialogWidth,
                    const std::string& text, float fontSize, const eui::Color& color,
                    const DialogTextOptions& options = {});

// 弹窗标题字号（2026-11-01 S10 统一）：此前 25 处 drawDialogTitle 传 20.0f，
// 而 release 自绘弹窗标题另有两套（dialogHeading 19/600、新建任务 20/600）——
// 同一类控件三个规格。定死为唯一来源。
inline constexpr float kDialogTitleFontSize = 20.0f;

// 标题行（宽 w-56、高 28、行高 28、600 字重）
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

} // namespace ui_kit
