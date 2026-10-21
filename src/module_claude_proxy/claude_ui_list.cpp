// Profile 列表 UI：header / 引导条 / 左栏 Profile 列表与共享配置区块的绘制。
#include "claude_ui_list.h"

#include "claude_ui_logic.h"
#include "core/component_theme.h"
#include "provider_config.h"
#include "settings_sync.h"

#include <components/button.h>
#include <components/contextmenu.h>
#include <ui_kit/dialog_form.h>
#include <ui_kit/glass_card.h>
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

// 共享配置区块（UAT5 可折叠）：钉面板底；折叠 = 一行头（30），展开 = 头 + 恒 2 行
// 卡片框架（158），左列列表视口不随增删片段重排
constexpr float kSharedHeaderBottom = 30.0f;   // 头行高（标题/计数/勾箭/+ 钮），两态同高
constexpr float kSharedCardH = 56.0f;
constexpr float kSharedCardGap = 8.0f;
constexpr float kSharedInfoH = 18.0f;          // 错误 / 空态行高
constexpr float kSharedBottomGap = 12.0f;      // 区块底 → 底部按钮行顶
constexpr float kSharedListMinH = 96.0f;       // 列表视口保底高
constexpr float kListTopY = 56.0f;             // 列表 / 区块所在列的起点 y
constexpr float kBottomBtnRowH = 64.0f;        // 底部按钮行（新建/导入）预留高

// 底部按钮行上沿（面板内绝对 y）
float bottomButtonRowY(float bodyH) { return bodyH - kBottomBtnRowH; }

// 区块锚点：贴底时区块底沿的绝对 y
float sharedBlockBottomY(float bodyH) {
    return bottomButtonRowY(bodyH) - kSharedBottomGap;
}

// 可见高上界 = 区块底 − 列表起点 − 列表保底 − 间距（保证列表视口不被挤没）
float sharedBlockMaxH(float bodyH) {
    return std::max(0.0f, sharedBlockBottomY(bodyH) - kListTopY -
                              kSharedListMinH - kSharedBottomGap);
}

// 条目区恒 2 行高、超出滚动，不随条目数涨
constexpr size_t kSharedVisibleCards = 2;

// 条目区高按整行取口：取「2 卡 + 1 间距」会因行容器行尾距在恰 2 条时多出 8px 空滚动条
constexpr float kSharedCardsH = static_cast<float>(kSharedVisibleCards) *
                                (kSharedCardH + kSharedCardGap);

// 区块自然高：折叠 = 头行一行；展开 = 头行 + 条目框架（空 / 错态也按同一框架画，
// 不收缩到实际条数，保证左列列表视口不重排）
float sharedBlockNaturalH(bool collapsed) {
    return collapsed ? kSharedHeaderBottom
                     : kSharedHeaderBottom + kSharedCardsH;
}

