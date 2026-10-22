// src/module_claude_proxy/claude_ui_ccswitch.cpp
// 从 cc-switch 导入 Profile 弹窗：绘制 + 导入编排（头文件见分层与入口说明）
#include "claude_ui_ccswitch.h"

#include "ccswitch_import.h"

#include <components/button.h>
#include <components/scrollview.h>

#include <core/logging.h>
#include <ui_kit/dialog_form.h>
#include <ui_kit/metrics.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace module::claude {

namespace {

// 弹窗几何（单一来源，绘制与高度算式都用这里）
constexpr float kDialogHeight = 520.0f;
constexpr float kTitleY = 24.0f;
constexpr float kTitleH = 28.0f;
constexpr float kDescY = 56.0f;
constexpr float kDescH = 34.0f;
constexpr float kSelectAllY = 100.0f;
constexpr float kSelectAllH = 40.0f;  // H2：28→40 对齐规格（按钮高度 40px）
// 全选钮带（底 128→140，H2 钮高 28→40 对齐规格）与候选列表留 8px 间距（UAT6：原 4px 太挤）
constexpr float kListTopY = 148.0f;
// UAT6 P 批：行卡高 +12.5%（48→54）、行间 8→14（下边距增大）、整卡行距 56→68
constexpr float kRowPitch = 68.0f;        // 每条候选占的行高（行卡 54 + 行间 14）
constexpr float kRowCardH = kRowPitch - 14.0f;  // 行卡高 54px（原 48 +12.5%）
constexpr float kResultH = 18.0f;
constexpr float kButtonsBottomGap = 60.0f; // 底部按钮行（drawDialogButtons 写死 h-60）

// 候选行可见字段（**密钥不显示**）：name + base_url + model。映射失败行画红色
// skipReason（行本身仍可勾，勾了导入也只会进「跳过」结果）。
void drawCandidateRow(eui::Ui& ui, const core::theme::Tokens& t,
                      const components::theme::ThemeColorTokens& themeTokens,
                      const std::string& baseId, float rowW,
                      const CcSwitchImportRow& row,
                      const CcSwitchImportMapping& m, bool selected) {
    // UAT6 P 批：选中态淡绿底（#E8F7EE + 绿描边），未选中保白；
// 勾选指示绿圈白勾（加深绿 primaryPressed 防淡绿底淹没）。
    ui.rect(baseId + ".bg")
        .position(0.0f, 0.0f)
        .size(rowW, kRowCardH)
        .color(selected ? eui::Color{0xE8, 0xF7, 0xEE, 0xFF}  // 淡绿选中底
                         : t.colors.surface)
        .radius(t.sizes.radiusLarge)
        .border(1.0f, selected ? t.colors.primary          // 绿描边
                                : t.colors.borderDefault)
        .build();
    // 勾选指示器：20px 圆，选中=绿圈白勾（primaryPressed 加深防淡绿底淹没）、
    // 未选中=cardBg 白圈灰边空圈。
    constexpr float kBoxSize = 20.0f;
    const float boxY = (kRowCardH - kBoxSize) * 0.5f;
    ui.rect(baseId + ".cb.box")
        .position(12.0f, boxY)
        .size(kBoxSize, kBoxSize)
        .color(selected ? t.colors.primaryPressed : t.colors.surface)
        .radius(kBoxSize * 0.5f)
        .border(1.0f,
                selected ? t.colors.primaryPressed
                         : t.colors.borderDefault)
        .build();
    if (selected) {
        ui.text(baseId + ".cb.mark")
            .position(12.0f, boxY)
            .size(kBoxSize, kBoxSize)
            .icon(0xF00C)  // fa-check
            .fontSize(11.0f)
            .lineHeight(kBoxSize)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.onPrimary)
            .build();
    }
    // 名称（粗）+ base_url（mono 小字）+ model（小字）。**密钥一律不画**。
    // 文字四档（UAT5 米黄收尾）：name=textPrimary / url=textTertiary /
    // model=textSecondary / skip 红=warningRed，全走 wechat 档。
    // UAT6：三行文字收紧进 48 高卡内（name 5..22 / url 22..35 / model 35..47，
    // 原 model 行 40..52 溢出卡底 4px）；超宽走 Ellipsis 截断不再跑出卡框。
    const float textX = 44.0f;
    const float textW = rowW - textX - 12.0f;
    ui.text(baseId + ".name")
        .position(textX, 5.0f)
        .size(textW, 17.0f)
        .text(row.name.empty() ? std::string("（无名称）") : row.name)
        .fontSize(13.0f)
        .lineHeight(17.0f)
        .fontWeight(600)
        .maxWidth(textW)
        .wrap(false)
        .overflow(core::TextOverflow::Ellipsis)
        .color(t.colors.textPrimary)
        .build();
    ui.text(baseId + ".url")
        .position(textX, 22.0f)
        .size(textW, 13.0f)
        .text(row.appType + " · " +
              (m.ok ? m.baseUrl : std::string("—")))
        .fontSize(11.0f)
        .lineHeight(13.0f)
        .maxWidth(textW)
        .wrap(false)
        .overflow(core::TextOverflow::Ellipsis)
        .color(t.colors.textTertiary)
        .build();
    if (m.ok && !m.model.empty()) {
        ui.text(baseId + ".model")
            .position(textX, 35.0f)
            .size(textW, 12.0f)
            .text("model: " + m.model)
            .fontSize(11.0f)
            .lineHeight(12.0f)
            .maxWidth(textW)
            .wrap(false)
            .overflow(core::TextOverflow::Ellipsis)
            .color(t.colors.textSecondary)
            .build();
    }
    // 映射失败的行：右侧红字 skipReason（不阻断勾选，但导入选中时只会进「跳过」结果）
    if (!m.ok) {
        ui.text(baseId + ".skip")
            .position(textX, 35.0f)
            .size(textW, 12.0f)
            .text("跳过：" + m.skipReason)
            .fontSize(11.0f)
            .lineHeight(12.0f)
            .maxWidth(textW)
            .wrap(false)
            .overflow(core::TextOverflow::Ellipsis)
            .color(t.colors.error)
            .build();
    }
}

}  // namespace

