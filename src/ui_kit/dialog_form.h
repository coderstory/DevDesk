// src/ui_kit/dialog_form.h
// presentation 能力层（任务 D / R2）：跨模块复制的「对话框外框 + 表单行 + 按钮对」接线。
//
// 分层依据同 toast.h（spec 2026-09-27-refactor-incremental-design §4.1）：
//   - 不放 core：dialog / button 是需要 eui_neo 实体符号的运行时能力，core 的
//     边界只允许 header-only 类型桥接；
//   - 不放任一 module_*：module_* 之间不可互相依赖（红线 R2）；
//   - 故归本层：被 module_* 与 app 层链接，core / platform 不得反向依赖本层。
//
// 收编对象（D2：13 处 components::dialog( 调用点）：
//   1. 外框接线 —— dialog(ui,id).open().screen().size().theme()[.zIndex()].onOpenChange()；
//   2. 标题行 —— 13 处一律 (28, 24) 起、宽 w-56、行高 28、700 字重（少数派差异走 options）；
//   3. 说明 / 正文 / 标签 / 校验错误行 —— 「一行文本」同一套定位排版，宽一律 w-56；
//   4. 标准「取消 / 确定」按钮对 —— (28, h-60) 与 (28 + (w-68)/2 + 12, h-60)，各 (w-68)/2 × 40。
//
// R6 零行为变化：本文件不做任何「顺手优化」——所有几何、字号、颜色、字重、
// 节点 id 均按各站点现状逐字搬运，差异（少数派的坐标/字号/宽度/按钮样式）
// 一律参数化，不统一取值。节点 id 与拆分前逐字一致：外框保持
// `<id>` / `<id>.backdrop` / `<id>.panel*`，按钮保持 `<id>.cancel` / `<id>.confirm`
// （单测按这些 id 定位，不得改）。
#pragma once

#include <components/theme.h>
#include <core/theme.h>
#include <eui/dsl.h>

#include <functional>
#include <string>

namespace ui_kit {

// ---- 标准宽度（D2：13 处对话框现状宽度的取值集合）----
//
// 具名化的目的：宽度的唯一来源，避免同一个 460 在 9 个对话框里各写一遍字面量。
enum class DialogWidth {
    Compact,    // 420 —— claude 删除确认
    Standard,   // 460 —— todo 全部 / markdown 全部
    Wide,       // 560 —— todo 详情 / 逾期
    ExtraWide,  // 720 —— claude 调用记录
    Form,       // 760 —— claude Profile 表单
};

float dialogWidthPx(DialogWidth width);

// zIndex 哨兵：不调 .zIndex()（用组件默认 1000）。claude / markdown 现状不调；
// todo 现状按 popupZ() 显式给值（栈底恰好也是 1000，栈内越高越大）。
inline constexpr int kDialogZIndexUnset = -1;

// 对话框外框：13 处逐字重复的 dialog 接线（除 content 与 zIndex 取值外全同）。
struct DialogFrame {
    std::string id;
    DialogWidth width = DialogWidth::Standard;
    float height = 0.0f;
    bool open = false;
    int zIndex = kDialogZIndexUnset;  // kDialogZIndexUnset = 不调 .zIndex()
    std::function<void(bool)> onOpenChange;

    float widthPx() const { return dialogWidthPx(width); }
};

// 绘制外框 + 执行 content。content 即原各站点的 `.content([&]{...})` 闭包，
// 其中构建的节点 id 不变。
void drawDialogFrame(eui::Ui& ui, const eui::Screen& screen, const DialogFrame& frame,
                     const components::theme::ThemeColorTokens& themeTokens,
                     const std::function<void()>& content);

// ---- 一行文本（说明 / 正文 / 标签 / 校验错误行的共用接线）----
//
// 现状排版：position(x, y) + size(宽, 高) + text + fontSize + lineHeight
//           [+ fontWeight(700)] + [wrap] + color。
struct DialogTextOptions {
    float x = 28.0f;
    float y = 24.0f;
    float inset = 56.0f;       // 文本宽 = 对话框宽 - inset（D2 现状多数派）
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

// ---- 标题行（13 处同款：宽 w-56、高 28、行高 28、700 字重）----
struct DialogTitleOptions {
    float x = 28.0f;
    float y = 24.0f;
    float inset = 56.0f;    // 文本宽 = 对话框宽 - inset（逾期标题留图标位时为 78）
    float height = 28.0f;
    float lineHeight = 28.0f;
};

void drawDialogTitle(eui::Ui& ui, const std::string& id, float dialogWidth,
                     const std::string& text, float fontSize, const eui::Color& color,
                     const DialogTitleOptions& options = {});

// ---- 标准按钮对 ----
//
// 按钮样式：13 处现状观察到 7 种取值组合，逐一逐字复刻（不合并、不近似）。
// 命名里的 Themed = 该站点走 .theme(themeTokens, primary) 而非显式 .colors()。
enum class DialogButtonStyle {
    Secondary,        // .colors(surfaceElevated/hover/active) + textPrimary + border
    SecondaryThemed,  // .theme(themeTokens,false) + textPrimary + border
    Primary,          // .colors(primary/primaryHover/primaryPressed) + onPrimary
    PrimaryThemed,    // .theme(themeTokens,true) + onPrimary
    Danger,           // .colors(error,error,error) + onPrimary
    DangerThemed,     // .theme(themeTokens,false) + .colors(error,error,error) + onPrimary
    DangerOutline,    // .colors(surfaceElevated/hover/active) + textColor(error) + border
};

struct DialogButtonSpec {
    std::string label;
    float fontSize = 15.0f;
    DialogButtonStyle style = DialogButtonStyle::Secondary;
    std::function<void()> onClick;
};

// 取消 (28, h-60) + 确定 (28 + (w-68)/2 + 12, h-60)，各 (w-68)/2 × 40。
// 两按钮的圆角一律用 t.sizes.radius。
void drawDialogButtons(eui::Ui& ui, const std::string& id, float dialogWidth,
                       float dialogHeight, const DialogButtonSpec& cancel,
                       const DialogButtonSpec& confirm, const core::theme::Tokens& t,
                       const components::theme::ThemeColorTokens& themeTokens);

} // namespace ui_kit