// 实际高 = min(自然高, 上界)；取到上界即极矮窗退化，守门见 drawProfileListPanel
float sharedBlockH(float bodyH, bool collapsed) {
    return std::min(sharedBlockNaturalH(collapsed), sharedBlockMaxH(bodyH));
}

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
                     ProfileListState& st) {
    const float h = 56.0f;
    const bool selected = (p.id == st.selectedId_);
    // §5 Profile 选中态（UAT5 改版）：浅灰 wechat.selectedBg 底 + 右缘「绿圈白勾」
    // 唯一承担「谁是选中」语义（原 2px 左竖线移除，不叠双指示）。
    // isActive 不染底色（isActive 语义，与列表选中分立）。
    // UAT6 玻璃：未选中半透面 cardBg@0.62（原透明显列底 pageBg）
    const eui::Color bgNormal = selected
        ? t.colors.wechat.selectedBg
        : components::theme::withOpacity(t.colors.wechat.cardBg, 0.62f);
    // 共享徽标 / 选中绿圈右缘 x（供名称截断让位）
    const float badgeX = selected ? (width - 88.0f) : (width - 60.0f);

    ui.rect(id + ".bg")
        .position(0.0f, y)
        .size(width, h)
        .states(bgNormal, t.colors.wechat.dividerSecondary,
                t.colors.wechat.selectedBg)
        // 行 hover/选中颜色过渡
        .transition(core::Transition::make(0.1f, core::Ease::OutQuad))
        .animate(core::AnimProperty::Color)
        .radius(t.sizes.radiusSmall)
        .border(1.0f, components::theme::withOpacity(t.colors.wechat.cardBg, 0.55f))
        .onClick([sp = &st, pid = p.id, pname = p.name] {
            if (sp->selectedId_ == pid) return;  // 已选卡无操作
            if (sp->switchBypassConfirm_) {       // 测试缝直走
                sp->selectedId_ = pid;
                return;
            }
            sp->switchTargetId_ = pid;
            sp->switchTargetName_ = pname;
            sp->switchConfirmOpen_ = true;
        })
        .build();

    // UAT6 玻璃：淡雅玻璃层（hover/select 由 bg rect states 处理）
    ui_kit::drawGlassCardLayers(ui, id, 0.0f, y, width, h,
        t.sizes.radiusSmall, t, ui_kit::GlassCardStyle::subtle());

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
        .fontSize(16.0f)
        .lineHeight(avatarSize)
        .horizontalAlign(eui::HorizontalAlign::Center)
        // 文本盒 == 圆底高，须显式垂直居中，否则 ink 从行框顶沿起排
        .verticalAlign(eui::VerticalAlign::Center)
        .color(avatar.ring)
        .fontWeight(600)
        .build();

    // 名称：单行截断让位右缘共享徽标 / 选中绿圈（首建无徽标时伸展到卡右界 −12px）
    {
        float nameRight = width - 12.0f;
        if (p.sharedConfigId != 0) {
            nameRight = std::min(nameRight, badgeX - 4.0f);
        }
        if (selected) {
            nameRight = std::min(nameRight, width - 48.0f);  // checkX(44) - 4 gap
        }
        const float nameMaxW = std::max(40.0f, nameRight - 64.0f);
        ui.text(id + ".name")
            .position(64.0f, y + ((h - 20.0f) * 0.5f))
            .size(nameMaxW, 20.0f)
            .maxWidth(nameMaxW)
            .wrap(false)
            .overflow(core::TextOverflow::Ellipsis)
            .text(p.name)
            .fontSize(14.0f)
            .lineHeight(20.0f)
            .fontWeight(600)
            .color(t.colors.wechat.textPrimary)
            .build();
    }

    // 绑定共享片段的卡片角标「共享」：服务层保证无悬空 id（删片段同事务解绑），
    // sharedConfigId != 0 即可判定。选中时右缘让位绿圈白勾（左移 28px），不叠压。
    if (p.sharedConfigId != 0) {
        ui.rect(id + ".shared.badge")
            .position(badgeX, y + 20.0f)
            .size(48.0f, 16.0f)
            .color(t.colors.wechat.dividerSecondary)
            .radius(t.sizes.radiusPill)
            .border(1.0f, t.colors.wechat.dividerPrimary)
            .build();
        ui.text(id + ".shared.badge.text")
            .position(badgeX, y + 20.0f)
            .size(48.0f, 16.0f)
            .text("共享")
            .fontSize(9.0f)
            .lineHeight(16.0f)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.wechat.textTertiary)
            .build();
    }

    // UAT6：绿圈白勾 20→26px（用户拍板），fa-check 同步放大 11→14。右缘
    // checkX = width-42-4 → width-44（26+4+gap=4），与共享徽标 4px 隔开。
    if (selected) {
        constexpr float kCheckD = 26.0f;
        const float checkX = width - 44.0f;
        const float checkY = y + (h - kCheckD) * 0.5f;
        ui.rect(id + ".check")
            .position(checkX, checkY)
            .size(kCheckD, kCheckD)
            .color(t.colors.wechat.primary)
            .radius(kCheckD * 0.5f)
            .build();
        ui.text(id + ".check.icon")
            .position(checkX, checkY)
            .size(kCheckD, kCheckD)
            .icon(0xF00C)  // fa-check
            .fontSize(14.0f)
            .lineHeight(kCheckD)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.wechat.onPrimary)
            .build();
    }

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
                                .color(t.colors.wechat.textPrimary)
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
                                ? t.colors.wechat.primary
                                : t.colors.wechat.textTertiary;
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
                                .color(proxyRunning ? t.colors.wechat.primary
                                                    : t.colors.wechat.textTertiary)
                                .fontWeight(500)
                                .build();
                        })
                        .build();

                    // §5 帮助图标钮（UAT6 问号钮重设计，尺寸对齐 md.help.icon 的
                    // 24px 档）：正圆、icon 居中、hover 三态（透明底→浅灰→按压灰），
                    // 照已过 UAT 的圆钮先例（claude.shared.header 勾箭钮同款结构）。
                    // 从状态组 wrap 行挪出为独立钮——右缘可静态推算（tooltip 锚点
                    // 需要 wrap 行给不了）；长文案 hover 经 EUI tooltip 弹回（下方）。
                    ui.stack("claude.title.help")
                        .size(kHelpBtnD, kHelpBtnD)
                        .visualStateFrom("claude.title.help.bg", 0.965f)
                        .content([&] {
                            ui.rect("claude.title.help.bg")
                                .size(kHelpBtnD, kHelpBtnD)
                                .states(core::Color{0.0f, 0.0f, 0.0f, 0.0f},
                                        t.colors.wechat.selectedBg,
                                        t.colors.wechat.dividerSecondary)
                                .radius(kHelpBtnD * 0.5f)
                                .build();
                            ui.text("claude.title.help.icon")
                                .size(kHelpBtnD, kHelpBtnD)
                                .icon(0xF059)  // fa-circle-question
                                .fontSize(14.0f)
                                .lineHeight(kHelpBtnD)
                                .horizontalAlign(eui::HorizontalAlign::Center)
                                .verticalAlign(eui::VerticalAlign::Center)
                                .color(t.colors.wechat.textTertiary)
                                .build();
                        })
                        .build();

                    // §5「启动代理」微信绿主按钮（§4.1 Primary 绿填充+白字；运行态=Secondary 浅灰「停止代理」），无边框
                    const bool stopped = !proxyRunning;
                    components::button(ui, "claude.btn.proxy")
                        .size(110.0f, 40.0f)
                        .text(proxyRunning ? "停止代理" : "启动代理")
                        .icon(proxyRunning ? 0xF04C : 0xF04B)  // fa-pause / fa-play
                        .fontSize(t.sizes.fontSizeSmall)
                        .colors(stopped ? t.colors.wechat.primary
                                        : t.colors.wechat.secondaryButtonBg,
                                stopped ? t.colors.wechat.primaryHover
                                        : t.colors.wechat.dividerSecondary,
                                stopped ? t.colors.wechat.primaryPressed
                                        : t.colors.wechat.dividerSecondary)
                        .textColor(stopped ? t.colors.wechat.onPrimary
                                           : t.colors.wechat.textPrimary)
                        .iconColor(stopped ? t.colors.wechat.onPrimary
                                           : t.colors.wechat.textPrimary)
                        .radius(t.sizes.radiusWeChat)
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
    components::tooltip(ui, "claude.title.help.tooltip")
        .theme(core::theme::componentTokensWeChat(t))
        .source("claude.title.help.bg")
        .value(proxyRunning ? "本地代理运行中，管理多套 Claude API 配置"
                             : "管理多套 Claude API 配置")
        .minWidth(320.0f)
        .maxWidth(320.0f)
        .build();
}