void openCcSwitchImportDialog(CcSwitchImportDialogState& st) {
    st.open_ = true;
    st.loaded_ = false;
    st.resultLines_.clear();
    st.resultSuccess_ = st.resultSkipped_ = st.resultFailed_ = 0;
    // 读库 + 映射（同步：cc-switch 库只读 + 本地，无网络；compose 线程跑得动）
    CcSwitchImportRows rows = listCcSwitchImportRows();
    st.loadError_.clear();
    if (rows.status != CcSwitchImportStatus::Ok) {
        st.loadError_ = rows.unavailableReason;
        st.rows_.clear();
        st.mappings_.clear();
        st.selected_.clear();
        st.loaded_ = true;
        return;
    }
    st.rows_ = std::move(rows.rows);
    st.mappings_.clear();
    st.mappings_.reserve(st.rows_.size());
    // 现有 profile 名单此刻尚未传入（open 是按钮回调），映射层 existingNames 留空
    // → 不做冲突判定；confirmCcSwitchImport 时带 existingProfiles 重映射一次。
    for (const auto& row : st.rows_) {
        st.mappings_.push_back(
            mapCcSwitchProviderToProfile(row.name, row.settingsConfig));
    }
    st.selected_.assign(st.rows_.size(), true);   // 默认全选
    st.loaded_ = true;
}

