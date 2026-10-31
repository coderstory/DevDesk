// memory_module_modals.cpp
// 从 memory_module.cpp 拆出（2026-10-31）：四个弹窗——编辑、掩码、清空、向导。

#include "memory_module_internal.h"

namespace module::memory {

void MemoryModule::composeEditDialog(eui::Ui& ui, const eui::Screen& screen,
                                     const core::theme::Tokens& t,
                                     const components::theme::ThemeColorTokens& ct) {
    if (editingId_.empty()) return;
    const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Standard);
    ui_kit::DialogFrame frame;
    frame.id = "memory.dialog.edit";
    frame.width = ui_kit::DialogWidth::Standard;
    frame.height = 240.0f;
    frame.open = true;
    frame.onOpenChange = [this](bool v) {
        if (!v) editingId_.clear();
    };
    ui_kit::drawDialogFrame(ui, screen, frame, ct, [&] {
        ui_kit::drawDialogTitle(ui, "memory.dialog.edit.title", w, "编辑后接受",
                                20.0f, t.colors.textPrimary, {});
        ui.text("memory.dialog.edit.hint")
            .position(ui_kit::kDialogInsetX, 56.0f)
            .size(w - ui_kit::kDialogBodyInsetX, 20.0f)
            .text("修改记忆正文后接受；引擎将按新正文执行。")
            .fontSize(core::theme::scaled(12.0f))
            .lineHeight(20.0f)
            .color(t.colors.textSecondary)
            .build();
        components::input(ui, "memory.dialog.edit.input")
            .position(ui_kit::kDialogInsetX, 84.0f)
            .size(w - ui_kit::kDialogBodyInsetX, 40.0f)
            .value(editText_)
            .placeholder("记忆正文")
            .fontSize(core::theme::scaled(14.0f))
            .theme(ct)
            .onChange([this](const std::string& v) { editText_ = v; })
            .onCancel([this] { editingId_.clear(); })
            .build();
        ui_kit::button(ui, "memory.dialog.edit.cancel", "取消",
                       ui_kit::ButtonLevel::Secondary, ui_kit::kDialogInsetX,
                       150.0f, 110.0f, ui_kit::kButtonHeight, t,
                       {[this] { editingId_.clear(); }});
        ui_kit::button(ui, "memory.dialog.edit.go", "接受",
                       ui_kit::ButtonLevel::Primary,
                       w - ui_kit::kDialogBodyInsetX - 110.0f, 150.0f, 110.0f,
                       ui_kit::kButtonHeight, t,
                       {[this] {
                           const std::string id = editingId_;
                           const std::string text = editText_;
                           editingId_.clear();
                           auto r = panel_.editThenAccept(id, text);
                           if (r.isFail()) {
                               showToast("未能执行", r.errorMessage(), true);
                           } else {
                               showToast("已接受", "编辑后的记忆已执行");
                           }
                       }});
    });
}

void MemoryModule::composeMaskModal(eui::Ui& ui, const eui::Screen& screen,
                                    const core::theme::Tokens& t,
                                    const components::theme::ThemeColorTokens& ct) {
    if (!maskModalOpen_) return;
    const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Standard);
    ui_kit::DialogFrame frame;
    frame.id = "memory.dialog.mask";
    frame.width = ui_kit::DialogWidth::Standard;
    frame.height = 220.0f;
    frame.open = true;
    frame.onOpenChange = [this](bool v) {
        if (!v) maskModalOpen_ = false;
    };
    ui_kit::drawDialogFrame(ui, screen, frame, ct, [&] {
        // 设计稿逐字文案（mask-modal）
        ui_kit::drawDialogTitle(ui, "memory.dialog.mask.title", w,
                                "确认关闭脱敏？", 20.0f, t.colors.errorText, {});
        ui.text("memory.dialog.mask.body")
            .position(ui_kit::kDialogInsetX, 56.0f)
            .size(w - ui_kit::kDialogBodyInsetX, 64.0f)
            .wrap()
            .text("关闭后，密钥/手机号等敏感信息将原样发送给云端大模型。"
                  "仅建议临时排查时使用，用完请及时重新开启。")
            .fontSize(core::theme::scaled(13.0f))
            .lineHeight(20.0f)
            .color(t.colors.textSecondary)
            .build();
        ui_kit::button(ui, "memory.dialog.mask.cancel", "取消",
                       ui_kit::ButtonLevel::Secondary, ui_kit::kDialogInsetX,
                       150.0f, 110.0f, ui_kit::kButtonHeight, t,
                       {[this] { maskModalOpen_ = false; }});
        ui_kit::button(ui, "memory.dialog.mask.confirm", "仍要关闭",
                       ui_kit::ButtonLevel::Danger,
                       w - ui_kit::kDialogBodyInsetX - 130.0f, 150.0f, 130.0f,
                       ui_kit::kButtonHeight, t,
                       {[this] {
                           maskModalOpen_ = false;
                           panel_.setSanitizeEnabled(false);
                           showToast("脱敏已关闭",
                                     "内容将原样出机，用完请尽快重新开启", true);
                       }});
    });
}

