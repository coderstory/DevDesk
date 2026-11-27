// src/ui_kit/dialog_form.cpp — 对话框文本行 / 标题行的唯一实现（边界见 .h）。
// need533（批次 C-1）：外框 drawDialogFrame 与按钮对 drawDialogButtons 已删
// （全仓零调用，弹窗壳已统一走 dialog_shell）。
#include <ui_kit/dialog_form.h>

namespace ui_kit {

void drawDialogText(eui::Ui& ui, const std::string& id, float dialogWidth,
                    const std::string& text, float fontSize, const eui::Color& color,
                    const DialogTextOptions& options) {
    // 校验错误行是「空串不建节点」，不是画空文本。
    if (options.skipIfEmpty && text.empty()) {
        return;
    }
    const float width =
        options.width > 0.0f ? options.width : (dialogWidth - options.inset);

    auto builder = ui.text(id);
    // need530（批次 B 步骤 1）：height / lineHeight 也必须进字号缩放通道。
    // 此前只缩 fontSize —— 130% 档字号涨而行盒不涨，文字被裁（硬契约同
    // scaledText：options.height / lineHeight 必须传**未缩放基准值**）。
    builder.position(options.x, options.y)
        .size(width, core::theme::scaled(options.height))
        .text(text)
        .fontSize(core::theme::scaled(fontSize))
        .lineHeight(core::theme::scaled(options.lineHeight));
    if (options.bold) {
        builder.fontWeight(600);  // 弹窗标题档 600（need322 字重豁免名单：700→600 随迁）
    }
    if (options.wrap) {
        builder.wrap();
    }
    builder.color(color).build();
}

void drawDialogTitle(eui::Ui& ui, const std::string& id, float dialogWidth,
                     const std::string& text, float fontSize, const eui::Color& color,
                     const DialogTitleOptions& options) {
    DialogTextOptions textOptions;
    textOptions.x = options.x;
    textOptions.y = options.y;
    textOptions.inset = options.inset;
    textOptions.height = options.height;
    textOptions.lineHeight = options.lineHeight;
    textOptions.bold = true;  // 标题一律 600（need322：弹窗标题档 700→600 随迁）
    drawDialogText(ui, id, dialogWidth, text, fontSize, color, textOptions);
}

} // namespace ui_kit
