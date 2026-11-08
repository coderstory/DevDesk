// Profile 列表 UI：header / 引导条 / 左栏 Profile 列表的绘制。
#include "claude_ui_list.h"

#include "claude_ui_logic.h"
#include "core/component_theme.h"
#include "provider_config.h"
#include "settings_sync.h"

#include <components/button.h>
#include <components/contextmenu.h>
#include <ui_kit/dialog_form.h>
#include <ui_kit/dialog_shell.h>
#include <ui_kit/materials.h>
#include <components/scrollview.h>
#include <components/tooltip.h>
#include <components/virtuallist.h>

#include <core/logging.h>
#include <core/router.h>
#include <core/ui_geometry.h>

#include <algorithm>
#include <cstdint>
#include <functional>

namespace module::claude {

namespace {

constexpr float kListTopY = 56.0f;             // 列表起点 y
constexpr float kListBottomGap = 12.0f;        // 列表底 → 面板底内距
constexpr float kListMinH = 96.0f;             // 列表视口保底高
// 列表头新增按钮（2026-10-31 反馈：新建入口从底部移到列表头右侧）
constexpr float kHeadBtnSize = 28.0f;

// 头像圆圈色按 profile id 稳定派生（FNV-1a 取色板下标）：compose 每帧跑真随机会闪烁。色板 9 个低饱和暖色。
constexpr size_t kAvatarPaletteSize = 9;
const eui::Color kAvatarPalette[kAvatarPaletteSize] = {
    eui::Color("#D97757"),  // clay（与主题主色同族）
    eui::Color("#C2846B"),  // 陶土
    eui::Color("#B08D57"),  // 赭石
    eui::Color("#A8977A"),  // 沙棕
    eui::Color("#8F9779"),  // 灰绿
    eui::Color("#7E9B8B"),  // 桉叶
    eui::Color("#7A93A8"),  // 雾蓝
    eui::Color("#9C8AA5"),  // 藕紫
    eui::Color("#B37E85"),  // 豆沙
};

// FNV-1a 64：无依赖、跨帧跨启动一致；std::hash 整型实现未定义，不赌它
std::uint64_t avatarHash(std::uint64_t v) {
    std::uint64_t hash = 14695981039346656037ULL;  // FNV offset basis
    for (int i = 0; i < 8; ++i) {
        hash ^= static_cast<std::uint8_t>(v >> (i * 8));
        hash *= 1099511628211ULL;                  // FNV prime
    }
    return hash;
}

// 由基色 + 主题派生头像 {fill, ring}；字形色随 ring（圆内对比恒定）
struct AvatarColors {
    eui::Color fill;
    eui::Color ring;
};
AvatarColors avatarColorsFor(const eui::Color& base, bool isDark) {
    if (isDark) {
        return {eui::mixColor(base, eui::Color(0.0f, 0.0f, 0.0f), 0.55f),
                eui::mixColor(base, eui::Color(1.0f, 1.0f, 1.0f), 0.45f)};
    }
    return {eui::mixColor(base, eui::Color(1.0f, 1.0f, 1.0f), 0.72f),
            eui::mixColor(base, eui::Color(0.0f, 0.0f, 0.0f), 0.35f)};
}

// 头像首字：取名字里第一个「字体一定覆盖得到」的字符——ASCII 字母数字或常用汉字。
//
// 为什么不用「取第一个 UTF-8 码点」：EUI 遇到字体里没有的字形时走
// ensureGlyph 失败 → 跳过该 glyph（text_layout.cpp:237）→ 该行无 ink →
// **整段文字不渲染**。名字以 emoji/生僻符号开头时（真实用户的 profile 名常是
// "🚀 生产" 这类），头像圆就白着——2026-11-01 用户反馈「圆圈里的文字丢了」。
// 这里显式跳过 emoji（4 字节扩展平面）、ZWJ、变体选择符、符号区、全角符号，
// 取不到任何安全字符时由调用方落 fa-user 图标兜底（图标字体必有字形）。
std::string avatarInitial(const std::string& name) {
    size_t i = 0;
    while (i < name.size()) {
        const auto b0 = static_cast<unsigned char>(name[i]);
        size_t len = 0;
        std::uint32_t cp = 0;
        if (b0 < 0x80) {
            len = 1;
            cp = b0;
        } else if ((b0 & 0xE0u) == 0xC0u) {
            len = 2;
            cp = b0 & 0x1Fu;
        } else if ((b0 & 0xF0u) == 0xE0u) {
            len = 3;
            cp = b0 & 0x0Fu;
        } else if ((b0 & 0xF8u) == 0xF0u) {
            len = 4;
            cp = b0 & 0x07u;
        } else {
            ++i;  // 非法续字节打头：跳过
            continue;
        }
        if (i + len > name.size()) break;
        bool valid = true;
        for (size_t k = 1; k < len; ++k) {
            const auto b = static_cast<unsigned char>(name[i + k]);
            if ((b & 0xC0u) != 0x80u) {
                valid = false;
                break;
            }
            cp = (cp << 6) | (b & 0x3Fu);
        }
        if (!valid) {
            ++i;
            continue;
        }
        i += len;
        if (len == 4) continue;                      // emoji / 扩展平面
        if (cp == 0x200Du || cp == 0xFE0Fu || cp == 0xFE0Eu) continue;  // ZWJ / 变体选择符
        if (cp >= 0x2600u && cp <= 0x27BFu) continue;  // 杂项符号 / 带框数字
        if (cp >= 0x2190u && cp <= 0x21FFu) continue;  // 箭头
        if (cp >= 0x3000u && cp <= 0x303Fu) continue;  // CJK 标点
        if (cp >= 0xFF00u && cp <= 0xFFEFu) continue;  // 全角符号
        if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') ||
            (cp >= '0' && cp <= '9') || (cp >= 0x4E00u && cp <= 0x9FFFu)) {
            std::string ch = name.substr(i - len, len);
            if (cp >= 'a' && cp <= 'z') {
                ch[0] = static_cast<char>(cp - 'a' + 'A');  // 拉丁首字母大写
            }
            return ch;
        }
    }
    return std::string();
}

// baseUrl → 主机名（去协议与路径）；无 baseUrl 落 model；都空 → 空串。
std::string profileSubtitle(const Profile& p) {
    std::string host = p.baseUrl;
    const auto scheme = host.find("://");
    if (scheme != std::string::npos) host = host.substr(scheme + 3);
    const auto slash = host.find('/');
    if (slash != std::string::npos) host = host.substr(0, slash);
    if (host.empty()) host = p.model;
    return host;
}

// Profile 卡片（点击选中，选中/激活高亮）。st 为引用：点击回调写回选中态，须指向存活到下一帧的模块状态成员
void drawProfileCard(eui::Ui& ui,
                     const std::string& id,
                     float y,
                     float width,
                     const core::theme::Tokens& t,
                     const Profile& p,
                     ProfileListState& st,
                     int64_t selectedId,
                     bool isActive) {
    // 2026-11-01 item 重设计：行高 56→60、头像 40→32、名称与副信息两行、
    // 右侧「使用中」标记填掉原先的大片空白（旧版行右侧 60% 是空的）。
    // bug174：标记改悬浮半透明徽标，不再占名字布局位（见函数尾 showActiveChip）。
    constexpr float kRowH = 60.0f;
    constexpr float kAvatar = 32.0f;
    constexpr float kPadX = 12.0f;
    constexpr float kTextX = kPadX + kAvatar + 12.0f;  // 56
    constexpr float kActiveChipW = 52.0f;
    constexpr float kRightPad = 12.0f;
    const float h = kRowH;
    const bool selected = (p.id == selectedId);
    // S2.5 选中态：primarySoftBg 底 + 左 3px primary 竖线（替代旧绿圈白勾）
    const eui::Color bgNormal = selected
        ? t.colors.primarySoftBg
        : ui_kit::itemSurface(t);  // need219 §5.2-1：卡面玻璃 itemSurface（原不透明 surface）
    // 选中字色用 primaryText（浅色档 #157E4E on 白 4.99:1）；primary 本身仅
    // 2.96:1，14px 小字不达 WCAG AA（与审计 F3.4 同一裁决口径）。
    const eui::Color nameColor = selected
        ? t.colors.primaryText
        : t.colors.textPrimary;

    ui.rect(id + ".bg")
        .position(0.0f, y)
        .size(width, h)
        .states(bgNormal, t.colors.hover,
                t.colors.primarySoftBg)
        // 行 hover/选中颜色过渡
        .transition(core::Transition::make(0.1f, core::Ease::OutQuad))
        .animate(core::AnimProperty::Color)
        .radius(t.sizes.radiusSmall)
        .onClick([sp = &st, pid = p.id, pname = p.name, sel = selectedId] {
            if (pid == sel) return;  // 已选卡（含跟随激活态）无操作
            if (sp->switchBypassConfirm_) {       // 测试缝直走
                sp->selectedId_ = pid;
                return;
            }
            sp->switchTargetId_ = pid;
            sp->switchTargetName_ = pname;
            sp->switchConfirmOpen_ = true;
        })
        .build();

    // S2.5 左 3px 竖线（仅选中时）
    if (selected) {
        ui.rect(id + ".accent")
            .position(0.0f, y)
            .size(3.0f, h)
            .color(t.colors.primary)
            .build();
    }

    // 头像：按 profile id 稳定派生的圆 + 1.5px 同色系包边；垂直居中由 (h - 头像径) 推导
    const float avatarY = y + (h - kAvatar) * 0.5f;
    const AvatarColors avatar = avatarColorsFor(
        kAvatarPalette[avatarHash(static_cast<std::uint64_t>(p.id)) %
                       kAvatarPaletteSize],
        t.isDark);
    ui.rect(id + ".avatar")
        .position(kPadX, avatarY)
        .size(kAvatar, kAvatar)
        .color(avatar.fill)
        .radius(kAvatar * 0.5f)
        .border(1.5f, avatar.ring)
        .build();

    // 头像内容：首字（字体覆盖不到的字符已在 avatarInitial 里剔除）；
    // 取不到安全字符 → fa-user 图标兜底（图标字体必有字形，圆圈永不空白）
    const std::string initial = avatarInitial(p.name);
    if (!initial.empty()) {
        ui.text(id + ".avatar.text")
            .position(kPadX, avatarY)
            .size(kAvatar, kAvatar)
            .text(initial)
            .fontSize(core::theme::scaled(14.0f))
            .lineHeight(kAvatar)
            .horizontalAlign(eui::HorizontalAlign::Center)
            // 文本盒 == 圆底高，须显式垂直居中，否则 ink 从行框顶沿起排
            .verticalAlign(eui::VerticalAlign::Center)
            .color(avatar.ring)
            .fontWeight(600)
            .build();
    } else {
        ui.text(id + ".avatar.icon")
            .position(kPadX, avatarY)
            .size(kAvatar, kAvatar)
            .icon(eui::utf8(0xF007))  // fa-user
            .fontSize(core::theme::scaled(13.0f))
            .lineHeight(kAvatar)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(avatar.ring)
            .build();
    }

    // 右侧「使用中」标记（仅激活项）：bug174 改悬浮半透明徽标——不再从名字宽
    // 里扣布局位（旧版扣 60px 致长名截断），名字恢复全宽；徽标绘制见函数尾。
    // need236：徽标从名字行（10..28）下移到副标题行带（30..46），名字行零遮挡；
    // 副标题在激活卡上右缩让出徽标宽。
    const bool showActiveChip = isActive;
    const float nameMaxW = std::max(40.0f, width - kTextX - kRightPad);
    const float subMaxW = showActiveChip
        ? std::max(40.0f, nameMaxW - (kActiveChipW + 8.0f))
        : nameMaxW;

    // 名称：单行截断让位右缘
    {
        ui.text(id + ".name")
            .position(kTextX, y + 10.0f)
            .size(nameMaxW, core::theme::scaled(18.0f))
            .maxWidth(nameMaxW)
            .wrap(false)
            .overflow(core::TextOverflow::Ellipsis)
            .text(p.name)
            .fontSize(core::theme::scaled(14.0f))
            .lineHeight(core::theme::scaled(18.0f))
            .fontWeight(600)
            .color(nameColor)
            .build();
    }

    // 副信息（主机名 / 模型）：第二行灰字，与名称左对齐
    {
        const std::string sub = profileSubtitle(p);
        if (!sub.empty()) {
            ui.text(id + ".subtitle")
                .position(kTextX, y + 30.0f)
                .size(subMaxW, core::theme::scaled(16.0f))
                .maxWidth(subMaxW)
                .wrap(false)
                .overflow(core::TextOverflow::Ellipsis)
                .text(sub)
                .fontSize(core::theme::scaled(11.0f))
                .lineHeight(core::theme::scaled(16.0f))
                .color(t.colors.textTertiary)
                .build();
        }
    }

    if (showActiveChip) {
        // need236 落位：徽标锚在副标题行带内垂直居中（y+30 起、行高 scaled(16)），
        // 徽标高 20 → 100% 档占 28..48；名字行 10..28 零重叠、卡底 60 有余量。
        // 半透明 primary 底 + 角落锚定保对比。
        // 放大档下界钳制：kRowH 是硬编码 60 不随 fontScale 缩放（本条目禁改骨架
        // 常量），而名字行高 scaled(18) 会随档增长——fs=1.3 时名字行伸到 33.4，
        // 与「副标题带居中」算出的徽标顶 30.4 产生 3px 重叠。取「名字行底」作
        // 徽标顶的下界（再加 2px 呼吸），保证任意字号档徽标都不压名字行；卡底
        // 越界风险随之消失（顶更晚了但徽标仅 20 高，fs≥1.15 时已贴卡底内沿）。
        const float nameRowBottom =
            y + 10.0f + core::theme::scaled(18.0f);
        const float chipX = width - kRightPad - kActiveChipW;
        const float chipY = std::max(
            y + 30.0f + (core::theme::scaled(16.0f) - 20.0f) * 0.5f,
            nameRowBottom + 2.0f);
        eui::Color chipBg = t.colors.primary;
        chipBg.a = 0.78f;
        ui.rect(id + ".active.chip")
            .position(chipX, chipY)
            .size(kActiveChipW, 20.0f)
            .radius(t.sizes.radiusPill)
            .color(chipBg)
            .border(1.0f, t.colors.primary)
            .build();
        ui.text(id + ".active.text")
            .position(chipX, chipY)
            .size(kActiveChipW, core::theme::scaled(20.0f))
            .text("使用中")
            .fontSize(core::theme::scaled(11.0f))
            .lineHeight(core::theme::scaled(20.0f))
            .horizontalAlign(eui::HorizontalAlign::Center)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.onPrimary)
            .build();
    }