void confirmCcSwitchImport(CcSwitchImportDialogState& st,
                           const std::vector<Profile>& existingProfiles,
                           const CcSwitchImportDialogHooks& hooks) {
    ClaudeService* svc = ClaudeService::global();
    if (svc == nullptr) {
        st.resultLines_.clear();
        st.resultLines_.push_back("服务未就绪（ClaudeService 未注册）");
        st.resultFailed_ = static_cast<int>(st.selected_.size());
        st.resultSuccess_ = st.resultSkipped_ = 0;
        if (hooks.toast) {
            hooks.toast("导入失败", "ClaudeService 未就绪", true);
        }
        return;
    }

    // 同名冲突判定用现有 profile 名 + 本次导入已成功建的名（同批内重名也跳过）
    std::vector<std::string> existingNames;
    existingNames.reserve(existingProfiles.size() + st.rows_.size());
    for (const Profile& p : existingProfiles) existingNames.push_back(p.name);

    st.resultLines_.clear();
    st.resultSuccess_ = st.resultSkipped_ = st.resultFailed_ = 0;

    for (size_t i = 0; i < st.rows_.size(); ++i) {
        if (!st.selected_[i]) continue;   // 未勾选的不进结果报告
        const CcSwitchImportRow& row = st.rows_[i];
        // 带 existingNames 重映射（捕获同批已建的重名）
        const CcSwitchImportMapping m =
            mapCcSwitchProviderToProfile(row.name, row.settingsConfig, existingNames);
        if (!m.ok) {
            st.resultLines_.push_back("跳过：" + row.name + "（" + m.skipReason + "）");
            ++st.resultSkipped_;
            continue;
        }
        // createProfile（含凭据加密）+ 二段补写 extraEnvJson（与表单 submitForm
        // 同款手法：createProfile 签名无 extraEnvJson，建行后 updateProfile 补）
        auto r = svc->createProfile(
            m.name, m.baseUrl, m.apiKey, m.model,
            /*activateIfNoneActive=*/true, /*sharedConfigId=*/0,
            m.authField, /*dialectOverride=*/"", /*fallbackProfileId=*/0,
            /*extraSettingsJson=*/m.extraSettingsJson);
        if (r.isFail()) {
            st.resultLines_.push_back("失败：" + row.name + "（" +
                                      r.errorMessage() + "）");
            ++st.resultFailed_;
            // errorMessage 不含密钥（createProfile 失败原因都是名称/URL/key 空校验
            // 或 UNIQUE 冲突或 DB 错误，无明文 key）
            continue;
        }
        // 二段补写：extraEnvJson / extraSettingsJson（与表单同款条件 + 同款字段集）
        if (!m.extraEnvJson.empty() || !m.extraSettingsJson.empty()) {
            Profile p = r.value();
            p.model = m.model;                  // 保持一致（createProfile 已写）
            p.extraEnvJson = m.extraEnvJson;
            p.extraSettingsJson = m.extraSettingsJson;
            auto u = svc->updateProfile(p);
            if (u.isFail()) {
                st.resultLines_.push_back(
                    "失败：" + row.name + "（已建，但 env 写入失败: " +
                    u.errorMessage() + "）");
                ++st.resultFailed_;
                existingNames.push_back(m.name);   // 行已建，同批后续重名会撞它
                continue;
            }
        }
        st.resultLines_.push_back("成功：" + row.name);
        ++st.resultSuccess_;
        existingNames.push_back(m.name);   // 同批重名兜底
    }

    if (hooks.markDataDirty) hooks.markDataDirty();

    if (hooks.toast) {
        const int total = st.resultSuccess_ + st.resultSkipped_ + st.resultFailed_;
        if (st.resultFailed_ > 0) {
            hooks.toast("导入完成（有失败）",
                        "成功 " + std::to_string(st.resultSuccess_) + " / 跳过 " +
                            std::to_string(st.resultSkipped_) + " / 失败 " +
                            std::to_string(st.resultFailed_) + "（共 " +
                            std::to_string(total) + " 条）",
                        true);
        } else if (st.resultSkipped_ > 0) {
            hooks.toast("导入完成",
                        "成功 " + std::to_string(st.resultSuccess_) + " / 跳过 " +
                            std::to_string(st.resultSkipped_) + "（共 " +
                            std::to_string(total) + " 条）",
                        false);
        } else {
            hooks.toast("导入成功",
                        "已导入 " + std::to_string(st.resultSuccess_) + " 条 Profile",
                        false);
        }
    }
}

