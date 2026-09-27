// src/ui_kit/toast.cpp
// toast 绘制接线的唯一实现（原因与边界见 toast.h）。
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
        .title(state.title);
    // 正文为空 = 不设正文，保持组件自带默认文案：todo 的 toast 现状即未调
    // .message()，迁移后渲染必须逐字一致（R1 零行为变化）。
    if (!state.body.empty()) {
        builder.message(state.body);
    }
    builder.onAutoDismiss([&state] { state.dismiss(); })
        .onDismiss([&state] { state.dismiss(); })
        .build();
}

} // namespace ui_kit
