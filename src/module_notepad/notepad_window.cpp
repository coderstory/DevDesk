// notepad_window.cpp — 记事本窗 compose + 窗口级键路由 + 窗内打开
//（need:273 批 1）。三段式版面（设计稿 §1.2）：工具条 44 / 提示条 32 条件 /
// 编辑区 flex / 状态栏 28。查找条 / 预览 / 字体下拉 / 格式化 = 批 2/3 面，
// 本批工具条只落「打开 / 保存」两钮（无新建钮 = N-14 硬约束）。
#include "notepad_session.h"

#include <cstdio>
#include <functional>
#include <string>
#include <utility>  // std::move（工具条 onClick 闭包搬运；std include 守卫要求显式声明）
#include <vector>

#include <components/input.h>
#include <components/theme.h>
#include <core/component_theme.h>
#include <core/input/input_types.h>
#include <core/theme.h>
#include <core/window/window_backend.h>  // requestWindowClose（M+W）
#include <eui/dsl_app.h>                 // app::requestUpdate（定义在 EUI glfw TU）
#include <eui/platform.h>                // openFileDialog（窗内打开 F-02）

#include "core/logging.h"
#include "core/notepad_file_types.h"  // 打开对话框过滤器（单一真源）
#include "notepad_file_io.h"
#include "notepad_module.h"
#include "ui_kit/light_toast.h"

namespace module::notepad {

namespace {

using InputState = components::input_detail::InputModel::InputState;

constexpr float kToolbarH = 44.0f;   // 2 + 40 + 2（设计稿 §4.2）
constexpr float kPromptH = 32.0f;    // 条件提示条（只读 / 大文件）
constexpr float kStatusH = 28.0f;    // 状态栏

// 工具条按钮（本批两枚文本钮；图标钮规格批 3 与字体下拉一起收敛到设计稿 §4.2）。
void toolbarButton(eui::Ui& ui, const char* id, const char* label, float x,
                   const core::theme::Tokens& t, bool enabled,
                   std::function<void()> onClick) {
    constexpr float kBtnW = 64.0f;
    constexpr float kBtnH = 28.0f;
    const std::string base(id);
    ui.stack(base)
        .position(x, 8.0f)
        .size(kBtnW, kBtnH)
        .disabled(!enabled)  // 真禁用（拦截点击）而非仅灰显；md 状态栏步进钮同款
        .content([&] {
            ui.rect(base + ".bg")
                .size(kBtnW, kBtnH)
                .color(enabled ? t.colors.surface : t.colors.background)
                .radius(6.0f)
                .border(1.0f, enabled ? t.colors.borderDefault
                                      : t.colors.borderMuted)
                .build();
            ui.text(base + ".label")
                .position(6.0f, 0.0f)
                .size(kBtnW - 12.0f, kBtnH)
                .text(label)
                .fontSize(13.0f)
                .lineHeight(kBtnH)
                .verticalAlign(eui::VerticalAlign::Center)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .color(enabled ? t.colors.textPrimary : t.colors.textDisabled)
                .build();
        })
        .onClick(std::move(onClick))
        .build();
}

}  // namespace

void notepadComposeSession(NotepadSession& s, eui::Ui& ui,
                           const eui::Screen& screen) {
    const core::theme::Tokens& t = core::theme::current();
    const auto componentTheme = core::theme::componentTokensWeChat(t);
    const float w = screen.width;
    const float h = screen.height;

    // 尺寸记忆回写源：每帧回填（关窗回调读不到 compose 态，只能这样取）
    s.lastW = static_cast<int>(w);
    s.lastH = static_cast<int>(h);
    s.sizeSeen = true;

    const bool errorState = !s.loadError.empty();
    const bool showPrompt = (s.readonly || s.largeFile || s.binarySuspect) &&
                            !errorState;
    const float editorTop = kToolbarH + (showPrompt ? kPromptH : 0.0f);

    // ── 工具条 44（无新建钮 = N-14）──
    ui.rect(kNotepadToolbarId)
        .position(0.0f, 0.0f)
        .size(w, kToolbarH)
        .color(t.colors.background)
        .build();
    const bool editable = !errorState && !s.readonly && !s.binarySuspect;
    toolbarButton(ui, kNotepadBtnOpenId, "打开", 16.0f, t, true, [&s] {
        notepadOpenViaDialog(s);
    });
    toolbarButton(ui, kNotepadBtnSaveId, "保存", 88.0f, t, editable, [&s] {
        notepadSaveSession(s);
    });

    // ── 提示条 32（只读降级 / 大文件模式；错误态文本直接占编辑区，不叠提示条）──
    if (showPrompt) {
        ui.rect("notepad.promptbar")
            .position(0.0f, kToolbarH)
            .size(w, kPromptH)
            .color(components::theme::withAlpha(t.colors.warning, 0.12f))
            .build();
        const char* msg = s.readonly ? s.readonlyReason.c_str()
                                     : "大文件模式：已关闭代码高亮与自动换行";
        ui.text("notepad.promptbar.text")
            .position(16.0f, kToolbarH)
            .size(w - 32.0f, kPromptH)
            .text(msg)
            .fontSize(12.0f)
            .lineHeight(kPromptH)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.textSecondary)
            .build();
    }

