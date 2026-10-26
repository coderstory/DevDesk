// Profile 列表 UI：header / 引导条 / 左栏 Profile 列表与共享配置区块的绘制。
#include "claude_ui_list.h"

#include "claude_ui_logic.h"
#include "core/component_theme.h"
#include "provider_config.h"
#include "settings_sync.h"

#include <components/button.h>
#include <components/contextmenu.h>
#include <ui_kit/dialog_form.h>
#include <components/scrollview.h>
#include <components/tooltip.h>
#include <components/virtuallist.h>

#include <core/logging.h>
#include <core/render/text.h>  // core::TextPrimitive::measureTextWidth
#include <core/router.h>
#include <core/ui_geometry.h>

#include <algorithm>
#include <cstdint>
#include <functional>

namespace module::claude {

namespace {

constexpr float kListTopY = 56.0f;             // 列表起点 y
constexpr float kListBottomGap = 12.0f;        // 列表底 → 底部按钮行顶
constexpr float kListMinH = 96.0f;             // 列表视口保底高
// 底部按钮区（两行：主键独行 + 两导入同行）= 40 + 12 + 40 + 16
constexpr float kBottomBtnRowH = 108.0f;

// 底部按钮行上沿（面板内绝对 y）
float bottomButtonRowY(float bodyH) { return bodyH - kBottomBtnRowH; }

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
            eui::mixColor(base, eui::Color(0.0f, 0.0f, 0.0f), 0.25f)};
}

// 取名字首 UTF-8 码点（单取首字节会把 CJK 截成 ?）；非法 / 截断序列回退 ?
std::string firstUtf8CharOr(const std::string& name) {
    if (name.empty()) return "?";
    const auto b0 = static_cast<unsigned char>(name[0]);
    size_t len = 0;
    if ((b0 & 0x80u) == 0x00u) {
        len = 1;
    } else if ((b0 & 0xE0u) == 0xC0u) {
        len = 2;
    } else if ((b0 & 0xF0u) == 0xE0u) {
        len = 3;
    } else if ((b0 & 0xF8u) == 0xF0u) {
        len = 4;
    } else {
        return "?";  // 0x80-0xBF 续字节打头 / 0xF8+ 超界 → 非法序列
    }
    if (name.size() < len) return "?";  // 序列被截断
    for (size_t i = 1; i < len; ++i) {
        if ((static_cast<unsigned char>(name[i]) & 0xC0u) != 0x80u) {
            return "?";  // 续字节不合法
        }
    }
    return name.substr(0, len);
}

