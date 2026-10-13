// claude_ui_quota.cpp —— 余量查询弹窗绘制（契约见同名头文件）。
// 节点 id 前缀一律 `claude.dialog.quota.*`；改 id 等于改测试。
#include "claude_ui_quota.h"

#include <components/button.h>
#include <components/dropdown.h>
#include <components/input.h>

#include <ui_kit/dialog_form.h>

#include <algorithm>
#include <vector>

namespace module::claude {

using quota::QuotaDialogState;
using quota::QuotaDialogField;
using quota::QuotaProviderOption;

namespace {

namespace quotaNs = module::claude::quota;

// 几何常量（全部具名；改一处必须同步 computeQuotaDialogLayout）
constexpr float kTitleY = 20.0f;
constexpr float kTitleH = 26.0f;
constexpr float kDescY = 54.0f;
constexpr float kDescH = 34.0f;
constexpr float kProviderLabelY = 96.0f;
constexpr float kLabelH = 16.0f;
constexpr float kControlH = 40.0f;
constexpr float kFieldLabelGap = 18.0f;   // label 底 → input 顶
constexpr float kFieldGap = 12.0f;        // input 底 → 下一个 label 顶
constexpr float kBlockGap = 20.0f;        // 区块之间
constexpr float kCcBtnW = 150.0f;
constexpr float kCcBtnH = 36.0f;
constexpr float kTestBtnW = 120.0f;
constexpr float kRowGap = 10.0f;
constexpr float kBottomPad = 60.0f;       // 按钮行距弹窗底（ui_kit 同款）

}  // namespace

QuotaDialogLayout computeQuotaDialogLayout(std::size_t fieldCount) {
    QuotaDialogLayout g;
    g.providerLabelY = kProviderLabelY;
    g.providerDdY = kProviderLabelY + kLabelH + 2.0f;
    g.fieldsY = g.providerDdY + kControlH + kBlockGap;
    g.fieldPitch = kLabelH + kFieldLabelGap + kControlH + kFieldGap;
    float y = g.fieldsY + static_cast<float>(fieldCount) * g.fieldPitch;

    g.ccSwitchY = y;
    y += kCcBtnH + kRowGap;
    g.ccNoteY = y;
    y += kLabelH + kRowGap;
    g.ccDdY = y;
    y += kControlH + kBlockGap;

    g.testBtnY = y;
    y += kCcBtnH + kRowGap;
    g.resultY = y;
    y += kLabelH;

    // 底部按钮行固定 h−60（与 ui_kit::drawDialogButtons 同款）。高度 = 内容底 +
    // 底部留白；下界保证按钮行恒有 40px 可用（字段数退到 0 时不塌）。
    g.height = y + 8.0f + kBottomPad;
    g.buttonsY = g.height - kBottomPad;   // 与 drawDialogButtons 的取法一致
    g.contentBottom = g.buttonsY - 8.0f;
    return g;
}

void drawQuotaConfigDialog(eui::Ui& ui, const eui::Screen& screen,
                           const core::theme::Tokens& t,
                           const components::theme::ThemeColorTokens& themeTokens,
                           QuotaDialogState& st,
                           const QuotaDialogHooks& hooks) {
    const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Form);
    const std::vector<QuotaProviderOption> options = quotaNs::quotaProviderOptions();
    // 几何只由字段数决定（见头文件）；cc-switch 候选条数由下拉弹层承载，不占行高。
    const QuotaDialogLayout g = computeQuotaDialogLayout(st.fields.size());

    ui_kit::DialogFrame frame;
    frame.id = "claude.dialog.quota";
    frame.width = ui_kit::DialogWidth::Form;
    frame.height = g.height;
    frame.open = st.open;
    frame.onOpenChange = [sp = &st](bool v) { sp->open = v; };

