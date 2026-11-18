// src/quit_save_bridge.cpp
// 退出丢稿防线桥实现（语义见 quit_save_bridge.h）。弹窗用 #156 标准壳危险变体
//（丢弃未保存修改是破坏性动作）；主题 token 每帧取 current()（跟随换肤）。
#include "quit_save_bridge.h"

#include <eui/app.h>  // app::requestExit

#include "core/component_theme.h"  // componentTokensWeChat（弹窗组件 token）
#include "core/theme.h"            // core::theme::current()
#include "ui_kit/dialog_shell.h"   // drawDialogShell / drawDialogText / kDialogBodyInsetX

namespace app::quit_save {

namespace {

HasUnsavedHandler g_hasUnsaved = nullptr;
DiscardHandler g_discard = nullptr;
ShowMainHandler g_showMain = nullptr;
bool g_dialogOpen = false;

}  // namespace

void setHandlers(HasUnsavedHandler hasUnsaved, DiscardHandler discard) {
    g_hasUnsaved = hasUnsaved;
    g_discard = discard;
}

void setShowHandler(ShowMainHandler show) {
    g_showMain = show;
}

bool guard() {
    // 未装配或无未保存修改 = 放行（退出直通，现行为不变）
    if (g_hasUnsaved == nullptr || !g_hasUnsaved()) {
        return true;
    }
    // 有未保存修改：开确认弹窗（已开则维持）并阻止本次退出。
    // 每次退出请求问一次（need272 语义），无缓存——「放弃修改并退出」清 dirty
    // 后二次 requestExit 自然放行。
    g_dialogOpen = true;
    // ②need:300：阻止 = 弹窗将画在主窗；主窗隐藏/最小化（托盘 Exit 态）时先
    // 唤起，否则用户看不见确认弹窗也无法响应（应用看似卡死不退出）。
    if (g_showMain != nullptr) {
        g_showMain();
    }
    return false;
}

void confirmDiscardAndExit() {
    if (g_discard != nullptr) {
        g_discard();
    }
    g_dialogOpen = false;
    app::requestExit();
}

void drawConfirmDialog(eui::Ui& ui, const eui::Screen& screen) {
    if (!g_dialogOpen) {
        return;
    }
    const core::theme::Tokens& t = core::theme::current();
    const components::theme::ThemeColorTokens wc =
        core::theme::componentTokensWeChat(t);

    ui_kit::DialogShellSpec spec;
    spec.id = "app.quitsave";
    spec.title = "有未保存修改";
    spec.titleColor = t.colors.errorText;  // 警示类弹窗标题提强度（规范 §3.3）
    spec.dangerConfirm = true;             // 主钮=丢弃修改，破坏性变体
    spec.open = true;
    // need337 紧凑档：老板验收「文本观感好大」实拍定因=240 底盘空白松散——
    // 内容实占（头 69+正文 66+底 73）≈208，取 212 紧凑不空
    spec.compact = true;
    spec.height = 212.0f;
    // z=1200（>#156 规范档 1000）：本弹窗画在 app 根层（主窗 compose 尾部、
    // router 页面子树之外），而 EUI 命中/渲染按「子树 max zIndex」排兄弟——页面
    // 子树内常驻着模块弹窗（z1000）与页内 toast（z1100），页面子树上界恒 ≥1100，
    // 根层弹窗取规范档 1000 会整层落后（驱动实测：被页面盖住 + 点击被编辑器截胡）。
    // 取 1200 严格压过页内最高 overlay，页面弹窗规范档不受影响。
    spec.zIndex = 1200;
    // Esc/组件自关 → 同步内标（下帧不再画）
    spec.onOpenChange = [](bool open) {
        if (!open) {
            g_dialogOpen = false;
        }
    };
    spec.cancel = {"放弃退出", [] { g_dialogOpen = false; }};
    spec.confirm = {"放弃修改并退出", [] { confirmDiscardAndExit(); }};
    ui_kit::drawDialogShell(ui, screen, spec, t, wc,
                            [&](const ui_kit::DialogContentArea& area) {
            // 文本宽口径：drawDialogText 自减 inset（见 md_module_menu_poll 同款）
            const float textW = area.w + ui_kit::kDialogBodyInsetX;
            ui_kit::drawDialogText(ui, "app.quitsave.msg.main", textW,
                                   "有未保存的修改尚未写入磁盘，退出后将丢失。",
                                   t.sizes.fontSizeBody, t.colors.textPrimary,
                                   {.y = area.y, .height = 20.0f,
                                    .lineHeight = 20.0f});
            ui_kit::drawDialogText(ui, "app.quitsave.msg.sub", textW,
                                   "「放弃修改并退出」将丢弃未保存内容；"
                                   "「放弃退出」返回应用继续编辑。",
                                   t.sizes.fontSizeSmall, t.colors.textTertiary,
                                   {.y = area.y + 30.0f, .height = 18.0f,
                                    .lineHeight = 18.0f});
        });
}

bool dialogOpenForTest() {
    return g_dialogOpen;
}

void resetForTest() {
    g_hasUnsaved = nullptr;
    g_discard = nullptr;
    g_showMain = nullptr;
    g_dialogOpen = false;
}

}  // namespace app::quit_save