    // S3.4 名称 tooltip。bug177 同族修正：不传 anchor 即默认 (0,0)（TooltipBuilder
    // 无 auto-position），气泡钉死卡片左上。锚取名称文本盒中心（与 .name 同一
    // stack 作用域、同一坐标空间）：x = kTextX + nameMaxW/2，y = 名称行垂直中
    // （y+10 + 18/2）；bounds=卡片盒，气泡钳在卡内、指向名称行。
    components::tooltip(ui, id + ".name.tooltip")
        .theme(core::theme::componentTokensWeChat(t))
        .source(id + ".name")
        .value(p.name)
        .anchor(kTextX + nameMaxW * 0.5f, y + 19.0f)
        .bounds(width, h)
        .minWidth(200.0f)
        .maxWidth(320.0f)
        .build();

    }

} // namespace

void drawClaudeHeader(eui::Ui& ui, float contentW, float headerH,
                      const core::theme::Tokens& t,
                      bool proxyRunning, const std::string& portStr,
                      const std::function<void()>& onToggleProxy) {
    // header 行 gap=0，两段宽必须相加等于 contentW，防字面量两处改一处忘
    constexpr float kHeaderActionsW = 360.0f;

    // §5 帮助钮几何（UAT6 问号钮重设计）：24px 正圆（对齐 md.help.icon 24px 档）。
    // Q 项：tooltip 锚改为跟随 source 元素实际位置，不再硬算 helpBtnX。
    constexpr float kHelpBtnD = 24.0f;

    // UAT5 页头内部收紧（toptrim 批点名跟进）：内部 50→40 与共享 kHeaderH 叠满。
    // UAT6 副题横扫：标题行下副题 14 行删除，标题行独占页头（同 todo/md/home 档），
    // 页头行高沿用共享 kHeaderH=40 不动（行内状态组/按钮 40 高）。
    ui.row("claude.header")
        .size(contentW, headerH)
        .alignItems(eui::Align::CENTER)
        .content([&] {
            ui.column("claude.title.col")
                .size(contentW - kHeaderActionsW, headerH)
                .gap(2.0f)
                .content([&] {
                    // UAT2 #10：title row 只留标题——状态组（dot+text+help）右对齐
                    // 挪到 header 右侧「claude.actions」，text 盒不再固定 200px 宽
                    ui.row("claude.title.row")
                        .size(contentW - kHeaderActionsW, 24.0f)
                        .alignItems(eui::Align::CENTER)
                        .content([&] {
                            ui.text("claude.h1")
                                .size(220.0f, core::theme::scaled(24.0f))
                                .text("Claude 配置")
                                .fontSize(t.sizes.fontSizeTitle)
                                .lineHeight(core::theme::scaled(24.0f))
                                .fontWeight(600)
                                .color(t.colors.textPrimary)
                                .build();
                        })
                        .build();
                })
                .build();

            ui.row("claude.actions")
                .size(kHeaderActionsW, 40.0f)
                .justifyContent(eui::Align::END)
                .alignItems(eui::Align::CENTER)
                .gap(t.sizes.spacingSmall)
                .content([&] {
                    // UAT2 #10：状态组从 title row 挪到 header 右侧右对齐（与操作按钮
                    // 同区）——原 text 盒 200px 固定宽，「已停止」只占几字致右侧大片
                    // 空；改 wrapContent 按内容收缩（EUI 文本走真实字形测量，
                    // 「已停止」≈39px、「运行中 :port」动态），上界无需再设。
                    // §5 灰圆点「已停止」/ 绿圆点「运行中」（wechat 令牌，isProxyRunning 内存态）
                    ui.row("claude.status.row")
                        // 行盒宽随内容收缩（dot+text），高取组内最高 18
                        .width(core::SizeValue::wrapContent())
                        .height(18.0f)
                        .gap(12.0f)
                        .alignItems(eui::Align::CENTER)
                        .margin(0.0f, 0.0f, 8.0f, 0.0f)
                        .content([&] {
                            const eui::Color dotColor = proxyRunning
                                ? t.colors.primary
                                : t.colors.textTertiary;
                            const std::string statusText = proxyRunning
                                ? std::string("运行中 ") + portStr
                                : std::string("已停止");
                            ui.rect("claude.status.dot")
                                .size(10.0f, 10.0f)
                                .color(dotColor)
                                .radius(5.0f)
                                .build();
                            ui.text("claude.status.text")
                                .width(core::SizeValue::wrapContent())
                                .height(18.0f)
                                .text(statusText)
                                .fontSize(t.sizes.fontSizeSmall)
                                .lineHeight(core::theme::scaled(18.0f))
                                .color(proxyRunning ? t.colors.primary
                                                    : t.colors.textTertiary)
                                .fontWeight(600)
                                .build();
                        })
                        .build();

                    // §5 帮助图标钮（UAT6 问号钮重设计，尺寸对齐 md.help.icon 的
                    // 24px 档）：正圆、icon 居中、hover 三态（透明底→浅灰→按压灰），
                    // 照已过 UAT 的圆钮先例（正圆 + icon 居中 + 三态）。
                    // 从状态组 wrap 行挪出为独立钮——右缘可静态推算（tooltip 锚点
                    // 需要 wrap 行给不了）；长文案 hover 经 EUI tooltip 弹回（下方）。
                    ui.stack("claude.title.help")
                        .size(kHelpBtnD, kHelpBtnD)
                        .visualStateFrom("claude.title.help.bg", 0.965f)
                        .content([&] {
                            ui.rect("claude.title.help.bg")
                                .size(kHelpBtnD, kHelpBtnD)
                                .states(core::Color{0.0f, 0.0f, 0.0f, 0.0f},
                                        t.colors.primarySoftBg,
                                        t.colors.hover)
                                .radius(kHelpBtnD * 0.5f)
                                .build();
                            ui.text("claude.title.help.icon")
                                .size(kHelpBtnD, kHelpBtnD)
                                .icon(0xF059)  // fa-circle-question
                                .fontSize(core::theme::scaled(14.0f))
                                .lineHeight(kHelpBtnD)
                                .horizontalAlign(eui::HorizontalAlign::Center)
                                .verticalAlign(eui::VerticalAlign::Center)
                                .color(t.colors.textTertiary)
                                .build();
                        })
                        .build();

                    // §5「启动代理」按钮（S3.4：停止态 Outline / 启动态 Filled）
                    const bool stopped = !proxyRunning;
                    components::button(ui, "claude.btn.proxy")
                        .size(110.0f, 40.0f)
                        .text(proxyRunning ? "停止代理" : "启动代理")
                        .icon(proxyRunning ? 0xF04C : 0xF04B)  // fa-pause / fa-play
                        .fontSize(t.sizes.fontSizeSmall)
                        .colors(stopped ? t.colors.primary
                                        : t.colors.surface,
                                stopped ? t.colors.primaryHover
                                        : t.colors.primarySoftBg,
                                stopped ? t.colors.primaryPressed
                                        : t.colors.primarySoftBg)
                        .textColor(stopped ? t.colors.onPrimary
                                           : t.colors.primary)
                        .iconColor(stopped ? t.colors.onPrimary
                                           : t.colors.primary)
                        .radius(t.sizes.radius)
                        .border(stopped ? 0.0f : 1.0f, t.colors.primary)
                        .onClick([onToggleProxy] { onToggleProxy(); })
                        .build();
                })
                .build();
        })
        .build();

    // 帮助钮 tooltip 绑定（现成 hint 机制）：UAT6 副题横扫时收起的一句话摘要
    // 在 hover 弹回。bug177 修正：原注释声称「跟随 source 元素实际位置」的
    // auto-position 在 TooltipBuilder（EUI-NEO/components/tooltip.h:32-151）里
    // 并不存在——不传 anchor 即默认 (0,0)，气泡恒钉容器左上 (12,44)。实际锚定
    // 用右缘静态推算（status 行 wrapContent 宽不可静态知，但 justify END 下
    // 钮位从右缘数恒定）：actions 行（kHeaderActionsW=360，gap 4）右起
    // 启动代理 110 → help 24，故 help 中心 = 行内 x 234；行起点 x =
    // contentW - 360（header 行 gap=0、title 列恰占余宽）。bounds=页头行使
    // 气泡钉 header 正下（topLimit 44）。
    // H17：tooltip 画在 header row 之外会被列布局当子项排入、占用 40px+16gap，
    // 裹进 ignoreLayout 栈防它参与 root column 流式布局（bug210 后 ignoreLayout
    // 语义下本包裹仍必要：overlay 定位原点=root 内容盒）。
    ui.stack("__tooltip_ignore")
        .ignoreLayout()
        .content([&] {
            components::tooltip(ui, "claude.title.help.tooltip")
                .theme(core::theme::componentTokensWeChat(t))
                .source("claude.title.help.bg")
                .value(proxyRunning ? "本地代理运行中，管理多套 Claude API 配置"
                                     : "管理多套 Claude API 配置")
                .anchor(contentW - kHeaderActionsW + 234.0f, headerH * 0.5f)
                .bounds(contentW, headerH)
                .minWidth(320.0f)
                .maxWidth(320.0f)
                .build();
        })
        .build();
}