    // ── 编辑区 flex（唯一内滚容器；错误态 = 占位文案）──
    if (errorState) {
        ui.text("notepad.loaderror")
            .position(0.0f, editorTop)
            .size(w, h - editorTop - kStatusH)
            .text(s.loadError)
            .fontSize(14.0f)
            .lineHeight(28.0f)
            .verticalAlign(eui::VerticalAlign::Center)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .color(t.colors.textSecondary)
            .build();
    } else {
        auto& st = ui.state<InputState>(kNotepadEditorInputId);
        if (!s.seeded) {
            // 首帧播种：载入初值（LF 域）进框架 InputState；此后镜像随 onChange 回流
            s.seeded = true;
            st.text = s.editText;
            st.cursor = 0;
            st.selectionStart = 0;
            st.selectionEnd = 0;
            ++st.textRevision;
        }
        components::input(ui, kNotepadEditorInputId)
            .position(0.0f, editorTop)
            .size(w, h - editorTop - kStatusH)
            .value(s.editText)
            .multiline(true)
            .scrollbar(true)
            .fontSize(static_cast<float>(s.editorFontSize))
            .inset(10.0f)
            .theme(componentTheme)
            .onChange([&s](const std::string& v) {
                // 只读降级窗：不建编辑计账（输入回显被下帧 value 重置覆盖，脏态
                // 恒 false → 保存/关窗链路零风险）；正 经编辑 = 镜像 + 脏态单点
                if (s.readonly || s.binarySuspect) return;
                if (v == s.editText) return;
                s.editText = v;
                sessionSetDirty(s, v != s.loadText);
                app::requestUpdate();  // 状态栏「行:列 / ●未保存」实时刷新
            })
            .build();
    }