    ui_kit::drawDialogFrame(ui, screen, frame, themeTokens, [&] {
        ui_kit::drawDialogTitle(ui, "claude.dialog.quota.title", w, "余量查询配置", 20.0f,
                                t.colors.textPrimary,
                                {.y = kTitleY, .height = kTitleH, .lineHeight = kTitleH});
        ui_kit::drawDialogText(
            ui, "claude.dialog.quota.desc", w,
            "先选供应商类型，再按该供应商需要补字段。凭据可以从 cc-switch 读取，"
            "也可以手动填写。",
            12.0f, t.colors.textSecondary,
            {.y = kDescY, .height = kDescH, .lineHeight = 17.0f, .wrap = true});

        // ① 供应商类型下拉
        ui.text("claude.dialog.quota.provider.label")
            .position(28.0f, g.providerLabelY).size(w - 56.0f, kLabelH)
            .text("供应商类型").fontSize(11.0f).lineHeight(kLabelH)
            .color(t.colors.textTertiary).build();
        std::vector<std::string> labels;
        labels.reserve(options.size());
        for (const QuotaProviderOption& o : options) labels.push_back(o.displayName);
        ui.stack("claude.dialog.quota.provider.ddwrap")
            .position(28.0f, g.providerDdY).size(w - 56.0f, kControlH)
            .content([&] {
                components::dropdown(ui, "claude.dialog.quota.provider.dd")
                    .size(w - 56.0f, kControlH)
                    .theme(themeTokens)
                    .items(labels)
                    .selected(std::clamp(st.providerIdx, 0,
                                         static_cast<int>(labels.size()) - 1))
                    .open(st.providerDdOpen)
                    .onOpenChange([sp = &st](bool v) { sp->providerDdOpen = v; })
                    .onChange([h = hooks.selectProvider](int idx) {
                        if (h) h(idx);
                    })
                    .build();
            })
            .build();

        // ② 动态字段（按选中 provider 的 fields() 逐行）
        float fy = g.fieldsY;
        for (std::size_t i = 0; i < st.fields.size(); ++i) {
            QuotaDialogField& f = st.fields[i];
            const std::string base = "claude.dialog.quota.field." + f.spec.key;
            ui.text(base + ".label")
                .position(28.0f, fy).size(w - 56.0f, kLabelH)
                .text(f.spec.label).fontSize(11.0f).lineHeight(kLabelH)
                .color(t.colors.textTertiary).build();
            // 字段来源标注
            ui.text(base + ".source")
                .position(28.0f + (w - 56.0f) * 0.42f, fy)
                .size((w - 56.0f) * 0.58f, kLabelH)
                .text(f.sourceLabel.empty() ? "手动填写" : f.sourceLabel)
                .fontSize(11.0f).lineHeight(kLabelH)
                .color(t.colors.textTertiary).build();
            // **secret 分支**：input 预填内容一打字就会混入 onChange 回传值（污染）。
            // secret 恒空框（明文/脱敏串都不进 InputState），已存值走 <key>.masked 回显。
                    auto builder = components::input(ui, base);
                    builder.position(28.0f, fy + kFieldLabelGap)
                        .size(w - 56.0f, kControlH)
                        .fontSize(14.0f)
                        .theme(themeTokens);
                    if (f.spec.secret) {
                        // 有值 → 占位符留空（下方「已设置」回显已说明状态）
                        builder.value(std::string())
                            .placeholder(f.value.empty() ? "点击输入该密钥"
                                                         : std::string());
                        ui.text(base + ".masked")
                            .position(28.0f + 12.0f,
                                      fy + kFieldLabelGap + kControlH * 0.5f - 8.0f)
                            .size(w - 80.0f, 16.0f)
                            .text(f.value.empty()
                                      ? std::string("尚未设置")
                                      : ("已设置：" + quotaNs::maskSecret(f.value)))
                            .fontSize(12.0f).lineHeight(16.0f)
                            .color(t.colors.textTertiary).build();
                    } else {
                        builder.value(f.value);
                    }
                    builder.onChange([h = hooks.setField, key = f.spec.key](
                                        const std::string& v) {
                        if (h) h(key, v);
                    })
                    .onCancel([sp = &st] { sp->open = false; })
                    .build();
            fy += g.fieldPitch;
        }

        // ③ cc-switch
        components::button(ui, "claude.dialog.quota.ccswitch.load")
            .position(28.0f, g.ccSwitchY).size(kCcBtnW, kCcBtnH)
            .text("从 cc-switch 读取").fontSize(t.sizes.fontSizeSmall)
            .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
            .textColor(t.colors.textPrimary).radius(t.sizes.radius)
            .border(1.0f, t.colors.borderDefault)
            .onClick([h = hooks.loadCcSwitch] { if (h) h(); })
            .build();

        // 状态行：读不到时给明确文案；空串 = 还没读，不占一行。
        std::string note;
        eui::Color noteColor = t.colors.textTertiary;
        if (st.ccSwitchLoaded && st.ccSwitch.status != quotaNs::CcSwitchSourceStatus::Ok) {
            note = "未找到 cc-switch 配置，可手动填写（" + st.ccSwitch.unavailableReason + "）";
            noteColor = t.colors.warning;
        } else if (st.ccSwitchLoaded && st.ccSwitchIdx >= 0 &&
                   static_cast<std::size_t>(st.ccSwitchIdx) <
                       st.ccSwitch.providers.size()) {
            // **上下界都判**：ccSwitchIdx 由外部 hook 写入，悬垂索引 operator[] 是 UB。
            const quota::CcSwitchProviderInfo& info =
                st.ccSwitch.providers[static_cast<std::size_t>(st.ccSwitchIdx)];
            note = "已选 cc-switch：" + info.name + "（密钥不落库，查询时现读）";
        }
        if (!note.empty()) {
            ui.text("claude.dialog.quota.ccswitch.note")
                .position(28.0f, g.ccNoteY).size(w - 56.0f, kLabelH)
                // 单行省略号：note 长度不受控，硬裁用户看不出被截（三件套 =
                // overflow==Ellipsis && maxWidth>0 && !wrap）；clip 保留作几何兜底
                .maxWidth(w - 56.0f)
                .overflow(core::TextOverflow::Ellipsis)
                .text(note).fontSize(11.0f).lineHeight(kLabelH).clip()
                .color(noteColor).build();
        }

        // 候选下拉（**恒渲染**：读不到 / 无候选时也要能看见「没有」这件事）
        std::vector<std::string> ccLabels;
        if (st.ccSwitch.status == quotaNs::CcSwitchSourceStatus::Ok) {
            for (const quota::CcSwitchProviderInfo& info : st.ccSwitch.providers) {
                ccLabels.push_back(info.name + "（" + info.baseUrl + "）");
            }
        }
        if (ccLabels.empty()) ccLabels.push_back("（无可用候选）");
        ui.stack("claude.dialog.quota.ccswitch.ddwrap")
            .position(28.0f, g.ccDdY).size(w - 56.0f, kControlH)
            .content([&] {
                components::dropdown(ui, "claude.dialog.quota.ccswitch.dd")
                    .size(w - 56.0f, kControlH)
                    .theme(themeTokens)
                    .items(ccLabels)
                    .selected(std::clamp(st.ccSwitchIdx, 0,
                                         static_cast<int>(ccLabels.size()) - 1))
                    .open(st.ccSwitchDdOpen)
                    .onOpenChange([sp = &st](bool v) { sp->ccSwitchDdOpen = v; })
                    .onChange([h = hooks.selectCcSwitch](int idx) { if (h) h(idx); })
                    .build();
            })
            .build();

        // ④ 测试连接
        components::button(ui, "claude.dialog.quota.test")
            .position(28.0f, g.testBtnY).size(kTestBtnW, kCcBtnH)
            .text(st.testing ? "测试中…" : "测试连接")
            .fontSize(t.sizes.fontSizeSmall)
            .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
            .textColor(t.colors.textPrimary).radius(t.sizes.radius)
            .border(1.0f, t.colors.borderDefault)
            .onClick([h = hooks.testConnection, sp = &st] {
                if (sp->testing) return;   // 在途不重复发（连点不重打供应商）
                sp->testing = true;
                sp->testMessage.clear();
                sp->testIsError = false;
                sp->error.clear();
                if (h) h(quotaNs::buildQuotaTestConfig(*sp));
            })
            .build();
        // 结果行 / 红字行（空串不建节点，同 drawDialogText 的 skipIfEmpty 口径）
        if (!st.testMessage.empty() || !st.error.empty()) {
            const bool isErr = !st.error.empty() || st.testIsError;
            ui.text("claude.dialog.quota.result")
                .position(28.0f, g.resultY).size(w - 56.0f, kLabelH)
                // 单行省略号（同 ccswitch.note）：文案长度不受控；clip 保留作几何兜底。
                .maxWidth(w - 56.0f)
                .overflow(core::TextOverflow::Ellipsis)
                .text(st.error.empty() ? st.testMessage : st.error)
                .fontSize(12.0f).lineHeight(kLabelH).clip()
                .color(isErr ? t.colors.error : t.colors.success).build();
        }

        // ⑤ 保存 / 取消
        ui_kit::DialogButtonSpec cancel;
        cancel.label = "取消";
        cancel.fontSize = 14.0f;
        cancel.style = ui_kit::DialogButtonStyle::Secondary;
        cancel.onClick = [sp = &st] { sp->open = false; };
        ui_kit::DialogButtonSpec confirm;
        confirm.label = "保存";
        confirm.fontSize = 14.0f;
        confirm.style = ui_kit::DialogButtonStyle::Primary;
        confirm.onClick = [h = hooks.save, sp = &st] {
            // 装配失败 → 红字 + **不调 save**（拿一行默认 QuotaConfigRow 去落库
            // = 把「校验没过」写成「配了个空配置」，静默丢用户填的东西）
            core::Result<quota::QuotaConfigRow> row =
                quotaNs::buildQuotaConfigRow(*sp, &sp->error);
            if (row.isFail()) return;
            sp->error.clear();
            if (h) h(row.value());
        };
        ui_kit::drawDialogButtons(ui, "claude.dialog.quota", w, g.height, cancel,
                                  confirm, t, themeTokens);
    });
}

}  // namespace module::claude