void drawClaudeGuideBar(eui::Ui& ui, float contentW,
                        const core::theme::Tokens& t,
                        ProfileListState& st) {
    // 引导条：代理未运行时的接管语义提示（图标字体不能与正文混排，图标/文字拆两元素）
    // S3.4 右侧添加关闭按钮
    ui.row("claude.guide.wrap")
        .size(contentW, kGuideBarH)
        .gap(6.0f)
        .alignItems(eui::Align::CENTER)
        .content([&] {
            ui.text("claude.guide.icon")
                .size(14.0f, core::theme::scaled(18.0f))
                .icon(0xF05A)  // fa-circle-info
                .fontSize(t.sizes.fontSizeSmall)
                .lineHeight(core::theme::scaled(18.0f))
                .color(t.colors.textSecondary)
                .build();
            ui.text("claude.guide.takeover")
                .size(contentW - 44.0f, core::theme::scaled(18.0f))
                .text("启动代理将接管 Claude Code 配置（" +
                      settings_sync::defaultPaths().settingsFile.string() +
                      " 指向本地代理），停止代理时自动还原")
                .fontSize(t.sizes.fontSizeSmall)
                .lineHeight(core::theme::scaled(18.0f))
                .color(t.colors.textSecondary)
                .build();
            // 关闭按钮（×）
            ui.stack("claude.guide.close")
                .size(18.0f, 18.0f)
                .visualStateFrom("claude.guide.close.bg", 0.965f)
                .content([&] {
                    ui.rect("claude.guide.close.bg")
                        .size(18.0f, 18.0f)
                        .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
                        .radius(t.sizes.radiusSmall)
                        .onClick([sp = &st] { sp->guideDismissed_ = true; })
                        .build();
                    ui.text("claude.guide.close.icon")
                        .size(core::theme::scaled(18.0f), core::theme::scaled(18.0f))
                        .icon(0xF00D)  // fa-times
                        .fontSize(core::theme::scaled(12.0f))
                        .lineHeight(core::theme::scaled(18.0f))
                        .horizontalAlign(eui::HorizontalAlign::Center)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(t.colors.textTertiary)
                        .build();
                })
                .build();
        })
        .build();
}

