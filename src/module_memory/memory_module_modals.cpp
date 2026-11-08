// memory_module_modals.cpp
// 从 memory_module.cpp 拆出（2026-10-31）：四个弹窗——编辑、掩码、清空、向导。

#include "memory_module_internal.h"

#include <ui_kit/dialog_shell.h>

namespace module::memory {

void MemoryModule::composeEditDialog(eui::Ui& ui, const eui::Screen& screen,
                                     const core::theme::Tokens& t,
                                     const components::theme::ThemeColorTokens& ct) {
    if (editingId_.empty()) return;
    // need156：壳改走标准弹窗壳（头区分隔线 + 底区细线 + 右锚按钮对）；内容区
    // 自 y=69 起，按钮归底区（旧 .go 主钮 → 壳 .confirm，节点语义同源）。
    ui_kit::DialogShellSpec shell;
    shell.id = "memory.dialog.edit";
    shell.width = ui_kit::DialogWidth::Standard;
    shell.height = 240.0f;
    shell.open = true;
    shell.title = "编辑后接受";
    shell.cancel = {"取消", [this] { editingId_.clear(); }};
    shell.confirm = {"接受", [this] {
        const std::string id = editingId_;
        const std::string text = editText_;
        editingId_.clear();
        auto r = panel_.editThenAccept(id, text);
        if (r.isFail()) {
            showToast("未能执行", r.errorMessage(), true);
        } else {
            showToast("已接受", "编辑后的记忆已执行");
        }
    }};
    shell.onOpenChange = [this](bool v) {
        if (!v) editingId_.clear();
    };
    ui_kit::drawDialogShell(ui, screen, shell, t, ct,
                            [&](const ui_kit::DialogContentArea& area) {
        ui.text("memory.dialog.edit.hint")
            .position(area.x, area.y)
            .size(area.w, core::theme::scaled(20.0f))
            .text("修改记忆正文后接受；引擎将按新正文执行。")
            .fontSize(core::theme::scaled(12.0f))
            .lineHeight(core::theme::scaled(20.0f))
            .color(t.colors.textSecondary)
            .build();
        components::input(ui, "memory.dialog.edit.input")
            .position(area.x, area.y + 28.0f)
            .size(area.w, 40.0f)
            .value(editText_)
            .placeholder("记忆正文")
            .fontSize(core::theme::scaled(14.0f))
            .theme(ct)
            .onChange([this](const std::string& v) { editText_ = v; })
            .onCancel([this] { editingId_.clear(); })
            .build();
    });
}

void MemoryModule::composeMaskModal(eui::Ui& ui, const eui::Screen& screen,
                                    const core::theme::Tokens& t,
                                    const components::theme::ThemeColorTokens& ct) {
    if (!maskModalOpen_) return;
    // need156：壳改走标准弹窗壳；220 低于壳最小 240 抬到下限（规范 §3.4），
    // 确认钮走壳危险变体（原 Danger 级，语义同源）。
    ui_kit::DialogShellSpec shell;
    shell.id = "memory.dialog.mask";
    shell.width = ui_kit::DialogWidth::Standard;
    shell.height = 240.0f;
    shell.open = true;
    shell.title = "确认关闭脱敏？";
    shell.dangerConfirm = true;
    shell.cancel = {"取消", [this] { maskModalOpen_ = false; }};
    shell.confirm = {"仍要关闭", [this] {
        maskModalOpen_ = false;
        panel_.setSanitizeEnabled(false);
        showToast("脱敏已关闭", "内容将原样出机，用完请尽快重新开启", true);
    }};
    shell.onOpenChange = [this](bool v) {
        if (!v) maskModalOpen_ = false;
    };
    ui_kit::drawDialogShell(ui, screen, shell, t, ct,
                            [&](const ui_kit::DialogContentArea& area) {
        // 设计稿逐字文案（mask-modal）
        ui.text("memory.dialog.mask.body")
            .position(area.x, area.y)
            .size(area.w, core::theme::scaled(64.0f))
            .wrap()
            .text("关闭后，密钥/手机号等敏感信息将原样发送给云端大模型。"
                  "仅建议临时排查时使用，用完请及时重新开启。")
            .fontSize(core::theme::scaled(13.0f))
            .lineHeight(core::theme::scaled(20.0f))
            .color(t.colors.textSecondary)
            .build();
    });
}

void MemoryModule::composeClearModal(eui::Ui& ui, const eui::Screen& screen,
                                     const core::theme::Tokens& t,
                                     const components::theme::ThemeColorTokens& ct) {
    if (!clearModalOpen_) return;
    const auto snap = panel_.snapshot();
    // need156：壳改走标准弹窗壳；输入校验未武装（未输入「清除」）时主钮禁用
    //（规范 §4.3），武装后走壳危险变体。280 = 原 260 抬高：正文 64 + 输入 40
    // 在壳内容区（h−158）内放不下。
    const bool armed = clearInput_ == "清除";
    ui_kit::DialogShellSpec shell;
    shell.id = "memory.dialog.clear";
    shell.width = ui_kit::DialogWidth::Standard;
    shell.height = 280.0f;
    shell.open = true;
    shell.title = "清除全部记忆？";
    shell.dangerConfirm = true;
    shell.cancel = {"取消", [this] { clearModalOpen_ = false; }};
    shell.confirm.label = "确认清除";
    shell.confirm.enabled = armed;
    shell.confirm.onClick = [this, armed] {
        if (!armed) return;
        clearModalOpen_ = false;
        auto r = panel_.clearAllOwnStores();
        if (r.isFail()) {
            showToast("清除失败", r.errorMessage(), true);
        } else {
            showToast("已清除", "面板库已清空（引擎侧记忆不在本操作范围）", true);
        }
    };
    shell.onOpenChange = [this](bool v) {
        if (!v) clearModalOpen_ = false;
    };
    ui_kit::drawDialogShell(ui, screen, shell, t, ct,
                            [&](const ui_kit::DialogContentArea& area) {
        ui.text("memory.dialog.clear.body")
            .position(area.x, area.y)
            .size(area.w, core::theme::scaled(64.0f))
            .wrap()
            .text("将删除本机面板库全部 " +
                  std::to_string(snap.proposalTotal) +
                  " 条提案记录与全部消耗账面，操作不可恢复。"
                  "如确定，请在下方输入「清除」二字。")
            .fontSize(core::theme::scaled(13.0f))
            .lineHeight(core::theme::scaled(20.0f))
            .color(t.colors.textSecondary)
            .build();
        components::input(ui, "memory.dialog.clear.input")
            .position(area.x, area.y + 72.0f)
            .size(area.w, 40.0f)
            .value(clearInput_)
            .placeholder("输入：清除")
            .fontSize(core::theme::scaled(14.0f))
            .theme(ct)
            .onChange([this](const std::string& v) { clearInput_ = v; })
            .onCancel([this] { clearModalOpen_ = false; })
            .build();
    });
}

void MemoryModule::composeWizardModal(eui::Ui& ui, const eui::Screen& screen,
                                      const core::theme::Tokens& t,
                                      const components::theme::ThemeColorTokens& ct) {
    if (!wizardOpen_) return;
    panel_.refresh(true);  // 向导打开期间快步刷新，步骤文案跟着引擎状态走
    const auto snap = panel_.snapshot();
    const auto es = snap.engine;
    const auto state = es.state;
    const bool installing = state == TelememEngine::State::Preparing;
    const bool canStart = !installing && state != TelememEngine::State::Running;
    // need156：壳改走标准弹窗壳；原「关闭」/「开始安装 / 重试」两钮归底区
    // （.cancel / .confirm），内容区自 area.y=69 起、相对原 y=56 基线平移。
    ui_kit::DialogShellSpec shell;
    shell.id = "memory.dialog.wizard";
    shell.width = ui_kit::DialogWidth::Standard;
    shell.height = 348.0f;  // need:202：三步→四步，320+一行行距 28（设计 §9 裁定）
    shell.open = true;
    shell.title = "首次安装向导";
    shell.cancel = {"关闭", [this] { wizardOpen_ = false; }};
    shell.confirm.label =
        installing ? "安装中…"
                   : (state == TelememEngine::State::Running ? "重新运行安装检查"
                                                            : "开始安装 / 重试");
    shell.confirm.enabled = canStart;
    shell.confirm.onClick = [] { TelememEngine::global().startAsync(); };
    shell.onOpenChange = [this](bool v) {
        if (!v) wizardOpen_ = false;
    };
    ui_kit::drawDialogShell(ui, screen, shell, t, ct,
                            [&](const ui_kit::DialogContentArea& area) {
        // 步骤清单：检测 → venv+pip → 拉起 → 完成/失败（状态真接 TelememEngine）
        auto stepRow = [&](const char* id, float ry, const char* name,
                           const std::string& status, eui::Color color) {
            ui.text(std::string(id) + ".name")
                .position(area.x, area.y + ry)
                .size(180.0f, core::theme::scaled(22.0f))
                .text(name)
                .fontSize(core::theme::scaled(13.0f))
                .lineHeight(core::theme::scaled(22.0f))
                .fontWeight(600)
                .color(t.colors.textPrimary)
                .build();
            ui.text(std::string(id) + ".status")
                .position(area.x + 190.0f, area.y + ry)
                .size(area.w - 190.0f, core::theme::scaled(22.0f))
                .text(status)
                .fontSize(core::theme::scaled(12.0f))
                .lineHeight(core::theme::scaled(22.0f))
                .color(color)
                .build();
        };
        const auto pendingColor = t.colors.textTertiary;
        const auto okColor = t.colors.success;
        const auto doingColor = t.colors.warning;
        const auto failColor = t.colors.errorText;
        const bool idleLike = state == TelememEngine::State::Idle ||
                              state == TelememEngine::State::Stopped ||
                              state == TelememEngine::State::Failed;
        stepRow("memory.dialog.wizard.s1", 0.0f, "① Python 3.10~3.13",
                es.pythonFound
                    ? ("已检测（" + es.pythonVersion + "）")
                    : std::string(idleLike ? "未检测到——安装后重试" : "检测中…"),
                es.pythonFound
                    ? okColor
                    : (state == TelememEngine::State::Preparing ? doingColor
                                                                : failColor));
        stepRow("memory.dialog.wizard.s2", 28.0f, "② venv + pip 安装",
                state == TelememEngine::State::Preparing
                    ? (es.detail.empty() ? std::string("进行中…") : es.detail)
                    : (state == TelememEngine::State::Running
                           ? std::string("完成")
                           : std::string(idleLike ? "待开始" : "进行中…")),
                state == TelememEngine::State::Preparing
                    ? doingColor
                    : (state == TelememEngine::State::Running ? okColor
                                                              : pendingColor));
        // need:202 F-04：③ = embedding 本机包（四态映射设计 §4，文案/色槽见设计 §2.2）
        const std::string embedStatus = wizardEmbedStatusText(es);
        eui::Color embedColor = pendingColor;
        using ES = TelememEngine::EmbedState;
        switch (es.embedState) {
            case ES::Ready: embedColor = okColor; break;
            case ES::Downloading: embedColor = doingColor; break;
            case ES::Failed: embedColor = failColor; break;
            case ES::NeedDownload:
            case ES::Unknown: embedColor = pendingColor; break;
        }
        stepRow("memory.dialog.wizard.s3", 56.0f, "③ embedding 本机包",
                embedStatus, embedColor);
        // ④ 引擎拉起与健康检查（原③顺延；拉起/健康失败显红，设计 §6）
        const char* s4Status =
            state == TelememEngine::State::Running
                ? "运行中（127.0.0.1:8421）"
                : (state == TelememEngine::State::Failed ? "失败（见下方说明）"
                                                         : (idleLike ? "待开始"
                                                                     : "进行中…"));
        const auto s4Color =
            state == TelememEngine::State::Running
                ? okColor
                : (state == TelememEngine::State::Failed
                       ? failColor
                       : (state == TelememEngine::State::Preparing ? doingColor
                                                                   : pendingColor));
        stepRow("memory.dialog.wizard.s4", 84.0f, "④ 引擎拉起与健康检查",
                s4Status, s4Color);
        // 完成态 / 失败态 / 引导态（内容区相对原 y=176 基线平移）
        if (state == TelememEngine::State::Running) {
            ui.text("memory.dialog.wizard.done")
                .position(area.x, area.y + 120.0f)
                .size(area.w, core::theme::scaled(44.0f))
                .wrap()
                .text("安装完成，记忆引擎已在后台运行。关掉本向导即可正常使用。")
                .fontSize(core::theme::scaled(13.0f))
                .lineHeight(core::theme::scaled(20.0f))
                .color(okColor)
                .build();
        } else if (state == TelememEngine::State::Failed) {
            ui.text("memory.dialog.wizard.fail")
                .position(area.x, area.y + 120.0f)
                .size(area.w, core::theme::scaled(60.0f))
                .wrap()
                .text("安装失败：" + es.detail + "（可点「开始安装 / 重试」再跑一轮）")
                .fontSize(core::theme::scaled(13.0f))
                .lineHeight(core::theme::scaled(20.0f))
                .color(failColor)
                .build();
        } else {
            ui.text("memory.dialog.wizard.intro")
                .position(area.x, area.y + 120.0f)
                .size(area.w, core::theme::scaled(44.0f))
                .wrap()
                .text("本向导将检测 Python、创建虚拟环境并安装 telemem，"
                      "检查 embedding 本机包（llama-server + 模型）就位，"
                      "全程只写数据目录。")
                .fontSize(core::theme::scaled(13.0f))
                .lineHeight(core::theme::scaled(20.0f))
                .color(t.colors.textSecondary)
                .build();
        }
    });
}

// need:181（设计 §2.6）：引擎详情弹窗（#156 Standard 档）——说明文案 /
// 版本 / Python / 语义档 / 数据目录五段信息从状态带移入；语义档缺失行
// warning 加粗（异常态在弹窗内可见）；主操作（启停）留在状态带不进弹窗。
void MemoryModule::composeEngineDetailModal(eui::Ui& ui, const eui::Screen& screen,
                                            const core::theme::Tokens& t,
                                            const components::theme::ThemeColorTokens& ct) {
    if (!engineDetailOpen_) return;
    const auto snap = panel_.snapshot();
    const auto es = snap.engine;
    // need156：壳改走标准弹窗壳；「关闭」归底区 .cancel，内容区自 area.y=69 起
    //（原 y=56 基线整体 +13 平移，行距不变）。
    ui_kit::DialogShellSpec shell;
    shell.id = "memory.dialog.engine";
    shell.width = ui_kit::DialogWidth::Standard;
    shell.height = 320.0f;
    shell.open = true;
    shell.title = "telemem 记忆引擎";
    shell.cancel = {"关闭", [this] { engineDetailOpen_ = false; }};
    shell.onOpenChange = [this](bool v) {
        if (!v) engineDetailOpen_ = false;
    };
    ui_kit::drawDialogShell(ui, screen, shell, t, ct,
                            [&](const ui_kit::DialogContentArea& area) {
        ui.text("memory.dialog.engine.desc")
            .position(area.x, area.y)
            .size(area.w, core::theme::scaled(40.0f))
            .wrap()
            .text(es.detail.empty()
                      ? std::string("它替 Claude 记住你的偏好和项目背景，聊天时自动带上，"
                                    "不用每次重复交代。")
                      : es.detail)
            .fontSize(core::theme::scaled(13.0f))
            .lineHeight(core::theme::scaled(19.0f))
            .color(es.state == TelememEngine::State::Failed ? t.colors.errorText
                                                            : t.colors.textSecondary)
            .build();
        // 分组行：标签 12 textTertiary + 值 12.5 textSecondary；语义档缺失 = warning 600
        auto row = [&](const std::string& id, float ry, const std::string& k,
                       const std::string& v, bool warnValue) {
            ui.text(id + ".k")
                .position(area.x, area.y + ry)
                .size(90.0f, core::theme::scaled(20.0f))
                .text(k)
                .fontSize(core::theme::scaled(12.0f))
                .lineHeight(core::theme::scaled(20.0f))
                .color(t.colors.textTertiary)
                .build();
            ui.text(id + ".v")
                .position(area.x + 90.0f, area.y + ry)
                .size(area.w - 90.0f, core::theme::scaled(20.0f))
                .text(v)
                .fontSize(core::theme::scaled(12.5f))
                .lineHeight(core::theme::scaled(20.0f))
                .fontWeight(warnValue ? 600 : 400)
                .color(warnValue ? t.colors.warning : t.colors.textSecondary)
                .build();
        };
        float ry = 48.0f;
        row("memory.dialog.engine.ver", ry, "版本", "v1.10.1", false);
        ry += core::theme::scaled(24.0f);
        row("memory.dialog.engine.python", ry, "Python",
            engineStatusPythonLine(es), false);
        ry += core::theme::scaled(24.0f);
        const std::string embedLine = engineStatusEmbedLine(es);
        row("memory.dialog.engine.embed", ry, "语义档",
            embedLine.empty() ? std::string("待引擎启动后就绪") : embedLine,
            es.llamaMissing);
        ry += core::theme::scaled(24.0f);
        row("memory.dialog.engine.dir", ry, "数据目录",
            truncateByAdvance(engine::memoryDataDir().generic_string(),
                              area.w - 90.0f - core::theme::scaled(12.5f),
                              core::theme::scaled(12.5f)),
            false);
        // 未安装态追加 [首次安装向导]：壳只提供 .cancel/.confirm 两槽，该入口是
        // 第三个动作，故仍走 ui_kit::button 并落在壳按钮行左缘
        //（area.y+area.h+17 = h−56，与壳 .cancel 同基线）。
        const bool uninstalled = !es.embedReady || es.llamaMissing || !es.pythonFound;
        if (uninstalled) {
            ui_kit::button(ui, "memory.dialog.engine.wizard", "首次安装向导",
                           ui_kit::ButtonLevel::Secondary,
                           area.x, area.y + area.h + 17.0f, 158.0f,
                           ui_kit::kButtonHeight, t,
                           {[this] {
                               engineDetailOpen_ = false;
                               wizardOpen_ = true;
                               TelememEngine::global().refreshEmbedProbe();
                           }});
        }
    });
}

// need:183：审核卡「查看全文」只读弹窗——多行滚动可复制；不挂 onChange =
// 改动不落任何状态（编辑通路仍走「编辑后接受」弹窗）。
// need156：壳改走标准弹窗壳（头区分隔线 + 底区细线 + 右锚按钮）。
void MemoryModule::composeFullTextDialog(eui::Ui& ui, const eui::Screen& screen,
                                         const core::theme::Tokens& t,
                                         const components::theme::ThemeColorTokens& ct) {
    if (!textDialogOpen_) return;
    ui_kit::DialogShellSpec shell;
    shell.id = "memory.dialog.fulltext";
    shell.width = ui_kit::DialogWidth::Standard;
    shell.height = 420.0f;
    shell.open = true;
    shell.title = textDialogTitle_.empty() ? "提案全文" : textDialogTitle_;
    shell.cancel = {"关闭", [this] { textDialogOpen_ = false; }};
    shell.onOpenChange = [this](bool v) {
        if (!v) textDialogOpen_ = false;
    };
    ui_kit::drawDialogShell(ui, screen, shell, t, ct,
                            [&](const ui_kit::DialogContentArea& area) {
                                components::input(ui, "memory.dialog.fulltext.body")
                                    .position(area.x, area.y)
                                    .size(area.w, area.h)
                                    .value(textDialogBody_)
                                    .multiline(true)
                                    .fontSize(core::theme::scaled(13.0f))
                                    .theme(ct)
                                    .build();
                            });
}

} // namespace module::memory
