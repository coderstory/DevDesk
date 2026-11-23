// src/module_settings/web_access_card.cpp
// 「网页访问」设置卡实现（need#490 W6）。四态：服务关闭 / 监听未绑定 /
// 已绑定验证器 / 启动失败。卡高由 webAccessCardHeight 单一来源派生，
// 卡片绘制按同一 layout() 摆位，防卡高与内容漂移。
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
inline constexpr float kQrBox = 160.0f;    // 二维码边长（设计 §5.10）
inline constexpr float kSwitchW = 44.0f;
inline constexpr float kSwitchH = 24.0f;
inline constexpr float kBtnH = 30.0f;

bool isLoopback(const std::string& addr) {
    // 服务快照给裸 IPv4（127.0.0.1）；探针/未来方言也可能给完整 URL。
    return addr.rfind("127.", 0) == 0 || addr == "localhost" || addr == "::1" ||
           addr.find("://127.") != std::string::npos ||
           addr.find("://localhost") != std::string::npos ||
           addr.find("://[::1]") != std::string::npos;
}

// 快照地址是 host（局域网 IPv4）时补 scheme+port；已带 scheme 原样返回。
std::string webUrl(const std::string& addr, int port) {
    if (addr.find("://") != std::string::npos) return addr;
    return "http://" + addr + ":" + std::to_string(port);
}

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
    bool hasLan = false;
    bool errorShown = false;
    bool fwHint = false;
    float titleY = 0.0f;
    float hintY = 0.0f;
    float switchY = 0.0f;
    float errorY = -1.0f;
    float portY = 0.0f;
    float addrTitleY = 0.0f;
    float addrY = 0.0f;
    int addrRows = 0;
    float lanWarnY = -1.0f;
    float bindTitleY = 0.0f;
    float qrY = -1.0f;
    float boundY = -1.0f;
    float delY = -1.0f;
    float confirmY = -1.0f;
    float diagTitleY = 0.0f;
    float diagY = 0.0f;
    float fwY = -1.0f;
    float logBtnY = 0.0f;
    float height = 0.0f;
};

Layout layout(const core::WebAccessSnapshot& s, bool confirm) {
    Layout l;
    l.running = s.running;
    l.errorShown = !s.error.empty() || s.bindState == core::WebBindState::Error;
    l.bound = s.running && s.bindTime > 0;
    l.unbound = s.running && !l.bound;
    l.addrRows = s.running ? std::max<int>(1, static_cast<int>(s.addresses.size())) : 0;
    for (const auto& a : s.addresses) {
        if (!isLoopback(a)) l.hasLan = true;
    }
    l.fwHint = core::platform_info::osName() == "Windows";

    float y = roundFs(16.0f);
    l.titleY = y;
    y += roundFs(22.0f) + roundFs(8.0f);
    l.hintY = y;
    y += roundFs(16.0f) + roundFs(10.0f);
    l.switchY = y;
    y += roundFs(44.0f);
    if (l.errorShown) {
        l.errorY = y;
        y += roundFs(18.0f);
    }
    l.portY = y;
    y += roundFs(44.0f) + roundFs(8.0f);
    l.addrTitleY = y;
    y += roundFs(18.0f);
    l.addrY = y;
    if (s.running) {
        y += static_cast<float>(l.addrRows) * roundFs(30.0f);
        if (!l.hasLan) {
            l.lanWarnY = y;
            y += roundFs(18.0f);
        }
    } else {
        y += roundFs(24.0f);
    }
    y += roundFs(10.0f);
    l.bindTitleY = y;
    y += roundFs(18.0f);
    if (l.unbound) {
        l.qrY = y;
        y += roundFs(kQrBox) + roundFs(8.0f);
    } else if (l.bound) {
        l.boundY = y;
        y += roundFs(22.0f) * 3.0f + roundFs(8.0f);
        l.delY = y;
        y += roundFs(36.0f);
        if (confirm) {
            l.confirmY = y;
            y += roundFs(44.0f) + roundFs(6.0f);
        }
    } else {
        y += roundFs(20.0f);
    }
    y += roundFs(10.0f);
    l.diagTitleY = y;
    y += roundFs(18.0f);
    l.diagY = y;
    y += roundFs(18.0f) * 3.0f;
    if (l.fwHint) {
        l.fwY = y;
        y += roundFs(18.0f) * 2.0f;
    }
    l.logBtnY = y;
    y += roundFs(kBtnH);
    y += roundFs(16.0f);
    l.height = y;
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
                          bool bindingConfirm) {
    return layout(snapshot, bindingConfirm).height;
}

