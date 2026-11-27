// 通知渠道管理弹窗实现（need527）。契约见 schedule_ui_channels.h。
#include "module_schedule/schedule_ui_channels.h"

#include "core/component_theme.h"
#include "ui_kit/button.h"
#include "ui_kit/dialog_form.h"
#include "ui_kit/dialog_shell.h"
#include "ui_kit/metrics.h"

#include <components/button.h>
#include <components/input.h>
#include <components/scrollview.h>
#include <components/switch.h>
#include <components/theme.h>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace module::schedule::ui {

namespace {

constexpr float kRowH = 56.0f;       // 渠道行高
constexpr float kRowGap = 8.0f;      // 行间距
constexpr float kInputH = 32.0f;     // 输入框高
constexpr float kFieldGap = 12.0f;   // 字段纵距

}  // namespace

const std::vector<ChannelKindOption>& channelKindOptions() {
    // 顺序 = 用户心智（最常见在前）：企业微信（唯一需要外网密钥）> 系统通知 >
    // 应用内提示 > 全屏提醒 > 静默。
    static const std::vector<ChannelKindOption> kOptions = {
        {kChannelWecom, "企业微信", true},
        {kChannelOs, "系统通知", false},
        {kChannelToast, "应用内提示", false},
        {kChannelFullscreen, "全屏提醒", false},
        {kChannelSilent, "静默", false},
    };
    return kOptions;
}

std::string channelKindLabel(const std::string& kind) {
    for (const auto& opt : channelKindOptions()) {
        if (opt.key == kind) return opt.label;
    }
    return kind;
}

