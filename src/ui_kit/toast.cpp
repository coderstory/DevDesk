// src/ui_kit/toast.cpp — toast 绘制接线的唯一实现（边界见 toast.h）。
#include <ui_kit/toast.h>

#include <components/toast.h>

namespace ui_kit {

void drawToast(eui::Ui& ui, const eui::Screen& screen, const std::string& id,
               ToastState& state,
               const components::theme::ThemeColorTokens& tokens,
               const ToastOptions& options) {
    auto builder = components::toast(ui, id);
    builder.theme(tokens)
        .screen(screen.width, screen.height)
        .visible(state.open)
        .duration(options.durationSec)
        .icon(state.isError ? options.errorIcon : options.okIcon)
        .title(state.title)
        // 无条件覆盖正文（含空串）：组件默认正文是英文占位串，不覆盖会在无正文站点画出来。
        .message(state.body);
    builder.onAutoDismiss([&state] { state.dismiss(); })
        .onDismiss([&state] { state.dismiss(); })
        .build();
}

} // namespace ui_kit
