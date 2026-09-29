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
        .title(state.title)
        // 无条件覆盖正文（含空串）：组件自带默认正文是英文占位串
        //（EUI-NEO components/toast.h 的 "Short status message for
        // non-blocking feedback."），不覆盖就会在「调用方本来就没打算给正文」
        // 的站点（todo 的 setStatus 即如此）把它画成正文——2026-09-29 真机
        // 截图实锤。空串 → 空文本节点 → 视觉无正文。
        .message(state.body);
    builder.onAutoDismiss([&state] { state.dismiss(); })
        .onDismiss([&state] { state.dismiss(); })
        .build();
}

} // namespace ui_kit