void drawClaudeGuideBar(eui::Ui& ui, float contentW,
                        const core::theme::Tokens& t) {
    // 引导条：代理未运行时的接管语义提示（图标字体不能与正文混排，图标/文字拆两元素）
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
                .color(t.colors.wechat.textSecondary)
                .build();
            ui.text("claude.guide.takeover")
                .size(contentW - 20.0f, 18.0f)
                .text("启动代理将接管 Claude Code 配置（" +
                      settings_sync::defaultPaths().settingsFile.string() +
                      " 指向本地代理），停止代理时自动还原")
                .fontSize(t.sizes.fontSizeSmall)
                .lineHeight(18.0f)
                .color(t.colors.wechat.textSecondary)
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
                          const std::vector<SharedConfig>& sharedConfigs,
                          const std::string& sharedConfigsError,
                          const ProfileListActions& actions) {
    ui.column("claude.profiles")
        .size(leftW, bodyH)
        .gap(0.0f)
        .content([&] {
            ui.rect("claude.profiles.bg")
                .size(leftW, bodyH)
                // §4.14 列底浅灰底（wechat.pageBg），白卡浮起；边框一级分割线
                .color(t.colors.wechat.pageBg)
                .radius(t.sizes.radiusLarge)
                .border(1.0f, t.colors.wechat.dividerPrimary)
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
                .color(t.colors.wechat.textPrimary)
                .build();

            const float cardY0 = kListTopY;
            const float cardGap = 12.0f;

            // 退化守门只有两种「高度预算不足」：① 展开态区块被压扁（折叠态一行头
            // 无内容区，不适用）；② 列表保底视口被挤没。记 ERROR 而非 assert（Release 会编掉 assert）
            const float sharedH =
                sharedBlockH(bodyH, st.sharedBlockCollapsed_);
            const float sharedBlockTop = sharedBlockBottomY(bodyH) - sharedH;
            if (!st.sharedBlockCollapsed_ &&
                sharedH < kSharedHeaderBottom + kSharedInfoH) {
                LOG_ERROR("[layout] 共享配置区块被压扁（头行与错误/空态行重叠）: "
                          "sharedH=" + std::to_string(sharedH) + " < " +
                          std::to_string(kSharedHeaderBottom + kSharedInfoH) +
                          " bodyH=" + std::to_string(bodyH));
            }

            if (!profilesError.empty()) {
                ui.text("claude.profiles.error")
                    .position(20.0f, cardY0 + 20.0f)
                    .size(leftW - 40.0f, 60.0f)
                    .text("Profile 列表加载失败：" + profilesError)
                    .fontSize(12.0f)
                    .lineHeight(20.0f)
                    .color(t.colors.error)
                    .build();
                // 无 scroll 时共享区块同样钉面板底（占位不写死 y）
                ui.stack("claude.shared.block")
                    .position(20.0f, sharedBlockTop)
                    .size(leftW - 40.0f, sharedH)
                    .content([&] {
                        drawSharedConfigBlock(ui, leftW - 40.0f, sharedH,
                                              t, themeTokens, st, sharedConfigs,
                                              sharedConfigsError, actions);
                    })
                    .build();
            } else if (profiles.empty()) {
                ui.text("claude.profiles.empty")
                    .position(20.0f, cardY0 + 20.0f)
                    .size(leftW - 40.0f, 60.0f)
                    .text("暂无 Profile，点击下方按钮新建")
                    .fontSize(12.0f)
                    .lineHeight(20.0f)
                    .color(t.colors.wechat.textTertiary)
                    .build();
                ui.stack("claude.shared.block")
                    .position(20.0f, sharedBlockTop)
                    .size(leftW - 40.0f, sharedH)
                    .content([&] {
                        drawSharedConfigBlock(ui, leftW - 40.0f, sharedH,
                                              t, themeTokens, st, sharedConfigs,
                                              sharedConfigsError, actions);
                    })
                    .build();
            } else {
                // virtualList：仅可见行进 slot 窗口重建，overscan 一个视口高；视口高 = 区块顶 − 间距 − 列表起点，列表恒吃剩余高
                const float rowPitch = 56.0f + cardGap;
                const float listH = std::max(
                    0.0f, sharedBlockTop - kSharedBottomGap - cardY0);
                // 退化守门：仅 bodyH 极小时区块高上界被夹到 0 而失效
                if (listH < kSharedListMinH) {
                    LOG_ERROR("[layout] Profile 列表保底视口被挤没: listH=" +
                              std::to_string(listH) + " < " +
                              std::to_string(kSharedListMinH) + " bodyH=" +
                              std::to_string(bodyH));
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
                                            st);
                                    })
                                    .build();
                            })
                            .build();
                    })
                    .build();
                // 共享区块不并入 virtualList 行窗（无非等高末行支持），垫在列表下方钉面板底，位于视口 clip 之外
                ui.stack("claude.shared.block")
                    .position(20.0f, sharedBlockTop)
                    .size(leftW - 40.0f, sharedH)
                    .content([&] {
                        drawSharedConfigBlock(ui, leftW - 40.0f, sharedH,
                                              t, themeTokens, st, sharedConfigs,
                                              sharedConfigsError, actions);
                    })
                    .build();
            }

            // 底部三键等分（新建 / 导入配置 / 从 CC 导入），12px 间距
            const float btnW = (leftW - 40.0f - 24.0f) * (1.0f / 3.0f);
            const float btnGap = 12.0f;
            // §5 列表底部按钮=Secondary 浅灰次按钮（§4.1：#F2F2F2 底 + 深灰字 + 6px 圆角，无边框）
            components::button(ui, "claude.profiles.new")
                .position(20.0f, bottomButtonRowY(bodyH))
                .size(btnW, 40.0f)
                .text("+ 新建 Profile")
                .fontSize(t.sizes.fontSizeSmall)
                .colors(t.colors.wechat.secondaryButtonBg,
                        t.colors.wechat.dividerSecondary,
                        t.colors.wechat.dividerSecondary)
                .textColor(t.colors.wechat.textPrimary)
                .radius(t.sizes.radiusWeChat)
                .onClick([onNew = actions.onNewProfile] { onNew(); })
                .build();
            components::button(ui, "claude.profiles.import")
                .position(20.0f + btnW + btnGap, bottomButtonRowY(bodyH))
                .size(btnW, 40.0f)
                .text("导入配置")
                .fontSize(t.sizes.fontSizeSmall)
                .colors(t.colors.wechat.secondaryButtonBg,
                        t.colors.wechat.dividerSecondary,
                        t.colors.wechat.dividerSecondary)
                .textColor(t.colors.wechat.textPrimary)
                .radius(t.sizes.radiusWeChat)
                .onClick([onImport = actions.onImportCurrent] { onImport(); })
                .build();
            components::button(ui, "claude.profiles.import.ccswitch")
                .position(20.0f + 2.0f * (btnW + btnGap),
                          bottomButtonRowY(bodyH))
                .size(btnW, 40.0f)
                .text("从 CC 导入")
                .fontSize(t.sizes.fontSizeSmall)
                .colors(t.colors.wechat.secondaryButtonBg,
                        t.colors.wechat.dividerSecondary,
                        t.colors.wechat.dividerSecondary)
                .textColor(t.colors.wechat.textPrimary)
                .radius(t.sizes.radiusWeChat)
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