void MemoryModule::composeClearModal(eui::Ui& ui, const eui::Screen& screen,
                                     const core::theme::Tokens& t,
                                     const components::theme::ThemeColorTokens& ct) {
    if (!clearModalOpen_) return;
    const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Standard);
    ui_kit::DialogFrame frame;
    frame.id = "memory.dialog.clear";
    frame.width = ui_kit::DialogWidth::Standard;
    frame.height = 260.0f;
    frame.open = true;
    frame.onOpenChange = [this](bool v) {
        if (!v) clearModalOpen_ = false;
    };
    const auto snap = panel_.snapshot();
    ui_kit::drawDialogFrame(ui, screen, frame, ct, [&] {
        ui_kit::drawDialogTitle(ui, "memory.dialog.clear.title", w,
                                "清除全部记忆？", 20.0f, t.colors.errorText, {});
        ui.text("memory.dialog.clear.body")
            .position(ui_kit::kDialogInsetX, 56.0f)
            .size(w - ui_kit::kDialogBodyInsetX, 64.0f)
            .wrap()
            .text("将删除本机面板库全部 " +
                  std::to_string(snap.proposalTotal) +
                  " 条提案记录与全部消耗账面，操作不可恢复。"
                  "如确定，请在下方输入「清除」二字。")
            .fontSize(core::theme::scaled(13.0f))
            .lineHeight(20.0f)
            .color(t.colors.textSecondary)
            .build();
        components::input(ui, "memory.dialog.clear.input")
            .position(ui_kit::kDialogInsetX, 128.0f)
            .size(w - ui_kit::kDialogBodyInsetX, 40.0f)
            .value(clearInput_)
            .placeholder("输入：清除")
            .fontSize(core::theme::scaled(14.0f))
            .theme(ct)
            .onChange([this](const std::string& v) { clearInput_ = v; })
            .onCancel([this] { clearModalOpen_ = false; })
            .build();
        ui_kit::button(ui, "memory.dialog.clear.cancel", "取消",
                       ui_kit::ButtonLevel::Secondary, ui_kit::kDialogInsetX,
                       190.0f, 110.0f, ui_kit::kButtonHeight, t,
                       {[this] { clearModalOpen_ = false; }});
        const bool armed = clearInput_ == "清除";
        ui_kit::button(ui, "memory.dialog.clear.go", "确认清除",
                       armed ? ui_kit::ButtonLevel::Danger
                             : ui_kit::ButtonLevel::Secondary,
                       w - ui_kit::kDialogBodyInsetX - 130.0f, 190.0f, 130.0f,
                       ui_kit::kButtonHeight, t,
                       {[this, armed] {
                           if (!armed) return;
                           clearModalOpen_ = false;
                           auto r = panel_.clearAllOwnStores();
                           if (r.isFail()) {
                               showToast("清除失败", r.errorMessage(), true);
                           } else {
                               showToast("已清除",
                                         "面板库已清空（引擎侧记忆不在本操作范围）",
                                         true);
                           }
                       }});
    });
}