    // ── 状态栏 28（F-17 基础版：文件名｜●未保存｜行:列｜字符数）──
    ui.rect(kNotepadStatusbarId)
        .position(0.0f, h - kStatusH)
        .size(w, kStatusH)
        .color(t.colors.surface)
        .border(1.0f, t.colors.borderMuted)
        .build();
    ui.text("notepad.status.title")
        .position(16.0f, h - kStatusH)
        .size(280.0f, kStatusH)
        .text(s.title)
        .fontSize(12.0f)
        .lineHeight(kStatusH)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textSecondary)
        .build();
    if (s.dirty) {
        // x 随标题估宽：码点数 × 7px（12px 字号粗估，CJK 偏宽按余量吸收）
        const float titleW =
            static_cast<float>(notepadUtf8Length(s.title)) * 7.0f + 20.0f;
        ui.text("notepad.status.dirty")
            .position(16.0f + titleW, h - kStatusH)
            .size(90.0f, kStatusH)
            .text("●未保存")
            .fontSize(12.0f)
            .lineHeight(kStatusH)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.warning)
            .build();
    }
    if (!errorState) {
        const InputState& st = ui.state<InputState>(kNotepadEditorInputId);
        const int line = notepadByteOffsetToLine(st.text, st.cursor);
        const size_t lineStart = notepadLineStartOffset(st.text, line);
        // cursor 是 int（InputState 契约），lineStart 是 size_t：先归一为 size_t
        // 再比较，避免 -Wsign-compare（本仓 -Werror）。
        const size_t cursorBytes = static_cast<size_t>(st.cursor);
        const size_t colCp =
            cursorBytes >= lineStart
                ? notepadUtf8Length(st.text.substr(lineStart, cursorBytes - lineStart))
                : 0;
        char buf[64];
        std::snprintf(buf, sizeof buf, "行 %d, 列 %zu", line + 1, colCp + 1);
        ui.text("notepad.status.cursor")
            .position(w - 300.0f, h - kStatusH)
            .size(140.0f, kStatusH)
            .text(buf)
            .fontSize(12.0f)
            .lineHeight(kStatusH)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.textSecondary)
            .build();
        std::snprintf(buf, sizeof buf, "%zu 字符", notepadUtf8Length(st.text));
        ui.text("notepad.status.chars")
            .position(w - 150.0f, h - kStatusH)
            .size(134.0f, kStatusH)
            .text(buf)
            .fontSize(12.0f)
            .lineHeight(kStatusH)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.textSecondary)
            .build();
    }

    // ── 会话内轻提示（聚焦/保存反馈；自管 1.5s 窗口，多窗互不覆盖）──
    if (!s.toastMessage.empty() && std::chrono::steady_clock::now() < s.toastUntil) {
        ui_kit::drawLightToastPill(ui, "notepad.toast", s.toastMessage, w / 2.0f,
                                   h * 0.4f, t);
    } else if (!s.toastMessage.empty()) {
        s.toastMessage.clear();
    }
}

void notepadHandleKeySession(NotepadSession& s, const eui::KeyEvent& key) {
    if (!key.isDown()) return;
    // M=平台主修饰键（shortcut() 按 App=Super / Win=Ctrl 映射）；Shift 组合另判
    const bool shortcut =
        key.modifiers.shortcut() && !key.modifiers.shift && !key.modifiers.alt;
    if (shortcut && key.key == eui::InputKey::O) {
        notepadOpenViaDialog(s);  // F-02：窗内打开（已开路径由查重聚焦收口）
        return;
    }
    if (shortcut && key.key == eui::InputKey::S) {
        notepadSaveSession(s);  // F-03：干净短路 / mtime 拦截 / 脏重编码
        return;
    }
    if (shortcut && key.key == eui::InputKey::W && s.handle != nullptr) {
        // M+W 关窗（窗口级，不进主窗 Router）；未保存保护链批 4 于
        // onCloseRequested 挂载（本批回调直通放行）
        core::window::requestWindowClose(
            static_cast<core::window::Handle>(s.handle));
        return;
    }
}

void notepadOpenViaDialog(NotepadSession& s) {
    (void)s;
    eui::platform::FileDialogOptions options;
    options.prompt = "打开文件";
    options.filterName = "文本与代码文件";
    for (const std::string_view ext : core::notepad::notepadExtensions()) {
        options.allowedExtensions.emplace_back(ext);
    }
    const eui::platform::FileDialogResult result =
        eui::platform::openFileDialog(options);
    if (!result.selected()) return;  // 取消 → 零副作用（F-02）
    for (const std::string& path : result.paths) {
        LOG_INFO("记事本窗内打开: " + path);
        openNotepadWindow(path);  // 已开路径 → 查重命中聚焦，不开新窗
    }
    app::requestUpdate();
}

}  // namespace module::notepad