// 共享配置区块（UAT5 可折叠）：默认收起只画一行头，点头行/勾箭钮展开；「+」
// 圆钮两态常驻头行右缘（原区块底整宽「新建片段」钮已删，入口不丢）。
// 块高由调用方按 sharedBlockH(bodyH, collapsed) 传入，块内元素相对块顶从 0 起算。
// id: claude.shared.block / .header.toggle / .title / .header.chevron / .header.add
//     / .error / .empty / .scroll / .card.N.*
void drawSharedConfigBlock(eui::Ui& ui, float width, float blockH,
                           const core::theme::Tokens& t,
                           const components::theme::ThemeColorTokens& themeTokens,
                           ProfileListState& st,
                           const std::vector<SharedConfig>& sharedConfigs,
                           const std::string& sharedConfigsError,
                           const ProfileListActions& actions) {
    const bool collapsed = st.sharedBlockCollapsed_;
    const float rowH = kSharedHeaderBottom;

    // ── 头行（两态恒在）：整行可点切换折叠（wechat 透明底 + hover 高亮，
    // md 目录行同款）；按钮后画 = 命中层压在行之上，点按钮不触发折叠 ──
    ui.rect("claude.shared.header.toggle")
        .position(0.0f, 0.0f)
        .size(width, rowH)
        .states(eui::Color{0.0f, 0.0f, 0.0f, 0.0f},
                t.colors.wechat.dividerSecondary,
                t.colors.wechat.selectedBg)
        .radius(t.sizes.radiusWeChat)
        .onClick([sp = &st] {
            sp->sharedBlockCollapsed_ = !sp->sharedBlockCollapsed_;
        })
        .build();

    // 标题 + 条目计数（错态不计数；空集不挂「· 0」尾巴）
    const std::string countSuffix =
        (sharedConfigsError.empty() && !sharedConfigs.empty())
            ? std::string(" · ") + std::to_string(sharedConfigs.size())
            : std::string();
    ui.text("claude.shared.title")
        .position(12.0f, 0.0f)
        .size(std::max(40.0f, width - 110.0f), rowH)
        .text(std::string("共享配置片段") + countSuffix)
        .fontSize(13.0f)
        .lineHeight(rowH)
        .fontWeight(600)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.wechat.textPrimary)
        .build();

    // 勾箭钮（UAT6 居中修复：components::button 的 iconWidth=iconFont*1.15
    // 对窄 FA 字形不精确 → 自构建，icon 占满 26px 圆，horizontalAlign::Center 居中）
    {
        constexpr float kD = 26.0f;
        const float x0 = width - 72.0f;
        const float y0 = 2.0f;
        auto normalC = t.colors.wechat.cardBg;
        auto hoverC  = core::mixColor(normalC, t.colors.wechat.primary, 0.10f);
        auto pressC  = core::mixColor(normalC, t.colors.wechat.selectedBg, 0.22f);
        ui.stack("claude.shared.header.chevron")
            .position(x0, y0).size(kD, kD)
            .visualStateFrom("claude.shared.header.chevron.bg", 0.965f)
            .content([&] {
                ui.rect("claude.shared.header.chevron.bg")
                    .size(kD, kD)
                    .states(normalC, hoverC, pressC)
                    .radius(t.sizes.radiusWeChat)
                    .onClick([sp = &st] {
                        sp->sharedBlockCollapsed_ = !sp->sharedBlockCollapsed_;
                    })
                    .build();
                ui.text("claude.shared.header.chevron.icon")
                    .size(kD, kD)
                    .icon(collapsed ? 0xF078 : 0xF077)
                    .fontSize(13.0f)
                    .lineHeight(kD)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.wechat.textSecondary)
                    .build();
            })
            .build();
    }

    // 「+」圆钮（UAT6 居中修复：components::button 的 iconWidth=iconFont*1.15
    // 对窄 FA 字形不精确 → 自构建，icon 占满 26px 圆，horizontalAlign::Center 居中）
    {
        constexpr float kD = 26.0f;
        const float x0 = width - 42.0f;
        const float y0 = 2.0f;
        ui.stack("claude.shared.header.add")
            .position(x0, y0).size(kD, kD)
            .visualStateFrom("claude.shared.header.add.bg", 0.965f)
            .content([&] {
                ui.rect("claude.shared.header.add.bg")
                    .size(kD, kD)
                    .states(t.colors.wechat.primary,
                            t.colors.wechat.primaryHover,
                            t.colors.wechat.primaryPressed)
                    .radius(13.0f)
                    .onClick([onNew = actions.onNewShared] { onNew(); })
                    .build();
                ui.text("claude.shared.header.add.icon")
                    .size(kD, kD)
                    .icon(0xF067)  // fa-plus
                    .fontSize(13.0f)
                    .lineHeight(kD)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.wechat.onPrimary)
                    .build();
            })
            .build();
    }

    if (collapsed) {
        return;   // 折叠态：列表区不渲染（错误/空态文案也收进展开区）
    }

    const float contentTop = rowH;
    const float contentH = std::max(0.0f, blockH - rowH);
    // 片段集 / 空态 / 错误态任一变化 → 块内滚动偏移归零（签名跨数据集残留会跳回旧偏移）；签名计算放在分支之前
    size_t scrollKey = sharedConfigsError.empty() ? 0u : 1u;
    for (const auto& sc : sharedConfigs) {
        scrollKey = scrollKey * 1315423911u +
                    std::hash<std::string>{}(sc.name) +
                    static_cast<size_t>(sc.id);
    }
    if (st.sharedScrollKey_ != scrollKey) {
        st.sharedScrollKey_ = scrollKey;
        st.sharedScrollOffset_ = 0.0f;
    }
    if (!sharedConfigsError.empty()) {
        ui.text("claude.shared.error")
            .position(0.0f, contentTop).size(width, kSharedInfoH)
            .text("共享配置片段加载失败：" + sharedConfigsError)
            .fontSize(11.0f).lineHeight(kSharedInfoH)
            .color(t.colors.error)
            .build();
    } else if (sharedConfigs.empty()) {
        // §5 空状态引导新建（empty_state.h 语义内联）；「+」钮在头行右上 → 口径同步
        ui.text("claude.shared.empty")
            .position(0.0f, contentTop).size(width, kSharedInfoH)
            .text("暂无片段，点击右上 + 新建")
            .fontSize(11.0f).lineHeight(kSharedInfoH)
            .color(t.colors.wechat.textTertiary)
            .build();
    } else {
        // 片段卡片：name + 键数；点击开编辑弹窗，装不下时块内滚
        components::scrollView(ui, "claude.shared.scroll")
            .position(0.0f, contentTop)
            .size(width, contentH)
            .gap(0.0f)
            .step(kSharedCardH + kSharedCardGap)
            .theme(themeTokens)
            .contentKey("claude.shared.scroll." + std::to_string(scrollKey))
            .offset(st.sharedScrollOffset_)
            .onChange([sp = &st](float v) { sp->sharedScrollOffset_ = v; })
            .content([&](eui::Ui& inner, float contentWidth, float viewportHeight) {
                (void)viewportHeight;
                for (size_t i = 0; i < sharedConfigs.size(); ++i) {
                    const SharedConfig& sc = sharedConfigs[i];
                    const std::string cid =
                        "claude.shared.card." + std::to_string(i);
                    // 行容器 = 定高 stack：滚动内容是流式 column，行距由容器高多出的 kSharedCardGap 提供
                    inner.stack(cid + ".item")
                        .size(contentWidth, kSharedCardH + kSharedCardGap)
                        .content([&] {
                            inner.rect(cid + ".bg")
                                .position(0.0f, 0.0f)
                                .size(contentWidth, kSharedCardH)
                                .states(components::theme::withOpacity(t.colors.wechat.cardBg, 0.62f),
                                        t.colors.wechat.dividerSecondary,
                                        t.colors.wechat.selectedBg)
                                .radius(t.sizes.radiusWeChat)
                                .border(1.0f, components::theme::withOpacity(t.colors.wechat.cardBg, 0.55f))
                                .onClick([onEdit = actions.requestEditShared, sc] {
                                    onEdit(sc);   // 按值拷贝（sc 引用 compose 局部副本，跨帧悬垂）
                                })
                                .build();
                            ui_kit::drawGlassCardLayers(inner, cid,
                                0.0f, 0.0f, contentWidth, kSharedCardH,
                                t.sizes.radiusWeChat, t,
                                ui_kit::GlassCardStyle::subtle());
                            inner.text(cid + ".name")
                                .position(16.0f, 8.0f)
                                .size(contentWidth - 32.0f, 20.0f)
                                .text(sc.name)
                                .fontSize(13.0f).lineHeight(20.0f)
                                .fontWeight(600)
                                .color(t.colors.wechat.textPrimary)
                                .build();
                            inner.text(cid + ".keys")
                                .position(16.0f, 32.0f)
                                .size(contentWidth - 32.0f, 14.0f)
                                .text(sc.extraSettingsJson.empty()
                                          ? std::string("高级属性 JSON 预设（未配置）")
                                          : std::string("高级属性 JSON 预设 · 绑定它的 Profile 写入时覆盖同名键"))
                                .fontSize(11.0f).lineHeight(14.0f)
                                .color(t.colors.wechat.textSecondary)
                                .build();
                        })
                        .build();
                }
            })
            .build();
    }
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