void drawProfileListPanel(eui::Ui& ui, float leftW, float bodyH,
                          const core::theme::Tokens& t,
                          const components::theme::ThemeColorTokens& themeTokens,
                          ProfileListState& st,
                          const std::vector<Profile>& profiles,
                          const std::string& profilesError,
                          const ProfileListActions& actions) {
    ui.column("claude.profiles")
        .size(leftW, bodyH)
        .gap(0.0f)
        .content([&] {
            ui.rect("claude.profiles.bg")
                .size(leftW, bodyH)
                // §4.14 列底浅灰底（wechat.pageBg），白卡浮起；边框一级分割线
                .color(t.colors.background)
                .radius(t.sizes.radiusLarge)
                .border(1.0f, t.colors.borderDefault)
                .build();
        })
        .build();

    ui.stack("claude.profiles.overlay")
        .size(leftW, bodyH)
        .ignoreLayout()  // overlay 不参与 row 流式布局，覆盖在 profiles 列上
        .content([&] {
            ui.text("claude.profiles.title")
                .position(20.0f, 20.0f)
                .size(leftW - 40.0f - kHeadBtnSize, core::theme::scaled(20.0f))
                .text("Profile 列表")
                .fontSize(t.sizes.fontSizeBody)
                .lineHeight(core::theme::scaled(20.0f))
                .fontWeight(600)
                .color(t.colors.textPrimary)
                .build();

            // 列表头新增按钮（唯一新建入口）：与标题同行右对齐，幽灵样式
            components::button(ui, "claude.profiles.new")
                .position(leftW - 20.0f - kHeadBtnSize, 16.0f)
                .size(kHeadBtnSize, kHeadBtnSize)
                .text("+")
                .fontSize(t.sizes.fontSizeBody)
                .theme(themeTokens, false)
                .textColor(t.colors.textSecondary)
                .radius(t.sizes.radius)
                .onClick([onNew = actions.onNewProfile] { onNew(); })
                .build();

            const float cardY0 = kListTopY;
            const float cardGap = 8.0f;  // need219 §4.3 卡与卡纵向间距统一 8px

            // 列表底 = 面板底内距（底部按钮行已取消，空间回收给列表）
            const float listBottom = bodyH - kListBottomGap;

            if (!profilesError.empty()) {
                ui.text("claude.profiles.error")
                    .position(20.0f, cardY0 + 20.0f)
                    .size(leftW - 40.0f, core::theme::scaled(60.0f))
                    .text("Profile 列表加载失败：" + profilesError)
                    .fontSize(core::theme::scaled(12.0f))
                    .lineHeight(core::theme::scaled(20.0f))
                    .wrap(true)  // 长错误原因必须换行读全（P0-C1：单行截断丢自查线索）
                    .color(t.colors.errorText)
                    .build();
            } else if (profiles.empty()) {
                ui.text("claude.profiles.empty")
                    .position(20.0f, cardY0 + 20.0f)
                    .size(leftW - 40.0f, core::theme::scaled(60.0f))
                    .text("暂无 Profile，点右上角 + 新建")
                    .fontSize(core::theme::scaled(12.0f))
                    .lineHeight(core::theme::scaled(20.0f))
                    .color(t.colors.textTertiary)
                    .build();
            } else {
                // virtualList：仅可见行进 slot 窗口重建，overscan 一个视口高；列表恒吃剩余高
                const float rowPitch = 60.0f + cardGap;
                const float listH = std::max(0.0f, listBottom - cardY0);
                // 退化守门：仅 bodyH 极小时列表视口被挤没
                if (listH < kListMinH) {
                    LOG_ERROR("[layout] Profile 列表保底视口被挤没: listH=" +
                              std::to_string(listH) + " < " +
                              std::to_string(kListMinH) + " bodyH=" +
                              std::to_string(bodyH));
                }
                // selectedId_ 契约（claude_ui_state.h）= 0 跟随激活 profile；显式 id
                // 不在列表（已删）同规则回落激活，与右栏详情 shown 取用口径一致。
                // 缺此回落则首帧列表无任何选中高亮，详情有值而列表裸白。
                int64_t effSelectedId = st.selectedId_;
                bool effFound = false;
                for (const auto& p : profiles) {
                    if (p.id == effSelectedId) { effFound = true; break; }
                }
                if (!effFound) {
                    effSelectedId = st.cachedHasActive_
                        ? st.cachedActive_.id : 0;
                }
                components::virtualList(ui, "claude.profiles.scroll")
                    .position(20.0f, cardY0)
                    .size(leftW - 40.0f, listH)
                    .itemCount(static_cast<int64_t>(profiles.size()))
                    .rowHeight(rowPitch)
                    .step(rowPitch)
                    .overscanViewports(1.0f)
                    .theme(themeTokens)
                    .offset(st.profilesScrollOffset_)
                    .onChange([sp = &st](float v) {
                        sp->profilesScrollOffset_ = v;
                    })
                    .row([&](eui::Ui& inner, const std::string& rowId,
                             int64_t index, float contentWidth,
                             float rowHeight) {
                        (void)rowHeight;
                        const auto idx = static_cast<size_t>(index);
                        if (idx >= profiles.size()) return;
                        // 行容器 = 裸 stack：card 组件自带底色会与交互层 .bg 叠成双底色，仅保留 .bg 作唯一底
                        const std::string cardId =
                            "claude.profile." + std::to_string(idx);
                        inner.stack(rowId + ".card")
                            .size(contentWidth, 60.0f)
                            .content([&] {
                                // 右键菜单锚点 = PointerEvent 原始坐标经 menuAnchorForPointer 换算（页面坐标）；st 为引用写回模块状态
                                inner.stack(cardId + ".wrap")
                                    .size(contentWidth, 60.0f)
                                    .onContextMenu(
                                        [sp = &st,
                                         pid = profiles[idx].id](
                                            const eui::PointerEvent& ev,
                                            const eui::Rect&) {
                                            const ::core::Router* router =
                                                ::core::appRouter();
                                            const core::MenuAnchor a =
                                                core::menuAnchorForPointer(
                                                    static_cast<float>(ev.x),
                                                    static_cast<float>(ev.y),
                                                    router
                                                        ? router->pageOriginX()
                                                        : 0.0f,
                                                    0.0f, 1.0f);
                                            sp->ctxProfileId_ = pid;
                                            sp->ctxMenuX_ = a.x;
                                            sp->ctxMenuY_ = a.y;
                                            sp->ctxMenuOpen_ = true;
                                        })
                                    .content([&] {
                                        drawProfileCard(
                                            inner, cardId, 0.0f,
                                            contentWidth, t, profiles[idx],
                                            st, effSelectedId,
                                            st.cachedHasActive_ &&
                                                st.cachedActive_.id ==
                                                    profiles[idx].id);
                                    })
                                    .build();
                            })
                            .build();
                    })
                    .build();
            }
        })
        .build();
}