// Profile 卡片（点击选中，选中/激活高亮）。st 为引用：点击回调写回选中态，须指向存活到下一帧的模块状态成员
void drawProfileCard(eui::Ui& ui,
                     const std::string& id,
                     float y,
                     float width,
                     const core::theme::Tokens& t,
                     const Profile& p,
                     ProfileListState& st,
                     int64_t selectedId) {
    const float h = 56.0f;
    const bool selected = (p.id == selectedId);
    // S2.5 选中态：primarySoftBg 底 + 左 3px primary 竖线（替代旧绿圈白勾）
    const eui::Color bgNormal = selected
        ? t.colors.primarySoftBg
        : t.colors.surface;
    const eui::Color nameColor = selected
        ? t.colors.primary
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
    const float avatarSize = 40.0f;
    const float avatarY = y + (h - avatarSize) * 0.5f;
    const AvatarColors avatar = avatarColorsFor(
        kAvatarPalette[avatarHash(static_cast<std::uint64_t>(p.id)) %
                       kAvatarPaletteSize],
        t.isDark);
    ui.rect(id + ".avatar")
        .position(16.0f, avatarY)
        .size(avatarSize, avatarSize)
        .color(avatar.fill)
        .radius(avatarSize * 0.5f)
        .border(1.5f, avatar.ring)
        .build();
    // 头像首字按 UTF-8 码点取，颜色随包边
    const std::string initial = firstUtf8CharOr(p.name);
    ui.text(id + ".avatar.text")
        .position(16.0f, avatarY)
        .size(avatarSize, avatarSize)
        .text(initial)
        .fontSize(core::theme::scaled(16.0f))
        .lineHeight(avatarSize)
        .horizontalAlign(eui::HorizontalAlign::Center)
        // 文本盒 == 圆底高，须显式垂直居中，否则 ink 从行框顶沿起排
        .verticalAlign(eui::VerticalAlign::Center)
        .color(avatar.ring)
        .fontWeight(600)
        .build();

    // 名称：单行截断让位右缘
    {
        const float nameMaxW = std::max(40.0f, width - 12.0f - 64.0f);
        ui.text(id + ".name")
            .position(64.0f, y + ((h - 20.0f) * 0.5f))
            .size(nameMaxW, 20.0f)
            .maxWidth(nameMaxW)
            .wrap(false)
            .overflow(core::TextOverflow::Ellipsis)
            .text(p.name)
            .fontSize(core::theme::scaled(14.0f))
            .lineHeight(core::theme::scaled(20.0f))
            .fontWeight(600)
            .color(nameColor)
            .build();
    }

    // S3.4 名称 tooltip
    components::tooltip(ui, id + ".name.tooltip")
        .theme(core::theme::componentTokensWeChat(t))
        .source(id + ".name")
        .value(p.name)
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
                                .size(220.0f, 24.0f)
                                .text("Claude 切换器")
                                .fontSize(t.sizes.fontSizeTitle)
                                .lineHeight(24.0f)
                                .fontWeight(700)
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
                                .lineHeight(18.0f)
                                .color(proxyRunning ? t.colors.primary
                                                    : t.colors.textTertiary)
                                .fontWeight(500)
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
                        .radius(8.0f)
                        .border(stopped ? 0.0f : 1.0f, t.colors.primary)
                        .onClick([onToggleProxy] { onToggleProxy(); })
                        .build();
                })
                .build();
        })
        .build();

    // 帮助钮 tooltip 绑定（现成 hint 机制）：UAT6 副题横扫时收起的一句话摘要
    // 在 hover 弹回。Q 项：去掉手算锚位 / 页面 bounds，改为跟随 source 元素
    // 当前实际位置（claude.form.env 同款默认锚 auto-position 先例），不再硬编码
    // helpBtnX / headerH 推算——任何按钮位置变动不用改 tooltip。
    // H17：tooltip 画在 header row 之外会被列布局当子项排入、占用 40px+16gap，
    // 裹进 ignoreLayout 栈防它参与 root column 流式布局。
    ui.stack("__tooltip_ignore")
        .ignoreLayout()
        .content([&] {
            components::tooltip(ui, "claude.title.help.tooltip")
                .theme(core::theme::componentTokensWeChat(t))
                .source("claude.title.help.bg")
                .value(proxyRunning ? "本地代理运行中，管理多套 Claude API 配置"
                                     : "管理多套 Claude API 配置")
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
                .size(14.0f, 18.0f)
                .icon(0xF05A)  // fa-circle-info
                .fontSize(t.sizes.fontSizeSmall)
                .lineHeight(18.0f)
                .color(t.colors.textSecondary)
                .build();
            ui.text("claude.guide.takeover")
                .size(contentW - 44.0f, 18.0f)
                .text("启动代理将接管 Claude Code 配置（" +
                      settings_sync::defaultPaths().settingsFile.string() +
                      " 指向本地代理），停止代理时自动还原")
                .fontSize(t.sizes.fontSizeSmall)
                .lineHeight(18.0f)
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
                        .radius(4.0f)
                        .onClick([sp = &st] { sp->guideDismissed_ = true; })
                        .build();
                    ui.text("claude.guide.close.icon")
                        .size(18.0f, 18.0f)
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
                .size(leftW - 40.0f, 20.0f)
                .text("Profile 列表")
                .fontSize(t.sizes.fontSizeBody)
                .lineHeight(20.0f)
                .fontWeight(600)
                .color(t.colors.textPrimary)
                .build();

            const float cardY0 = kListTopY;
            const float cardGap = 12.0f;

            // 列表底 = 底部按钮行上沿 − 间距
            const float listBottom = bottomButtonRowY(bodyH) - kListBottomGap;

            if (!profilesError.empty()) {
                ui.text("claude.profiles.error")
                    .position(20.0f, cardY0 + 20.0f)
                    .size(leftW - 40.0f, 60.0f)
                    .text("Profile 列表加载失败：" + profilesError)
                    .fontSize(core::theme::scaled(12.0f))
                    .lineHeight(core::theme::scaled(20.0f))
                    .color(t.colors.errorText)
                    .build();
            } else if (profiles.empty()) {
                ui.text("claude.profiles.empty")
                    .position(20.0f, cardY0 + 20.0f)
                    .size(leftW - 40.0f, 60.0f)
                    .text("暂无 Profile，点击下方按钮新建")
                    .fontSize(core::theme::scaled(12.0f))
                    .lineHeight(core::theme::scaled(20.0f))
                    .color(t.colors.textTertiary)
                    .build();
            } else {
                // virtualList：仅可见行进 slot 窗口重建，overscan 一个视口高；列表恒吃剩余高
                const float rowPitch = 56.0f + cardGap;
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
                            .size(contentWidth, 56.0f)
                            .content([&] {
                                // 右键菜单锚点 = PointerEvent 原始坐标经 menuAnchorForPointer 换算（页面坐标）；st 为引用写回模块状态
                                inner.stack(cardId + ".wrap")
                                    .size(contentWidth, 56.0f)
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
                                            st, effSelectedId);
                                    })
                                    .build();
                            })
                            .build();
                    })
                    .build();
            }

            // S3.4 底部按钮主次分级：新建=Filled，导入/CC导入=Outline。
            // H2 残留修复：等分三键 (leftW-64)/3≈58.7px 容不下「从 CC 导入」
            // 14px/600 实宽约 66px 文字（EUI 按钮文本无 clip，超出即溢出按钮外）。
            // 改两行布局 + 每键 fit-content 宽度（规格：内边距水平 16 + 文字实宽
            // 14/600；S4.5 单按钮公式 W=textWidth+32 钳 [96,200]），禁缩字号。
            constexpr float kBtnPadH = 16.0f;   // 规格内边距水平 16
            constexpr float kBtnFont = 14.0f;   // 规格文字 14/600
            constexpr int kBtnWeight = 600;
            const auto fitBtnW = [](const std::string& label) {
                float textW = core::TextPrimitive::measureTextWidth(
                    label, {}, kBtnFont, kBtnWeight);
                if (textW <= 0.0f) {
                    // 测量不可用（无字体上下文）退文本估算：CJK 全宽 14、ASCII 半宽
                    // 7（三个按钮文案的保守上界，宁宽勿裁）。按 UTF-8 字符迭代——
                    // 字节级计数会把 3 字节 CJK 当成 3 个字符。
                    textW = 0.0f;
                    for (std::size_t i = 0; i < label.size(); ) {
                        const unsigned char b = static_cast<unsigned char>(label[i]);
                        const bool isAscii = (b < 0x80);
                        textW += isAscii ? kBtnFont * 0.5f : kBtnFont;
                        // UTF-8 步进：ASCII 1 字节，multi-byte 按头字节前导 1 位数跳
                        if (isAscii) { ++i; continue; }
                        int nBytes = 0;
                        if ((b & 0xE0) == 0xC0) nBytes = 2;
                        else if ((b & 0xF0) == 0xE0) nBytes = 3;
                        else if ((b & 0xF8) == 0xF0) nBytes = 4;
                        i += (nBytes > 0 ? static_cast<std::size_t>(nBytes) : 1);
                    }
                }
                return textW + 2.0f * kBtnPadH;   // 原值返回，钳制由布局处做
            };
            const float btnNewW =
                std::clamp(fitBtnW("+ 新建 Profile"), 96.0f, 200.0f);
            const float btnGap = 12.0f;
            // 同行两键不越面板右缘（20 + w + gap + w ≤ leftW - 20）：先给 CC 键
            // fit 宽（文字实测 + 2×16，H2 点名），左键吃剩余（不再套 96 下限——
            // 等分行给的空间本就只有 58.7，剩余不足时优先保住不溢出；正常测量下
            // 88+12+98=198 ≤ 200，两侧都是满规格内边距）
            const float importPairMaxW = leftW - 40.0f - btnGap;
            const float btnCcW = std::min(fitBtnW("从 CC 导入"), 200.0f);
            const float importW =
                std::min(fitBtnW("导入配置"), importPairMaxW - btnCcW);
            // 行 1 顶 = 按钮区顶（区块贴底锚点：区块底 = 新建钮 y − 12，测试锁此式）
            const float row1Y = bottomButtonRowY(bodyH);
            const float row2Y = row1Y + 40.0f + btnGap;
            // 行 1：新建（主操作独行）Filled（绿底白字）
            components::button(ui, "claude.profiles.new")
                .position(20.0f, row1Y)
                .size(btnNewW, 40.0f)
                .text("+ 新建 Profile")
                .fontSize(kBtnFont)
                .colors(t.colors.primary,
                        t.colors.primaryHover,
                        t.colors.primaryPressed)
                .textColor(t.colors.onPrimary)
                .radius(8.0f)
                .onClick([onNew = actions.onNewProfile] { onNew(); })
                .build();
            // 行 2：导入配置 + 从 CC 导入（两 Outline 同行，白底绿边绿字）
            components::button(ui, "claude.profiles.import")
                .position(20.0f, row2Y)
                .size(importW, 40.0f)
                .text("导入配置")
                .fontSize(kBtnFont)
                .colors(t.colors.surface,
                        t.colors.primarySoftBg,
                        t.colors.primarySoftBg)
                .textColor(t.colors.primary)
                .radius(8.0f)
                .border(1.0f, t.colors.primary)
                .onClick([onImport = actions.onImportCurrent] { onImport(); })
                .build();
            components::button(ui, "claude.profiles.import.ccswitch")
                .position(20.0f + importW + btnGap, row2Y)
                .size(btnCcW, 40.0f)
                .text("从 CC 导入")
                .fontSize(kBtnFont)
                .colors(t.colors.surface,
                        t.colors.primarySoftBg,
                        t.colors.primarySoftBg)
                .textColor(t.colors.primary)
                .radius(8.0f)
                .border(1.0f, t.colors.primary)
                .onClick([onImportCc = actions.onImportCcSwitch] {
                    if (onImportCc) onImportCc();
                })
                .build();
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
    constexpr float h = 156.0f;
    const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Compact);
    ui_kit::DialogFrame frame;
    frame.id = "claude.dialog.switch";
    frame.width = ui_kit::DialogWidth::Compact;
    frame.height = h;
    frame.open = st.switchConfirmOpen_;
    frame.onOpenChange = [sp = &st](bool v) { sp->switchConfirmOpen_ = v; };

    ui_kit::drawDialogFrame(ui, screen, frame, themeTokens, [&] {
        ui_kit::drawDialogTitle(ui, "claude.dialog.switch.title", w,
                                "切换 Profile", 20.0f, t.colors.textPrimary);
        ui_kit::drawDialogText(
            ui, "claude.dialog.switch.msg", w,
            "切换到\"" + st.switchTargetName_ + "\"？代理配置将立即生效",
            14.0f, t.colors.textSecondary,
            {.y = 56.0f, .height = 40.0f, .lineHeight = 20.0f,
             .wrap = true});

        ui_kit::DialogButtonSpec cancel;
        cancel.label = "取消";
        cancel.fontSize = 14.0f;
        cancel.style = ui_kit::DialogButtonStyle::SecondaryThemed;
        cancel.onClick = [sp = &st] { sp->switchConfirmOpen_ = false; };
        ui_kit::DialogButtonSpec confirm;
        confirm.label = "确认切换";
        confirm.fontSize = 14.0f;
        confirm.style = ui_kit::DialogButtonStyle::PrimaryThemed;
        confirm.onClick = [sp = &st] {
            sp->selectedId_ = sp->switchTargetId_;
            sp->switchConfirmOpen_ = false;
        };
        ui_kit::drawDialogButtons(ui, "claude.dialog.switch", w, h, cancel,
                                  confirm, t, themeTokens);
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