void drawWebAccessCard(const CardDrawArgs& args) {
    eui::Ui& ui = args.ui;
    const std::string& id = args.id;
    const float w = args.w;
    const core::theme::Tokens& t = args.t;
    const SettingsCardsIn& in = args.in;
    const SettingsCardActions& a = args.a;
    const core::WebAccessSnapshot& s = in.webAccess;
    const Layout l = layout(s, in.webBindingConfirm);
    const auto compTheme = core::theme::componentTokensWeChat(t);
    const float innerW = std::max(0.0f, w - 2.0f * kPadX);
    const float rightX = kPadX + innerW;

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
                 "网页访问", 19.0f, roundFs(22.0f), t.colors.textPrimary,
                 false, 600);
        textLine(ui, id + ".hint", kPadX, roundFs(l.hintY), innerW,
                 "开启后，同一局域网内的设备可访问本机终端。", 12.0f,
                 roundFs(16.0f), t.colors.textTertiary);

        // 开关行：label 15/500 + 右侧 pill 开关（开关只看服务真值，启动失败自动回弹）
        textLine(ui, id + ".switch.label", kPadX,
                 roundFs(l.switchY) + (roundFs(44.0f) - roundFs(20.0f)) * 0.5f,
                 innerW - kSwitchW - 12.0f,
                 in.webStarting ? "正在开启…" : "启用网页访问", 15.0f,
                 roundFs(20.0f), t.colors.textPrimary, false, 500);
        ui_kit::wswitch(ui, id + ".sw", s.running, rightX - kSwitchW,
                        roundFs(l.switchY) +
                            (roundFs(44.0f) - kSwitchH) * 0.5f,
                        kSwitchW, kSwitchH, t,
                        [f = a.applyWebAccessEnabled](bool v) { f(v); });

        // 错误行：服务启动失败（端口占用 / 无权限）透出人话原因
        if (l.errorShown) {
            textLine(ui, id + ".error", kPadX, roundFs(l.errorY), innerW,
                     s.error.empty() ? "服务启动失败，请查看日志"
                                     : s.error,
                     12.0f, roundFs(18.0f), t.colors.errorText);
        }

        // 端口行：运行中只读（静态文本），停止时可编辑 + 应用
        const std::string portText =
            in.webPortDraft.empty()
                ? (s.port > 0 ? std::to_string(s.port)
                              : (s.configuredPort > 0
                                     ? std::to_string(s.configuredPort)
                                     : std::string("7878")))
                : in.webPortDraft;
        ui_kit::formRow(
            ui, id + ".port", "端口", kPadX, roundFs(l.portY), innerW,
            roundFs(44.0f),
            [&](eui::Ui& inner, float cx, float cy, float cw, float ch) {
                const float inputW = 96.0f;
                const float applyW = 64.0f;
                const float gap = 8.0f;
                const float ctlH = 30.0f;
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
                        .radius(t.sizes.radiusSmall)
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

        // 访问地址
        textLine(ui, id + ".addr.title", kPadX, roundFs(l.addrTitleY), innerW,
                 "访问地址", 13.0f, roundFs(18.0f), t.colors.textSecondary,
                 false, 600);
        if (s.running) {
            int shown = 0;
            for (const auto& addr : s.addresses) {
                if (shown >= l.addrRows) break;
                const std::string url = webUrl(addr, s.port > 0 ? s.port
                                                               : s.configuredPort);
                const float rowY = roundFs(l.addrY) +
                                   static_cast<float>(shown) * roundFs(30.0f);
                const std::string rowId = id + ".addr." + std::to_string(shown);
                textLine(ui, rowId + ".text", kPadX, rowY, innerW - 96.0f,
                         url, 13.0f, roundFs(20.0f), t.colors.textPrimary);
                if (isLoopback(addr)) {
                    textLine(ui, rowId + ".chip", rightX - 44.0f, rowY, 44.0f,
                             "本机", 12.0f, roundFs(20.0f),
                             t.colors.textTertiary);
                }
                const float copyX = rightX - 88.0f;
                components::button(ui, rowId + ".copy")
                    .position(copyX, rowY)
                    .size(40.0f, 24.0f)
                    .text("复制")
                    .fontSize(fs(12.0f))
                    .theme(compTheme, /*primary=*/false)
                    .radius(t.sizes.radiusSmall)
                    .onClick([f = a.copyWebAddress, url] { f(url); })
                    .build();
                ++shown;
            }
            if (!l.hasLan) {
                textLine(ui, id + ".lan.warn", kPadX, roundFs(l.lanWarnY),
                         innerW, "未检测到局域网连接，手机暂无法访问", 12.0f,
                         roundFs(18.0f), t.colors.warningText);
            }
        } else {
            textLine(ui, id + ".addr.empty", kPadX, roundFs(l.addrY), innerW,
                     "服务未开启", 13.0f, roundFs(24.0f),
                     t.colors.textTertiary);
        }

        // 验证器绑定
        textLine(ui, id + ".bind.title", kPadX, roundFs(l.bindTitleY), innerW,
                 "验证器绑定", 13.0f, roundFs(18.0f), t.colors.textSecondary,
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
            const float colX = kPadX + roundFs(kQrBox) + 20.0f;
            const float colW = std::max(0.0f, rightX - colX);
            textLine(ui, id + ".qr.countdown", colX, roundFs(l.qrY), colW,
                     s.tokenRemainingSec > 0
                         ? "剩余 " + formatCountdown(s.tokenRemainingSec)
                         : std::string("已过期"),
                     13.0f, roundFs(20.0f),
                     s.tokenRemainingSec > 0 ? t.colors.textPrimary
                                             : t.colors.errorText);
            components::button(ui, id + ".qr.refresh")
                .position(colX, roundFs(l.qrY) + roundFs(28.0f))
                .size(64.0f, 28.0f)
                .text("刷新")
                .fontSize(fs(13.0f))
                .theme(compTheme, /*primary=*/s.tokenRemainingSec <= 0)
                .radius(t.sizes.radiusSmall)
                .onClick([f = a.refreshWebToken] { f(); })
                .build();
            textLine(ui, id + ".qr.desc", colX,
                     roundFs(l.qrY) + roundFs(64.0f), colW,
                     "手机扫码后按页面提示操作", 12.0f, roundFs(32.0f),
                     t.colors.textTertiary, true);
        } else if (l.bound) {
            textLine(ui, id + ".bind.state", kPadX, roundFs(l.boundY), innerW,
                     "验证器已绑定", 15.0f, roundFs(22.0f),
                     t.colors.textPrimary, false, 600);
            textLine(ui, id + ".bind.time", kPadX,
                     roundFs(l.boundY) + roundFs(22.0f), innerW,
                     "绑定时间：" + formatUnixTime(s.bindTime), 13.0f,
                     roundFs(22.0f), t.colors.textSecondary);
            textLine(ui, id + ".bind.last", kPadX,
                     roundFs(l.boundY) + roundFs(44.0f), innerW,
                     "最近登录：" + formatUnixTime(s.lastLogin), 13.0f,
                     roundFs(22.0f), t.colors.textSecondary);
            // 删除绑定（危险动作：红底白字 + 行内确认）
            const float delW = 96.0f;
            ui.rect(id + ".unbind")
                .position(kPadX, roundFs(l.delY))
                .size(delW, roundFs(36.0f))
                .radius(t.sizes.radiusSmall)
                .color(t.colors.error)
                .onClick([f = a.requestRevokeBinding] { f(); });
            textLine(ui, id + ".unbind.text", kPadX, roundFs(l.delY), delW,
                     "删除绑定", 13.0f, roundFs(36.0f),
                     eui::Color{1.0f, 1.0f, 1.0f, 1.0f}, false, 500);
            if (in.webBindingConfirm) {
                textLine(ui, id + ".confirm.text", kPadX,
                         roundFs(l.confirmY), innerW,
                         "确认删除绑定？所有网页登录会立即失效。", 12.0f,
                         roundFs(18.0f), t.colors.errorText);
                components::button(ui, id + ".confirm.yes")
                    .position(kPadX, roundFs(l.confirmY) + roundFs(20.0f))
                    .size(88.0f, 24.0f)
                    .text("确认删除")
                    .fontSize(fs(12.0f))
                    .theme(compTheme, /*primary=*/true)
                    .radius(t.sizes.radiusSmall)
                    .onClick([f = a.confirmRevokeBinding] { f(); })
                    .build();
                components::button(ui, id + ".confirm.no")
                    .position(kPadX + 96.0f,
                              roundFs(l.confirmY) + roundFs(20.0f))
                    .size(56.0f, 24.0f)
                    .text("取消")
                    .fontSize(fs(12.0f))
                    .theme(compTheme, /*primary=*/false)
                    .radius(t.sizes.radiusSmall)
                    .onClick([f = a.cancelRevokeBinding] { f(); })
                    .build();
            }
        } else {
            textLine(ui, id + ".bind.empty", kPadX,
                     roundFs(l.bindTitleY) + roundFs(18.0f), innerW,
                     "开启后在此显示二维码", 13.0f, roundFs(20.0f),
                     t.colors.textTertiary);
        }

        // 自诊区：监听端口 / 在线连接数 / 最近登录 / Windows 防火墙提示 / 日志目录
        textLine(ui, id + ".diag.title", kPadX, roundFs(l.diagTitleY), innerW,
                 "自诊信息", 13.0f, roundFs(18.0f), t.colors.textSecondary,
                 false, 600);
        const float d1 = roundFs(l.diagY);
        const float lh = roundFs(18.0f);
        textLine(ui, id + ".diag.port", kPadX, d1, innerW,
                 s.running ? "监听端口：127.0.0.1:" + std::to_string(s.port)
                           : std::string("监听端口：未监听"),
                 12.0f, lh, t.colors.textSecondary);
        textLine(ui, id + ".diag.online", kPadX, d1 + lh, innerW,
                 "在线连接数：" + std::to_string(s.onlineCount), 12.0f, lh,
                 t.colors.textSecondary);
        textLine(ui, id + ".diag.last", kPadX, d1 + lh * 2.0f, innerW,
                 "最近登录：" + formatUnixTime(s.lastLogin), 12.0f, lh,
                 t.colors.textSecondary);
        if (l.fwHint) {
            textLine(ui, id + ".diag.firewall", kPadX, roundFs(l.fwY), innerW,
                     "手机连不上？若为首次启动，请在 Windows 防火墙中允许 "
                     "DevDesk 访问专用网络。",
                     12.0f, roundFs(18.0f), t.colors.warningText, true);
        }
        components::button(ui, id + ".diag.logdir")
            .position(kPadX, roundFs(l.logBtnY))
            .size(120.0f, roundFs(kBtnH))
            .text("打开日志目录")
            .fontSize(fs(12.0f))
            .theme(compTheme, /*primary=*/false)
            .radius(t.sizes.radiusSmall)
            .onClick([f = a.openWebLogDir] { f(); })
            .build();
    }).build();
}

} // namespace module::settings
