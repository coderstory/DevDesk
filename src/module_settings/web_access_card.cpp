// src/module_settings/web_access_card.cpp
// 「网页终端」设置卡实现（need#490 W6，need#2 整改）。四态：服务关闭 / 监听未绑定 /
// 已绑定验证器 / 启动失败。卡高由 webAccessCardHeight 单一来源派生，
// 卡片绘制按同一 layout() 摆位，防卡高与内容漂移。
// 对齐体系（need#2 设计 §3）：标签列 x=28 宽 110 / 值列 x=146 / 右缘锚点 w−28；
// 行高 44（表单）/32（信息）/28（分组标题），组内 8 / 组间 24。
#include "web_access_card.h"

#include "settings_ui_logic.h"

#include <core/component_theme.h>
#include <core/platform_info.h>
#include <core/render/text.h>
#include <core/time.h>

#include <components/button.h>
#include <components/input.h>

#include <ui_kit/card.h>
#include <ui_kit/form_row.h>
#include <ui_kit/wswitch.h>

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <cstdio>
#include <string>
#include <vector>

namespace module::settings {

namespace {

inline constexpr float kPadX = 28.0f;      // 内容左轨（与其余卡同口径）
inline constexpr float kQrBox = 160.0f;    // 二维码边长（设计 §3）
inline constexpr float kSwitchW = 44.0f;
inline constexpr float kSwitchH = 24.0f;
inline constexpr float kLabelW = ui_kit::kFormRowLabelWidth;  // 110 标签列
inline constexpr float kRowForm = 44.0f;    // 表单/开关行
inline constexpr float kRowInfo = 32.0f;    // 信息行
inline constexpr float kRowGroup = 28.0f;   // 分组标题行
inline constexpr float kGapIn = 8.0f;       // 组内间距
inline constexpr float kGapGroup = 24.0f;   // 分组间距
inline constexpr float kBandMinW = 560.0f;  // 并排带最小内容宽（低于回落堆叠）
inline constexpr float kBandGap = 32.0f;    // 并排两列列距
inline constexpr float kLeftColMax = 480.0f;  // 左列（验证器绑定）收缩上限
inline constexpr float kRightColMin = 280.0f; // 右列（自诊信息）min-width

std::string formatUnixTime(std::int64_t sec) {
    if (sec <= 0) return "从未登录";
    std::tm tm{};
    ::core::time::localTime(static_cast<std::time_t>(sec), &tm);
    char buf[32] = {};
    if (std::strftime(buf, sizeof(buf), "%m-%d %H:%M", &tm) == 0) return "—";
    return buf;
}

std::string formatCountdown(int sec) {
    if (sec <= 0) return "已过期";
    char buf[16] = {};
    std::snprintf(buf, sizeof(buf), "%02d:%02d", sec / 60, sec % 60);
    return buf;
}

// 卡内全部带位的单一来源（高度与绘制共用；后续调参只改这里）。
struct Layout {
    bool running = false;
    bool bound = false;
    bool unbound = false;
    bool errorShown = false;
    bool fwHint = false;
    bool stacked = false;      // 并排带回落上下堆叠（innerW < 560）
    float titleY = 0.0f;
    float hintY = 0.0f;
    float switchY = 0.0f;
    float errorY = -1.0f;
    float portY = 0.0f;
    float bandY = 0.0f;        // 并排带顶（两分组标题基线）
    float leftW = 0.0f;        // 左列宽（并排 ≤480 收缩；堆叠 = 全宽）
    float bindTitleY = 0.0f;
    float qrY = -1.0f;
    float boundY = -1.0f;
    float delY = -1.0f;
    float confirmY = -1.0f;    // 确认文案行
    float confirmBtnY = -1.0f; // 确认/取消按钮行
    float bindEmptyY = -1.0f;  // 服务关闭占位
    float leftH = 0.0f;        // 左列内容高
    float diagTitleY = 0.0f;
    float diagY = 0.0f;
    float fwY = -1.0f;
    float diagH = 0.0f;
    float height = 0.0f;
};

float infoRowsH(int rows) {
    return static_cast<float>(rows) * roundFs(kRowInfo) +
           static_cast<float>(rows - 1) * roundFs(kGapIn);
}

Layout layout(const core::WebAccessSnapshot& s, bool confirm, float cardW) {
    Layout l;
    l.running = s.running;
    l.errorShown = !s.error.empty() || s.bindState == core::WebBindState::Error;
    l.bound = s.running && s.bindTime > 0;
    l.unbound = s.running && !l.bound;
    l.fwHint = core::platform_info::osName() == "Windows";

    float y = roundFs(24.0f);
    l.titleY = y;
    y += roundFs(26.0f) + roundFs(8.0f);
    l.hintY = y;
    y += roundFs(18.0f) + roundFs(16.0f);
    l.switchY = y;
    y += roundFs(kRowForm);
    if (l.errorShown) {
        l.errorY = y;
        y += roundFs(kRowInfo);
    }
    l.portY = y;
    y += roundFs(kRowForm) + roundFs(kGapGroup);
    l.bandY = y;

    // 并排/堆叠分支（need#2 设计 §7）：innerW = 卡宽−56，<560 回落上下堆叠
    const float innerW = std::max(0.0f, cardW - 2.0f * kPadX);
    l.stacked = innerW < kBandMinW;
    l.leftW = l.stacked
                  ? innerW
                  : std::clamp(innerW - kBandGap - kRightColMin, 208.0f,
                               kLeftColMax);

    l.bindTitleY = l.bandY;
    float contentY = l.bandY + roundFs(kRowGroup) + roundFs(kGapIn);

    // 左列：验证器绑定
    if (l.unbound) {
        l.qrY = contentY;
        l.leftH = roundFs(kQrBox);
    } else if (l.bound) {
        l.boundY = contentY;
        float y2 = contentY + infoRowsH(3);
        l.delY = y2 + roundFs(kGapIn);
        y2 = l.delY + roundFs(kRowInfo);
        if (confirm) {
            l.confirmY = y2 + roundFs(kGapIn);
            l.confirmBtnY = l.confirmY + roundFs(18.0f) + roundFs(kGapIn);
            y2 = l.confirmBtnY + roundFs(kRowInfo);
        }
        l.leftH = y2 - contentY;
    } else {
        l.bindEmptyY = contentY;
        l.leftH = roundFs(kRowInfo);
    }

    // 右列：自诊信息（堆叠时下移一个左列高 + 组间 24）
    if (l.stacked) {
        l.diagTitleY = l.bandY + roundFs(kRowGroup) + roundFs(kGapIn) +
                       l.leftH + roundFs(kGapGroup);
        l.diagY = l.diagTitleY + roundFs(kRowGroup) + roundFs(kGapIn);
    } else {
        l.diagTitleY = l.bandY;
        l.diagY = contentY;
    }
    float y3 = l.diagY + infoRowsH(3);
    if (l.fwHint) {
        l.fwY = y3 + roundFs(kGapIn);
        y3 = l.fwY + roundFs(18.0f) * 2.0f;  // 12px 提示最多 2 行
    }
    l.diagH = y3 - l.diagY;

    l.height = l.stacked
                   ? l.diagY + l.diagH + roundFs(24.0f)
                   : l.bandY + roundFs(kRowGroup) + roundFs(kGapIn) +
                         std::max(l.leftH, l.diagH) + roundFs(24.0f);
    return l;
}

void textLine(eui::Ui& ui, const std::string& id, float x, float y, float w,
              const std::string& text, float fontSize, float lineH,
              eui::Color color, bool wrap = false, int weight = 400) {
    ui.text(id)
        .position(x, y)
        .size(w, lineH)
        .text(text)
        .fontSize(fs(fontSize))
        .lineHeight(lineH)
        .fontWeight(weight)
        .wrap(wrap)
        .color(color)
        .maxWidth(w)
        .overflow(core::TextOverflow::Ellipsis)
        .clip()
        .build();
}

} // namespace

float webAccessCardHeight(const core::WebAccessSnapshot& snapshot,
                          bool bindingConfirm, float cardW) {
    return layout(snapshot, bindingConfirm, cardW).height;
}

void drawWebAccessCard(const CardDrawArgs& args) {
    eui::Ui& ui = args.ui;
    const std::string& id = args.id;
    const float w = args.w;
    const core::theme::Tokens& t = args.t;
    const SettingsCardsIn& in = args.in;
    const SettingsCardActions& a = args.a;
    const core::WebAccessSnapshot& s = in.webAccess;
    const Layout l = layout(s, in.webBindingConfirm, w);
    const auto compTheme = core::theme::componentTokensWeChat(t);
    const float innerW = std::max(0.0f, w - 2.0f * kPadX);
    const float rightX = kPadX + innerW;  // 右缘锚点 = 卡宽 − 28

    const float leftRight = kPadX + l.leftW;  // 左列右缘（带内右轨）
    const float diagX = l.stacked ? kPadX : leftRight + kBandGap;
    const float diagW = l.stacked ? innerW : innerW - l.leftW - kBandGap;

    ui.stack(id).size(w, args.h).content([&] {
        ui.rect(id + ".bg")
            .position(0.0f, 0.0f)
            .size(w, args.h)
            .color(ui_kit::cardSurface(t))
            .radius(t.sizes.radiusLarge)
            .shadow(components::theme::shadow(compTheme, 3.0f, 1.0f, 0.04f,
                                              0.02f))
            .border(1.0f, t.colors.borderDefault)
            .build();

        textLine(ui, id + ".title", kPadX, roundFs(l.titleY), innerW - 56.0f,
                 "网页终端", 19.0f, roundFs(26.0f), t.colors.textPrimary,
                 false, 600);
        textLine(ui, id + ".hint", kPadX, roundFs(l.hintY), innerW,
                 "开启后，同一局域网内的设备可访问本机终端。", 12.0f,
                 roundFs(18.0f), t.colors.textTertiary);

        // 开关行：label 15/500 落标签列 + 右缘锚定开关（开关只看服务真值，启动失败自动回弹）
        textLine(ui, id + ".switch.label", kPadX,
                 roundFs(l.switchY) + (roundFs(kRowForm) - roundFs(20.0f)) * 0.5f,
                 innerW - kSwitchW - 12.0f,
                 in.webStarting ? "正在开启…" : "启用网页终端", 15.0f,
                 roundFs(20.0f), t.colors.textPrimary, false, 500);
        ui_kit::wswitch(ui, id + ".sw", s.running, rightX - kSwitchW,
                        roundFs(l.switchY) +
                            (roundFs(kRowForm) - kSwitchH) * 0.5f,
                        kSwitchW, kSwitchH, t,
                        [f = a.applyWebAccessEnabled](bool v) { f(v); });

        // 错误行：服务启动失败（端口占用 / 无权限）透出人话原因
        if (l.errorShown) {
            textLine(ui, id + ".error", kPadX, roundFs(l.errorY), innerW,
                     s.error.empty() ? "服务启动失败，请查看日志"
                                     : s.error,
                     13.0f, roundFs(kRowInfo), t.colors.errorText);
        }

        // 端口行：110 栅格（formRow 内建）+ 右轨锚定；运行中只读，停止时可编辑 + 应用
        const std::string portText =
            in.webPortDraft.empty()
                ? (s.port > 0 ? std::to_string(s.port)
                              : (s.configuredPort > 0
                                     ? std::to_string(s.configuredPort)
                                     : std::string("7878")))
                : in.webPortDraft;
        ui_kit::formRow(
            ui, id + ".port", "端口", kPadX, roundFs(l.portY), innerW,
            roundFs(kRowForm),
            [&](eui::Ui& inner, float cx, float cy, float cw, float ch) {
                const float inputW = 96.0f;
                const float applyW = 64.0f;
                const float gap = 8.0f;
                const float ctlH = 32.0f;
                const float ctlY = cy + (ch - ctlH) * 0.5f;
                if (s.running) {
                    textLine(inner, id + ".port.readonly", cx, ctlY, 140.0f,
                             portText, 14.0f, ctlH, t.colors.textPrimary);
                    textLine(inner, id + ".port.hint", cx + 148.0f, ctlY,
                             std::max(0.0f, cw - 148.0f),
                             "服务运行时不可修改", 12.0f, ctlH,
                             t.colors.textTertiary);
                } else {
                    const float applyX = cx + cw - applyW;
                    const float inputX = applyX - gap - inputW;
                    components::input(inner, id + ".port.input")
                        .position(inputX, ctlY)
                        .size(inputW, ctlH)
                        .value(portText)
                        .placeholder("7878")
                        .fontSize(fs(13.0f))
                        .theme(compTheme)
                        .onChange([f = a.setWebPortDraft](const std::string& v) {
                            f(v);
                        })
                        .onEnter([f = a.applyWebPort] { f(); })
                        .onCancel([f = a.discardWebPortDraft] { f(); })
                        .build();
                    components::button(inner, id + ".port.apply")
                        .position(applyX, ctlY)
                        .size(applyW, ctlH)
                        .text("应用")
                        .fontSize(fs(13.0f))
                        .theme(compTheme, /*primary=*/true)
                        .radius(t.sizes.radius)
                        .onClick([f = a.applyWebPort] { f(); })
                        .build();
                    const bool hasFeedback = !in.webPortFeedback.empty();
                    textLine(inner, id + ".port.feedback", cx, ctlY,
                             std::max(0.0f, inputX - gap - cx),
                             hasFeedback ? in.webPortFeedback
                                         : std::string("端口范围 1024–65535"),
                             12.0f, ctlH,
                             hasFeedback && in.webPortFeedbackError
                                 ? t.colors.errorText
                                 : t.colors.textTertiary);
                }
            },
            t);

        // 并排带：验证器绑定（左列）｜ 自诊信息（右列），分组标题同基线顶对齐
        textLine(ui, id + ".bind.title", kPadX, roundFs(l.bindTitleY), l.leftW,
                 "验证器绑定", 13.0f, roundFs(kRowGroup), t.colors.textSecondary,
                 false, 600);
        textLine(ui, id + ".diag.title", diagX, roundFs(l.diagTitleY), diagW,
                 "自诊信息", 13.0f, roundFs(kRowGroup), t.colors.textSecondary,
                 false, 600);

        if (l.unbound) {
            const bool hasQr = s.qrSize > 0 &&
                               s.qrMatrix.size() >=
                                   static_cast<std::size_t>(s.qrSize) *
                                       static_cast<std::size_t>(s.qrSize);
            if (hasQr) {
                const float box = roundFs(kQrBox);
                ui.rect(id + ".qr.bg")
                    .position(kPadX, roundFs(l.qrY))
                    .size(box, box)
                    .color(eui::Color{1.0f, 1.0f, 1.0f, 1.0f})
                    .radius(t.sizes.radiusSmall)
                    .border(1.0f, t.colors.borderDefault)
                    .build();
                constexpr int kQuiet = 4;  // 4 模块静区
                const int total = s.qrSize + kQuiet * 2;
                const float mod = box / static_cast<float>(total);
                for (int r = 0; r < s.qrSize; ++r) {
                    for (int c = 0; c < s.qrSize; ++c) {
                        if (!s.qrMatrix[static_cast<std::size_t>(r) *
                                            static_cast<std::size_t>(s.qrSize) +
                                        static_cast<std::size_t>(c)]) {
                            continue;
                        }
                        ui.rect(id + ".qr.m." + std::to_string(r) + "." +
                                std::to_string(c))
                            .position(kPadX + static_cast<float>(c + kQuiet) * mod,
                                      roundFs(l.qrY) +
                                          static_cast<float>(r + kQuiet) * mod)
                            .size(mod, mod)
                            .color(eui::Color{0.0f, 0.0f, 0.0f, 1.0f})
                            .build();
                    }
                }
            } else {
                ui.rect(id + ".qr.placeholder")
                    .position(kPadX, roundFs(l.qrY))
                    .size(roundFs(kQrBox), roundFs(kQrBox))
                    .color(t.colors.hover)
                    .radius(t.sizes.radiusSmall)
                    .border(1.0f, t.colors.borderDefault)
                    .build();
                textLine(ui, id + ".qr.placeholder.text", kPadX + 16.0f,
                         roundFs(l.qrY) + roundFs(64.0f),
                         roundFs(kQrBox) - 32.0f, "二维码生成中…", 12.0f,
                         roundFs(20.0f), t.colors.textTertiary, true);
            }
            // QR 右列：x = 28+160+20 = 208；倒计时 / 刷新 / 说明
            const float colX = kPadX + roundFs(kQrBox) + 20.0f;
            const float colW = std::max(0.0f, leftRight - colX);
            textLine(ui, id + ".qr.countdown", colX, roundFs(l.qrY), colW,
                     s.tokenRemainingSec > 0
                         ? "剩余 " + formatCountdown(s.tokenRemainingSec)
                         : std::string("已过期"),
                     13.0f, roundFs(20.0f),
                     s.tokenRemainingSec > 0 ? t.colors.textPrimary
                                             : t.colors.errorText);
            components::button(ui, id + ".qr.refresh")
                .position(colX, roundFs(l.qrY) + roundFs(28.0f))
                .size(64.0f, 32.0f)
                .text("刷新")
                .fontSize(fs(13.0f))
                .theme(compTheme, /*primary=*/s.tokenRemainingSec <= 0)
                .radius(t.sizes.radius)
                .onClick([f = a.refreshWebToken] { f(); })
                .build();
            textLine(ui, id + ".qr.desc", colX,
                     roundFs(l.qrY) + roundFs(68.0f), colW,
                     "手机扫码后按页面提示操作", 12.0f, roundFs(32.0f),
                     t.colors.textTertiary, true);
        } else if (l.bound) {
            // 已绑定三行信息（32px 信息行）
            textLine(ui, id + ".bind.state", kPadX, roundFs(l.boundY), l.leftW,
                     "验证器已绑定", 13.0f, roundFs(kRowInfo),
                     t.colors.textPrimary, false, 500);
            textLine(ui, id + ".bind.time", kPadX,
                     roundFs(l.boundY) + roundFs(kRowInfo + kGapIn), l.leftW,
                     "绑定时间：" + formatUnixTime(s.bindTime), 13.0f,
                     roundFs(kRowInfo), t.colors.textSecondary);
            textLine(ui, id + ".bind.last", kPadX,
                     roundFs(l.boundY) + roundFs(2.0f * (kRowInfo + kGapIn)),
                     l.leftW, "最近登录：" + formatUnixTime(s.lastLogin), 13.0f,
                     roundFs(kRowInfo), t.colors.textSecondary);
            // 删除绑定（危险动作：红底白字 + 行内确认），锚定左列右轨
            const float delW = 96.0f;
            const float delX = leftRight - delW;
            ui.rect(id + ".unbind")
                .position(delX, roundFs(l.delY))
                .size(delW, roundFs(kRowInfo))
                .radius(t.sizes.radius)
                .color(t.colors.error)
                .onClick([f = a.requestRevokeBinding] { f(); });
            textLine(ui, id + ".unbind.text", delX, roundFs(l.delY), delW,
                     "删除绑定", 13.0f, roundFs(kRowInfo),
                     eui::Color{1.0f, 1.0f, 1.0f, 1.0f}, false, 500);
            if (in.webBindingConfirm) {
                textLine(ui, id + ".confirm.text", kPadX,
                         roundFs(l.confirmY), l.leftW,
                         "确认删除绑定？所有网页登录会立即失效。", 12.0f,
                         roundFs(18.0f), t.colors.errorText);
                const float noW = 56.0f;
                const float yesW = 88.0f;
                components::button(ui, id + ".confirm.yes")
                    .position(leftRight - noW - kGapIn - yesW,
                              roundFs(l.confirmBtnY))
                    .size(yesW, 32.0f)
                    .text("确认删除")
                    .fontSize(fs(13.0f))
                    .theme(compTheme, /*primary=*/true)
                    .radius(t.sizes.radius)
                    .onClick([f = a.confirmRevokeBinding] { f(); })
                    .build();
                components::button(ui, id + ".confirm.no")
                    .position(leftRight - noW, roundFs(l.confirmBtnY))
                    .size(noW, 32.0f)
                    .text("取消")
                    .fontSize(fs(13.0f))
                    .theme(compTheme, /*primary=*/false)
                    .radius(t.sizes.radius)
                    .onClick([f = a.cancelRevokeBinding] { f(); })
                    .build();
            }
        } else {
            textLine(ui, id + ".bind.empty", kPadX, roundFs(l.bindEmptyY),
                     l.leftW, "开启后在此显示二维码", 13.0f,
                     roundFs(kRowInfo), t.colors.textTertiary);
        }

        // 自诊区：label-value 两列式（label 落 110 列，值列随右列栅格）
        const float diagLabelW = kLabelW;
        const float diagValueX = diagX + diagLabelW + kGapIn;
        const float diagValueW = std::max(0.0f, diagW - diagLabelW - kGapIn);
        const float d1 = roundFs(l.diagY);
        const float lh = roundFs(kRowInfo);
        const float step = roundFs(kRowInfo + kGapIn);
        textLine(ui, id + ".diag.port.label", diagX, d1, diagLabelW,
                 "监听地址", 13.0f, lh, t.colors.textSecondary);
        textLine(ui, id + ".diag.port", diagValueX, d1, diagValueW,
                 s.running ? "0.0.0.0:" + std::to_string(s.port) +
                                 "（局域网可见）"
                           : std::string("未监听"),
                 13.0f, lh, t.colors.textPrimary);
        textLine(ui, id + ".diag.online.label", diagX, d1 + step, diagLabelW,
                 "在线设备", 13.0f, lh, t.colors.textSecondary);
        textLine(ui, id + ".diag.online", diagValueX, d1 + step, diagValueW,
                 std::to_string(s.onlineCount), 13.0f, lh,
                 t.colors.textPrimary);
        textLine(ui, id + ".diag.last.label", diagX, d1 + step * 2.0f,
                 diagLabelW, "最近登录", 13.0f, lh, t.colors.textSecondary);
        textLine(ui, id + ".diag.last", diagValueX, d1 + step * 2.0f,
                 diagValueW, formatUnixTime(s.lastLogin), 13.0f, lh,
                 t.colors.textPrimary);
        if (l.fwHint) {
            textLine(ui, id + ".diag.firewall", diagX, roundFs(l.fwY), diagW,
                     "手机连不上？若为首次启动，请在 Windows 防火墙中允许 "
                     "DevDesk 访问专用网络。",
                     12.0f, roundFs(18.0f) * 2.0f, t.colors.warningText, true);
        }
    }).build();
}

} // namespace module::settings