void drawCcSwitchImportDialog(eui::Ui& ui, const eui::Screen& screen,
                              const core::theme::Tokens& t,
                              const components::theme::ThemeColorTokens& themeTokens,
                              CcSwitchImportDialogState& st,
                              const CcSwitchImportDialogHooks& hooks) {
    const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Wide);

    ui_kit::DialogFrame frame;
    frame.id = "claude.dialog.ccswitch";
    frame.width = ui_kit::DialogWidth::Wide;
    frame.height = kDialogHeight;
    frame.open = st.open_;
    frame.onOpenChange = [sp = &st](bool v) { sp->open_ = v; };

    ui_kit::drawDialogFrame(ui, screen, frame, themeTokens, [&] {
        ui_kit::drawDialogTitle(ui, "claude.dialog.ccswitch.title", w,
                                "从 CC 导入", 20.0f, t.colors.textPrimary,
                                {.y = kTitleY, .height = kTitleH, .lineHeight = kTitleH});
        ui_kit::drawDialogText(
            ui, "claude.dialog.ccswitch.desc", w,
            "从 CC 库（~/.cc-switch/cc-switch.db）只读读取 Claude / "
            "Claude Desktop 供应商，勾选后导入。密钥经既有加密管线入库，"
            "不在弹窗里显示。",
            12.0f, t.colors.textSecondary,
            {.y = kDescY, .height = kDescH, .lineHeight = 17.0f, .wrap = true});

        // 错误态：读库失败
        if (!st.loadError_.empty()) {
            ui_kit::drawDialogText(
                ui, "claude.dialog.ccswitch.err", w, st.loadError_, 13.0f,
                t.colors.error,
                {.y = kListTopY, .height = 60.0f, .lineHeight = 20.0f, .wrap = true});
        } else if (st.rows_.empty()) {
            ui_kit::drawDialogText(
                ui, "claude.dialog.ccswitch.empty", w,
                "CC 里没有 Claude / Claude Desktop 供应商，无内容可导入",
                13.0f, t.colors.textTertiary,
                {.y = kListTopY, .height = 40.0f, .lineHeight = 20.0f});
        } else {
            // 全选 / 取消全选（button 文本，避 checkbox 头告警）
            const int selCount = static_cast<int>(
                std::count(st.selected_.begin(), st.selected_.end(), true));
            const bool allSelected = (selCount == static_cast<int>(st.rows_.size()));
            components::button(ui, "claude.dialog.ccswitch.selall")
                .position(ui_kit::kDialogInsetX, kSelectAllY)
                .size(120.0f, kSelectAllH)
                .text(allSelected ? "取消全选" : "全选")
                .fontSize(14.0f)  // H2：fontSizeSmall(12)→14 对齐规格（按钮文字 14/600）
                //（UAT5 米黄清理：surfaceElevated 暖橙 → WeChat Secondary 白底灰边）
                .colors(t.colors.hover,
                        t.colors.hover,
                        t.colors.hover)
                .textColor(t.colors.textPrimary)
                .radius(8.0f)  // H2：radiusWeChat(6)→8 对齐规格（按钮圆角 8）
                .border(1.0f, t.colors.borderDefault)
                .onClick([sp = &st]() {
                    const bool anySelected =
                        std::any_of(sp->selected_.begin(),
                                    sp->selected_.end(),
                                    [](bool v) { return v; });
                    std::fill(sp->selected_.begin(), sp->selected_.end(),
                              !anySelected);
                })
                .build();

            // 候选列表（scrollView，装不下时滚）
            // 列表视口高 = 弹窗高 - 列表顶 - 底部按钮行 - 结果报告区（若有）
            const float resultAreaH =
                st.resultLines_.empty() ? 0.0f
                    : std::min<float>(
                          static_cast<float>(st.resultLines_.size()) * kResultH + 8.0f,
                          120.0f);
            const float listBottom = kDialogHeight - kButtonsBottomGap -
                                     resultAreaH - 8.0f;
            const float listH = std::max(0.0f, listBottom - kListTopY);
            components::scrollView(ui, "claude.dialog.ccswitch.list")
                .position(ui_kit::kDialogInsetX, kListTopY)
                .size(w - ui_kit::kDialogBodyInsetX, listH)
                .gap(8.0f)
                .step(kRowPitch)
                .theme(themeTokens)
                .contentKey("claude.dialog.ccswitch.list." +
                            std::to_string(st.rows_.size()) + "." +
                            std::to_string(selCount))
                .content([&](eui::Ui& inner, float contentWidth, float /*vh*/) {
                    const float rowW = contentWidth;
                    for (size_t i = 0; i < st.rows_.size(); ++i) {
                        const std::string id =
                            "claude.dialog.ccswitch.row." + std::to_string(i);
                        // 行 = stack：先画行卡内容，末尾叠整行透明命中区（stack 内
                        // 后建者命中优先 = 点行卡本体即翻勾选）。
                        // UAT6 修通：hit 原是 scrollView 流式布局的**兄弟**节点——
                        // column 布局无视显式 position(0,0)，命中区被排进行间空隙，
                        // 真机点行卡落空（全选钮正常 → 只单选坏）；且幽灵占位让
                        // 视觉行距翻倍（112px/行）。收进 stack 后命中区与行卡同帧
                        // 同位，行距回归 56px/行。
                        inner.stack(id + ".wrap")
                            .size(rowW, kRowCardH)
                            .content([&] {
                                drawCandidateRow(inner, t, themeTokens, id, rowW,
                                                 st.rows_[i], st.mappings_[i],
                                                 st.selected_[i]);
                                // 勾选框点击区 = 整行（透明覆盖层；rect 无 color
                                // 时默认白会盖住选中灰底，必须显式全透明）
                                inner.rect(id + ".hit")
                                    .position(0.0f, 0.0f)
                                    .size(rowW, kRowCardH)
                                    .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
                                    .onClick([sp = &st, idx = i] {
                                        if (idx < sp->selected_.size()) {
                                            sp->selected_[idx] =
                                                !sp->selected_[idx];
                                        }
                                    })
                                    .build();
                            })
                            .build();
                    }
                })
                .build();
        }

        // 结果报告区（已跑过一次导入时画）
        if (!st.resultLines_.empty()) {
            const float resultAreaH =
                std::min<float>(
                    static_cast<float>(st.resultLines_.size()) * kResultH + 8.0f,
                    120.0f);
            const float resultTop = kDialogHeight - kButtonsBottomGap -
                                    resultAreaH - 8.0f;
            float y = resultTop;
            for (size_t i = 0; i < st.resultLines_.size() &&
                               y < resultTop + resultAreaH; ++i) {
                ui.text("claude.dialog.ccswitch.result." + std::to_string(i))
                    .position(ui_kit::kDialogInsetX, y)
                    .size(w - ui_kit::kDialogBodyInsetX, kResultH)
                    .text(st.resultLines_[i])
                    .fontSize(11.0f)
                    .lineHeight(kResultH)
                    .color(t.colors.textSecondary)
                    .build();
                y += kResultH;
            }
        }

        // 底部按钮
        ui_kit::DialogButtonSpec cancel;
        cancel.label = "关闭";
        cancel.fontSize = 14.0f;
        cancel.style = ui_kit::DialogButtonStyle::SecondaryThemed;
        cancel.onClick = [sp = &st] { sp->open_ = false; };

        ui_kit::DialogButtonSpec confirm;
        const int selCount = static_cast<int>(
            std::count(st.selected_.begin(), st.selected_.end(), true));
        confirm.label = "导入选中(" + std::to_string(selCount) + ")";
        confirm.fontSize = 14.0f;
        confirm.style = ui_kit::DialogButtonStyle::PrimaryThemed;
        confirm.onClick = [sp = &st, hooks] {
            // 按钮回调发生在下一帧 compose 之前，拿不到装配参数 → 此处现读
            // ClaudeService::global()->listProfiles() 取现有名单。
            std::vector<Profile> existing;
            if (ClaudeService* svc = ClaudeService::global(); svc != nullptr) {
                auto r = svc->listProfiles();
                if (r.isOk()) existing = std::move(r).value();
            }
            confirmCcSwitchImport(*sp, existing, hooks);
        };

        ui_kit::drawDialogButtons(ui, "claude.dialog.ccswitch", w, kDialogHeight,
                                  cancel, confirm, t, themeTokens);
    });
}

}  // namespace module::claude