void MemoryModule::composeWizardModal(eui::Ui& ui, const eui::Screen& screen,
                                      const core::theme::Tokens& t,
                                      const components::theme::ThemeColorTokens& ct) {
    if (!wizardOpen_) return;
    const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Standard);
    ui_kit::DialogFrame frame;
    frame.id = "memory.dialog.wizard";
    frame.width = ui_kit::DialogWidth::Standard;
    frame.height = 320.0f;
    frame.open = true;
    frame.onOpenChange = [this](bool v) {
        if (!v) wizardOpen_ = false;
    };
    panel_.refresh(true);  // 向导打开期间快步刷新，步骤文案跟着引擎状态走
    const auto snap = panel_.snapshot();
    const auto es = snap.engine;
    ui_kit::drawDialogFrame(ui, screen, frame, ct, [&] {
        ui_kit::drawDialogTitle(ui, "memory.dialog.wizard.title", w,
                                "首次安装向导", 20.0f, t.colors.textPrimary, {});
        // 步骤清单：检测 → venv+pip → 拉起 → 完成/失败（状态真接 TelememEngine）
        auto stepRow = [&](const char* id, float ry, const char* name,
                           const std::string& status, eui::Color color) {
            ui.text(std::string(id) + ".name")
                .position(ui_kit::kDialogInsetX, ry)
                .size(180.0f, 22.0f)
                .text(name)
                .fontSize(core::theme::scaled(13.0f))
                .lineHeight(22.0f)
                .fontWeight(600)
                .color(t.colors.textPrimary)
                .build();
            ui.text(std::string(id) + ".status")
                .position(ui_kit::kDialogInsetX + 190.0f, ry)
                .size(w - ui_kit::kDialogBodyInsetX - 190.0f, 22.0f)
                .text(status)
                .fontSize(core::theme::scaled(12.0f))
                .lineHeight(22.0f)
                .color(color)
                .build();
        };
        const auto state = es.state;
        const auto pendingColor = t.colors.textTertiary;
        const auto okColor = t.colors.success;
        const auto doingColor = t.colors.warning;
        const auto failColor = t.colors.errorText;
        const bool idleLike = state == TelememEngine::State::Idle ||
                              state == TelememEngine::State::Stopped ||
                              state == TelememEngine::State::Failed;
        stepRow("memory.dialog.wizard.s1", 56.0f, "① Python 3.10~3.12",
                es.pythonFound
                    ? ("已检测（" + es.pythonVersion + "）")
                    : std::string(idleLike ? "未检测到——安装后重试" : "检测中…"),
                es.pythonFound
                    ? okColor
                    : (state == TelememEngine::State::Preparing ? doingColor
                                                                : failColor));
        stepRow("memory.dialog.wizard.s2", 84.0f, "② venv + pip 安装",
                state == TelememEngine::State::Preparing
                    ? (es.detail.empty() ? std::string("进行中…") : es.detail)
                    : (state == TelememEngine::State::Running
                           ? std::string("完成")
                           : std::string(idleLike ? "待开始" : "进行中…")),
                state == TelememEngine::State::Preparing
                    ? doingColor
                    : (state == TelememEngine::State::Running ? okColor
                                                              : pendingColor));
        stepRow("memory.dialog.wizard.s3", 112.0f, "③ 引擎拉起与健康检查",
                state == TelememEngine::State::Running
                    ? "运行中（127.0.0.1:8421）"
                    : std::string(idleLike ? "待开始" : "进行中…"),
                state == TelememEngine::State::Running ? okColor : doingColor);
        // 完成态 / 失败态 / 引导态
        if (state == TelememEngine::State::Running) {
            ui.text("memory.dialog.wizard.done")
                .position(ui_kit::kDialogInsetX, 148.0f)
                .size(w - ui_kit::kDialogBodyInsetX, 44.0f)
                .wrap()
                .text("安装完成，记忆引擎已在后台运行。关掉本向导即可正常使用。")
                .fontSize(core::theme::scaled(13.0f))
                .lineHeight(20.0f)
                .color(okColor)
                .build();
        } else if (state == TelememEngine::State::Failed) {
            ui.text("memory.dialog.wizard.fail")
                .position(ui_kit::kDialogInsetX, 148.0f)
                .size(w - ui_kit::kDialogBodyInsetX, 60.0f)
                .wrap()
                .text("安装失败：" + es.detail + "（可点「重试安装」再跑一轮）")
                .fontSize(core::theme::scaled(13.0f))
                .lineHeight(20.0f)
                .color(failColor)
                .build();
        } else {
            ui.text("memory.dialog.wizard.intro")
                .position(ui_kit::kDialogInsetX, 148.0f)
                .size(w - ui_kit::kDialogBodyInsetX, 44.0f)
                .wrap()
                .text("本向导将检测 Python、创建虚拟环境并 pip 安装 telemem，"
                      "全程只写数据目录，不捆绑解释器。")
                .fontSize(core::theme::scaled(13.0f))
                .lineHeight(20.0f)
                .color(t.colors.textSecondary)
                .build();
        }
        ui_kit::button(ui, "memory.dialog.wizard.close", "关闭",
                       ui_kit::ButtonLevel::Secondary, ui_kit::kDialogInsetX,
                       250.0f, 110.0f, ui_kit::kButtonHeight, t,
                       {[this] { wizardOpen_ = false; }});
        const bool installing = state == TelememEngine::State::Preparing;
        const bool canStart =
            !installing && state != TelememEngine::State::Running;
        ui_kit::button(ui, "memory.dialog.wizard.go",
                       installing
                           ? "安装中…"
                           : (state == TelememEngine::State::Running
                                  ? "重新运行安装检查"
                                  : "开始安装 / 重试"),
                       canStart ? ui_kit::ButtonLevel::Primary
                                : ui_kit::ButtonLevel::Secondary,
                       w - ui_kit::kDialogBodyInsetX - 190.0f, 250.0f, 190.0f,
                       ui_kit::kButtonHeight, t,
                       {canStart ? std::function<void()>([] {
                            TelememEngine::global().startAsync();
                        })
                                 : std::function<void()>()});
    });
}

} // namespace module::memory