void drawChannelManager(eui::Ui& ui, const eui::Screen& screen,
                        const core::theme::Tokens& t, ChannelManagerState& st,
                        const ChannelManagerHost& host) {
    const auto themeTokens = core::theme::componentTokensWeChat(t);

    ui_kit::DialogShellSpec shell;
    shell.id = "schedule.channels.dialog";
    shell.width = ui_kit::DialogWidth::Wide;
    shell.height = 520.0f;
    shell.open = st.open;
    shell.zIndex = 1200;
    shell.title = st.creating ? "新建通知渠道"
                             : (st.keyEditingId != 0 ? "设置渠道密钥"
                                                     : "通知渠道");
    // 新建/设密钥走「保存」；列表态走「关闭」（新建入口用 thirdAction 左槽）
    if (st.creating || st.keyEditingId != 0) {
        shell.cancel = {"取消", [&st] {
                            st.creating = false;
                            st.keyEditingId = 0;
                            st.keyDraft.clear();
                            st.keyEditDraft.clear();
                            st.error.clear();
                        }};
        shell.confirm = {"保存", [&st, &host] {
            std::string err;
            if (st.creating) {
                const auto& opts = channelKindOptions();
                const auto it = std::find_if(
                    opts.begin(), opts.end(),
                    [&st](const ChannelKindOption& o) { return o.key == st.kindDraft; });
                const bool needsKey = it != opts.end() && it->needsKey;
                if (needsKey && st.keyDraft.empty()) {
                    st.error = "企业微信渠道需要填写群机器人 webhook key";
                    return;
                }
                err = host.create ? host.create(st.kindDraft, st.nameDraft,
                                                st.keyDraft)
                                  : std::string("宿主未接线");
                if (!err.empty()) {
                    st.error = err;
                    return;
                }
                st.creating = false;
                st.nameDraft.clear();
                st.keyDraft.clear();
                st.error.clear();
            } else if (st.keyEditingId != 0) {
                if (st.keyEditDraft.empty()) {
                    st.error = "密钥不能为空";
                    return;
                }
                err = host.setKey ? host.setKey(st.keyEditingId, st.keyEditDraft)
                                  : std::string("宿主未接线");
                if (!err.empty()) {
                    st.error = err;
                    return;
                }
                st.keyEditingId = 0;
                st.keyEditDraft.clear();
                st.error.clear();
            }
            if (host.reload) {
                const std::string relErr = host.reload();
                if (!relErr.empty()) st.error = relErr;
            }
        }};
    } else {
        shell.cancel = {"关闭", [&st, &host] {
                            st.open = false;
                            if (host.close) host.close();
                        }};
        // 新建入口（左槽第三动作）
        shell.thirdAction = {"新建渠道", [&st] {
                                 st.creating = true;
                                 st.nameDraft.clear();
                                 st.keyDraft.clear();
                                 st.kindDraft = "wecom";
                                 st.error.clear();
                             }};
        // 主钮位置留空（列表态无主动作）
        shell.confirm = {"", {}};
    }
    shell.onOpenChange = [&st](bool v) { st.open = v; };

    ui_kit::drawDialogShell(ui, screen, shell, t, themeTokens,
                            [&](const ui_kit::DialogContentArea& area) {
        const float x0 = area.x;
        const float innerW = area.w;

        // 红字（校验/落库失败）恒占首行
        if (!st.error.empty()) {
            ui.text("schedule.channels.error")
                .position(x0, area.y)
                .size(innerW, 18.0f)
                .text(st.error)
                .fontSize(t.sizes.fontSizeSmall)
                .lineHeight(18.0f)
                .color(t.colors.errorText)
                .maxWidth(innerW)
                .overflow(core::TextOverflow::Ellipsis)
                .build();
        }
        const float top = area.y + (st.error.empty() ? 0.0f : 26.0f);

        if (st.creating) {
            // ── 新建表单：名称 / 类型 / 密钥 ──
            float fy = top;
            ui.text("schedule.channels.create.name.label")
                .position(x0, fy).size(innerW, 16.0f).text("渠道名称（可留空）")
                .fontSize(t.sizes.fontSizeSmall - 1.0f).lineHeight(16.0f)
                .color(t.colors.textTertiary).build();
            components::input(ui, "schedule.channels.create.name")
                .position(x0, fy + 20.0f).size(innerW, kInputH)
                .value(st.nameDraft)
                .placeholder("如：研发群")
                .fontSize(t.sizes.fontSizeSmall)
                .theme(themeTokens)
                .onChange([&st](const std::string& v) {
                    st.nameDraft = v;
                    st.error.clear();
                })
                .build();
            fy += 20.0f + kInputH + kFieldGap;

            ui.text("schedule.channels.create.kind.label")
                .position(x0, fy).size(innerW, 16.0f).text("渠道类型")
                .fontSize(t.sizes.fontSizeSmall - 1.0f).lineHeight(16.0f)
                .color(t.colors.textTertiary).build();
            fy += 20.0f;
            // 类型 chips：按宽度均分，最多 3 列
            const auto& opts = channelKindOptions();
            const float chipGap = 8.0f;
            const int cols = 3;
            const float chipW = (innerW - chipGap * (cols - 1)) / cols;
            for (std::size_t i = 0; i < opts.size(); ++i) {
                const int col = static_cast<int>(i) % cols;
                const int row = static_cast<int>(i) / cols;
                const bool on = st.kindDraft == opts[i].key;
                components::button(ui, "schedule.channels.create.kind." +
                                          std::to_string(i))
                    .position(x0 + col * (chipW + chipGap), fy + row * (36.0f + chipGap))
                    .size(chipW, 36.0f)
                    .text(opts[i].label)
                    .fontSize(t.sizes.fontSizeSmall - 1.0f)
                    .colors(on ? t.colors.primary : t.colors.background,
                            on ? t.colors.primaryHover : t.colors.hover,
                            on ? t.colors.primaryPressed : t.colors.primarySoftBg)
                    .textColor(on ? t.colors.onPrimary : t.colors.textPrimary)
                    .radius(t.sizes.radius)
                    .border(1.0f, on ? t.colors.primary : t.colors.borderDefault)
                    .onClick([&st, key = opts[i].key] {
                        st.kindDraft = key;
                        st.error.clear();
                    })
                    .build();
            }
            const int kindRows = static_cast<int>((opts.size() + cols - 1) / cols);
            fy += kindRows * (36.0f + chipGap) + kFieldGap - chipGap;

            // 密钥行：仅 wecom 需要
            const auto it = std::find_if(
                opts.begin(), opts.end(),
                [&st](const ChannelKindOption& o) { return o.key == st.kindDraft; });
            if (it != opts.end() && it->needsKey) {
                ui.text("schedule.channels.create.key.label")
                    .position(x0, fy).size(innerW, 16.0f)
                    .text("群机器人 webhook key（只显示是否已配置，不落明文）")
                    .fontSize(t.sizes.fontSizeSmall - 1.0f).lineHeight(16.0f)
                    .color(t.colors.textTertiary).build();
                components::input(ui, "schedule.channels.create.key")
                    .position(x0, fy + 20.0f).size(innerW, kInputH)
                    .value(st.keyDraft)
                    .placeholder("粘贴 webhook 地址里的 key 参数")
                    .fontSize(t.sizes.fontSizeSmall)
                    .theme(themeTokens)
                    .onChange([&st](const std::string& v) {
                        st.keyDraft = v;
                        st.error.clear();
                    })
                    .build();
            }
            return;
        }

        if (st.keyEditingId != 0) {
            // ── 重设既有行密钥 ──
            ui.text("schedule.channels.keyedit.label")
                .position(x0, top).size(innerW, 18.0f)
                .text("为渠道 #" + std::to_string(st.keyEditingId) +
                      " 设置新的 webhook key")
                .fontSize(t.sizes.fontSizeSmall).lineHeight(18.0f)
                .color(t.colors.textSecondary).build();
            components::input(ui, "schedule.channels.keyedit.input")
                .position(x0, top + 28.0f).size(innerW, kInputH)
                .value(st.keyEditDraft)
                .placeholder("粘贴 webhook key")
                .fontSize(t.sizes.fontSizeSmall)
                .theme(themeTokens)
                .onChange([&st](const std::string& v) {
                    st.keyEditDraft = v;
                    st.error.clear();
                })
                .build();
            return;
        }

        // ── 列表态 ──
        if (st.rows.empty()) {
            ui.text("schedule.channels.empty")
                .position(x0, top).size(innerW, 40.0f)
                .text("还没有通知渠道。点左下「新建渠道」添加一个，提醒才能发出去。")
                .fontSize(t.sizes.fontSizeSmall).lineHeight(20.0f)
                .color(t.colors.textTertiary)
                .maxWidth(innerW).wrap().build();
            return;
        }
        float fy = top;
        for (std::size_t i = 0; i < st.rows.size(); ++i) {
            const ChannelRowVm& row = st.rows[i];
            const std::string rowId =
                "schedule.channels.row." + std::to_string(i);
            ui.stack(rowId)
                .position(x0, fy)
                .size(innerW, kRowH)
                .content([&] {
                    const std::string title =
                        row.name.empty() ? channelKindLabel(row.kind) : row.name;
                    ui.text(rowId + ".name")
                        .position(0.0f, 8.0f).size(innerW - 120.0f, 20.0f)
                        .text(title)
                        .fontSize(t.sizes.fontSizeSmall).lineHeight(20.0f)
                        .fontWeight(600)
                        .color(t.colors.textPrimary)
                        .maxWidth(innerW - 120.0f)
                        .overflow(core::TextOverflow::Ellipsis)
                        .build();
                    std::string sub = channelKindLabel(row.kind);
                    if (row.needsKey) {
                        sub += row.hasKey ? " · 密钥已配置" : " · 密钥未配置";
                    }
                    if (!row.enabled) sub += " · 已停用";
                    ui.text(rowId + ".sub")
                        .position(0.0f, 30.0f).size(innerW - 120.0f, 16.0f)
                        .text(sub)
                        .fontSize(t.sizes.fontSizeSmall - 2.0f).lineHeight(16.0f)
                        .color(row.needsKey && !row.hasKey ? t.colors.warning
                                                          : t.colors.textTertiary)
                        .build();
                    // 右侧：设密钥（needsKey 才有）+ 启停开关
                    float rightX = innerW - 52.0f;
                    components::toggleSwitch(ui, rowId + ".switch")
                        .position(rightX, (kRowH - 24.0f) * 0.5f)
                        .size(44.0f, 24.0f)
                        .checked(row.enabled)
                        .theme(themeTokens)
                        .onChange([&st, &host, id = row.id](bool v) {
                            const std::string err =
                                host.setEnabled ? host.setEnabled(id, v)
                                                : std::string("宿主未接线");
                            if (!err.empty()) {
                                st.error = err;
                                return;
                            }
                            if (host.reload) {
                                const std::string relErr = host.reload();
                                if (!relErr.empty()) st.error = relErr;
                            }
                        })
                        .build();
                    if (row.needsKey) {
                        const float btnW = 64.0f;
                        const float btnX = rightX - 8.0f - btnW;
                        components::button(ui, rowId + ".key")
                            .position(btnX, (kRowH - 28.0f) * 0.5f)
                            .size(btnW, 28.0f)
                            .text("设密钥")
                            .fontSize(t.sizes.fontSizeSmall - 1.0f)
                            .colors(t.colors.surfaceElevated, t.colors.hover,
                                    t.colors.active)
                            .textColor(t.colors.textPrimary)
                            .radius(t.sizes.radiusSmall)
                            .border(1.0f, t.colors.borderDefault)
                            .onClick([&st, id = row.id] {
                                st.keyEditingId = id;
                                st.keyEditDraft.clear();
                                st.error.clear();
                            })
                            .build();
                    }
                    // 行分隔线
                    ui.rect(rowId + ".line")
                        .position(0.0f, kRowH - 1.0f)
                        .size(innerW, 1.0f)
                        .color(t.colors.borderDefault)
                        .build();
                })
                .build();
            fy += kRowH + kRowGap;
        }
    });
}

}  // namespace module::schedule::ui