// 切换 Profile 二次确认弹窗（UAT6）
// 点非选中卡不直接切——先弹确认：取消=关闭不动，确认=走现有切换路径并关窗。
// 点已选卡维持现状无操作（drawProfileCard onClick 已 return 跳过）。
void drawSwitchConfirmDialog(eui::Ui& ui, const eui::Screen& screen,
                              const core::theme::Tokens& t,
                              const components::theme::ThemeColorTokens& themeTokens,
                              ProfileListState& st) {
    // need156：壳改走标准弹窗壳；156 低于壳最小 240 抬到下限（规范 §3.4）。
    constexpr float h = 240.0f;
    const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Compact);
    ui_kit::DialogShellSpec shell;
    shell.id = "claude.dialog.switch";
    shell.width = ui_kit::DialogWidth::Compact;
    shell.height = h;
    shell.open = st.switchConfirmOpen_;
    shell.title = "切换 Profile";
    shell.cancel = {"取消", [sp = &st] { sp->switchConfirmOpen_ = false; }};
    shell.confirm = {"确认切换", [sp = &st] {
        sp->selectedId_ = sp->switchTargetId_;
        sp->switchConfirmOpen_ = false;
    }};
    shell.onOpenChange = [sp = &st](bool v) { sp->switchConfirmOpen_ = v; };

    ui_kit::drawDialogShell(ui, screen, shell, t, themeTokens,
                            [&](const ui_kit::DialogContentArea& area) {
        ui_kit::drawDialogText(
            ui, "claude.dialog.switch.msg", w,
            "切换到\"" + st.switchTargetName_ + "\"？代理配置将立即生效",
            14.0f, t.colors.textSecondary,
            {.y = area.y, .height = 40.0f, .lineHeight = 20.0f,
             .wrap = true});
    });
}


// Profile 右键菜单：动作经 ProfileListActions 委托到模块既有逻辑，本函数不写业务。
// 常驻构建（关闭态由组件内 opacity 收起）；无右键目标时强制置关，防「隐形菜单卡开」。
void drawProfileContextMenu(eui::Ui& ui, const eui::Screen& screen,
                            const components::theme::ThemeColorTokens& themeTokens,
                            ProfileListState& st,
                            const std::vector<Profile>& profiles,
                            const ProfileListActions& actions) {
    if (st.ctxProfileId_ == 0) {
        st.ctxMenuOpen_ = false;
        return;
    }
    const Profile* self = nullptr;
    for (const Profile& p : profiles) {
        if (p.id == st.ctxProfileId_) { self = &p; break; }
    }
    if (self == nullptr) {
        st.ctxMenuOpen_ = false;
        return;
    }
    const std::vector<components::ContextMenuItem> items = {
        {"编辑"}, {"建副本"}, {"复制配置"}, {"删除"}, {"设为当前"}};
    // 回调不捕获 self 指针（profiles 是 compose 局部副本，点击发生在下帧 → 悬垂）：按值拷贝 Profile 与五个闭包
    components::contextMenu(ui, "claude.list.ctx")
        .open(st.ctxMenuOpen_)
        .screen(screen.width, screen.height)
        .position(st.ctxMenuX_, st.ctxMenuY_)
        .autoWidth()
        .items(items)
        .theme(themeTokens)
        .onSelect([&st,
                   onEdit = actions.requestEdit,
                   onDup = actions.requestDuplicate,
                   onCopyCfg = actions.requestCopyConfig,
                   onDel = actions.requestDelete,
                   onSet = actions.requestSetActive,
                   p = *self](int index) {
            st.ctxMenuOpen_ = false;
            switch (index) {
                case 0: onEdit(p); break;
                case 1: onDup(p.id); break;
                case 2: onCopyCfg(p); break;
                case 3: onDel(p.id, p.name); break;
                case 4: onSet(p.id); break;
                default: break;
            }
        })
        .onOpenChange([&st](bool v) { st.ctxMenuOpen_ = v; })
        .build();
}

} // namespace module::claude
