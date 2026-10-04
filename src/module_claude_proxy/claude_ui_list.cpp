// Profile 列表 UI：header / 引导条 / 左栏 Profile 列表与共享配置区块的绘制。
#include "claude_ui_list.h"

#include "claude_ui_logic.h"
#include "provider_config.h"
#include "settings_sync.h"

#include <components/button.h>
#include <components/contextmenu.h>
#include <components/scrollview.h>
#include <components/virtuallist.h>

#include <core/logging.h>
#include <core/router.h>
#include <core/ui_geometry.h>

#include <algorithm>
#include <cstdint>
#include <functional>

namespace module::claude {

namespace {

// 共享配置区块几何：区块**钉面板底**（锚点 = 底部按钮行上沿 − 间距），自然高
// 恒定（固定部分 + 2 行条目），增删片段 / 出错时左列列表视口不重排；条目装
// 不下时在块内滚，「+ 新建片段」为固定尾部恒可见可点。列表视口吃剩余高。
constexpr float kSharedHeaderBottom = 30.0f;   // 标题行 + 下边距
constexpr float kSharedCardH = 56.0f;
constexpr float kSharedCardGap = 8.0f;
constexpr float kSharedBtnTopGap = 12.0f;
constexpr float kSharedBtnH = 34.0f;
constexpr float kSharedInfoH = 18.0f;          // 错误 / 空态行高
constexpr float kSharedBottomGap = 12.0f;      // 区块底 → 底部按钮行顶
constexpr float kSharedListMinH = 96.0f;       // 列表视口保底高
constexpr float kListTopY = 56.0f;             // 列表 / 区块所在列的起点 y
constexpr float kBottomBtnRowH = 64.0f;        // 底部按钮行（新建/导入）预留高

// 区块固定部分：标题行 + 按钮上间距 + 按钮高
constexpr float kSharedFixedH =
    kSharedHeaderBottom + kSharedBtnTopGap + kSharedBtnH;

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

// 条目区恒 2 行高（用户要求最多显示 2 条、超出滚动），不随条目数涨
constexpr size_t kSharedVisibleCards = 2;

// 条目区高按整行取口而非「2 卡 + 1 间距」(120)：行容器自带行尾距，恰 2 条时
// 内容高 128 == 视口 → 不出滚动条；取 120 会让恰 2 条也出一条 8px 空滚动条
constexpr float kSharedCardsH = static_cast<float>(kSharedVisibleCards) *
                                (kSharedCardH + kSharedCardGap);

// 区块自然高恒定：空 / 错态也按同一框架画（提示行画在框架顶部、其余留白），
// 不收缩到实际条数——区块恒高，左列列表视口增删片段 / 出错恢复不重排
float sharedBlockNaturalH() {
    return kSharedFixedH + kSharedCardsH;
}

// 实际高 = min(自然高, 上界)；取到上界即极矮窗退化，守门见 drawProfileListPanel
float sharedBlockH(float bodyH) {
    return std::min(sharedBlockNaturalH(), sharedBlockMaxH(bodyH));
}

// 头像圆圈随机色：compose 每帧都跑，真随机会闪烁 → 按 profile id 稳定派生
//（FNV-1a 哈希取色板下标，同一 profile 永远同色）。色板为 9 个低饱和暖色
//（与主题主色同族），深浅主题各派生 fill/ring 两档（eui::mixColor）
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

// 取名字首 UTF-8 码点：旧 std::string(1, name[0]) 只取首字节，CJK 首字符是
// 3 字节序列，截出单字节渲染成 ?；非法 / 截断序列回退 ?
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

// Profile 卡片（点击选中，选中/激活高亮）。
// st 为引用：点击回调写回选中态（须指向存活到下一帧的对象 = 模块状态成员）
void drawProfileCard(eui::Ui& ui,
                     const std::string& id,
                     float y,
                     float width,
                     const core::theme::Tokens& t,
                     const Profile& p,
                     ProfileListState& st) {
    const float h = 96.0f;
    const bool selected = (p.id == st.selectedId_);
    const eui::Color borderColor = (p.isActive || selected)
        ? t.colors.primary : t.colors.borderDefault;
    const float borderW = p.isActive ? 2.0f : 1.0f;
    const eui::Color bg = p.isActive ? t.colors.primarySoftBg : t.colors.surface;

    // 卡片背景（可点击 → 选中后详情面板跟随）
    ui.rect(id + ".bg")
        .position(0.0f, y)
        .size(width, h)
        .states(bg, t.colors.hover, t.colors.active)
        // 行 hover/选中颜色过渡：bg 与 border 同帧目标跳变也走动画
        .transition(core::Transition::make(0.1f, core::Ease::OutQuad))
        .animate(core::AnimProperty::Color | core::AnimProperty::Border)
        .radius(t.sizes.radiusLarge)
        .border(borderW, borderColor)
        .onClick([sp = &st, pid = p.id] { sp->selectedId_ = pid; })
        .build();

    // 头像：按 profile id 稳定派生的圆 + 1.5px 同色系包边（旧 1px 中性边
    // 用户看不见）；垂直居中由 (h - 头像径) 推导，行高变了也居中
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
    // 头像首字按 UTF-8 码点取（旧取首字节使 CJK 名渲染成 ?），颜色随包边
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

    // 名称右缘贴 ACTIVE 徽标左缘前 4px；徽标仅激活时占位，超长文本仍会
    // 从盒尾溢出被徽标盖住（与 URL 同款）
    ui.text(id + ".name")
        .position(64.0f, y + 18.0f)
        .size(width - 64.0f, 20.0f)
        .text(p.name)
        .fontSize(t.sizes.fontSizeBody)
        .lineHeight(20.0f)
        .fontWeight(600)
        .color(t.colors.textPrimary)
        .build();

    if (p.isActive) {
        ui.rect(id + ".active.bg")
            .position(width - 60.0f, y + 16.0f)
            .size(48.0f, 20.0f)
            .color(t.colors.primary)
            .radius(t.sizes.radiusPill)
            .build();
        // 胶囊文字须显式垂直居中：runtime 只在 verticalAlign=Center 时把
        // ink 中心对齐到 frame 中心，否则从行框顶沿起排，看着偏上
        ui.text(id + ".active.text")
            .position(width - 60.0f, y + 16.0f)
            .size(48.0f, 20.0f)
            .text("ACTIVE")
            .fontSize(9.0f)
            .lineHeight(20.0f)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.onPrimary)
            .fontWeight(700)
            .build();
    }

    // base-url：右缘贴共享徽标左缘（width-60）前 4px
    ui.text(id + ".url")
        .position(64.0f, y + 44.0f)
        .size(width - 64.0f, 14.0f)
        .text(p.baseUrl)
        .fontSize(11.0f)
        .lineHeight(14.0f)
        .color(t.colors.textTertiary)
        .build();

    // 绑定了共享配置片段的卡片角标「共享」：服务层保证无悬空 id（删片段
    // 同事务解绑），故 sharedConfigId != 0 即可判定
    if (p.sharedConfigId != 0) {
        ui.rect(id + ".shared.badge")
            .position(width - 60.0f, y + 42.0f)
            .size(48.0f, 16.0f)
            .color(t.colors.surfaceElevated)
            .radius(t.sizes.radiusPill)
            .border(1.0f, t.colors.borderDefault)
            .build();
        ui.text(id + ".shared.badge.text")
            .position(width - 60.0f, y + 42.0f)
            .size(48.0f, 16.0f)
            .text("共享")
            .fontSize(9.0f)
            .lineHeight(16.0f)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.textTertiary)
            .build();
    }

    // model 行：显示层剥 [1M]（与详情页 1M 复选框语义重复），存储与发上游
    // 的原始串不动
    ui.text(id + ".model")
        .position(64.0f, y + 62.0f)
        .size(width - 64.0f, 14.0f)
        .text(std::string("model: ") + deriveDisplayName(
                  effectiveModelsConfig(p.modelsConfigJson).def.model))
        .fontSize(11.0f)
        .lineHeight(14.0f)
        .color(t.colors.textSecondary)
        .build();
}

} // namespace

void drawClaudeHeader(eui::Ui& ui, float contentW, float headerH,
                      const core::theme::Tokens& t,
                      bool proxyRunning, const std::string& portStr,
                      const std::function<void()>& onToggleProxy) {
    // header 行 gap=0，两段宽必须相加等于 contentW，防字面量两处改一处忘
    constexpr float kHeaderActionsW = 360.0f;

    ui.row("claude.header")
        .size(contentW, headerH)
        .alignItems(eui::Align::CENTER)
        .content([&] {
            ui.column("claude.title.col")
                .size(contentW - kHeaderActionsW, headerH)
                .gap(6.0f)
                .content([&] {
                    ui.row("claude.title.row")
                        .size(contentW - kHeaderActionsW, 28.0f)
                        .gap(12.0f)
                        .alignItems(eui::Align::CENTER)
                        .content([&] {
                            ui.text("claude.h1")
                                .size(220.0f, 32.0f)
                                .text("Claude 切换器")
                                .fontSize(t.sizes.fontSizeTitle)
                                .lineHeight(32.0f)
                                .fontWeight(700)
                                .color(t.colors.textPrimary)
                                .build();

                            const eui::Color dotColor = proxyRunning
                                ? t.colors.success
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
                                .size(200.0f, 18.0f)
                                .text(statusText)
                                .fontSize(t.sizes.fontSizeSmall)
                                .lineHeight(18.0f)
                                .color(proxyRunning ? t.colors.success
                                                    : t.colors.textTertiary)
                                .fontWeight(500)
                                .build();
                        })
                        .build();

                    // 代理未运行时不显示假地址，改为可操作的提示
                    const std::string subtitle = proxyRunning
                        ? (std::string("管理多套 Claude API 配置 · 本地代理 http://127.0.0.1") +
                           portStr)
                        : std::string("管理多套 Claude API 配置 · 代理已停止，点右上角启动");
                    ui.text("claude.subtitle")
                        .size(contentW - kHeaderActionsW, 16.0f)
                        .text(subtitle)
                        .fontSize(t.sizes.fontSizeSmall)
                        .lineHeight(16.0f)
                        .color(t.colors.textSecondary)
                        .build();
                })
                .build();

            ui.row("claude.actions")
                .size(kHeaderActionsW, 40.0f)
                .justifyContent(eui::Align::END)
                .alignItems(eui::Align::CENTER)
                .gap(t.sizes.spacingSmall)
                .content([&] {
                    components::button(ui, "claude.btn.proxy")
                        .size(110.0f, 40.0f)
                        .text(proxyRunning ? "停止代理" : "启动代理")
                        .icon(proxyRunning ? 0xF04C : 0xF04B)  // fa-pause / fa-play
                        .fontSize(t.sizes.fontSizeSmall)
                        .colors(t.colors.surfaceElevated, t.colors.hover,
                                t.colors.active)
                        .textColor(t.colors.textSecondary)
                        .iconColor(t.colors.textSecondary)
                        .radius(t.sizes.radius)
                        .border(1.0f, t.colors.borderDefault)
                        .onClick([onToggleProxy] { onToggleProxy(); })
                        .build();
                })
                .build();
        })
        .build();
}

void drawClaudeGuideBar(eui::Ui& ui, float contentW,
                        const core::theme::Tokens& t) {
    // 引导条：代理未运行时的接管语义提示（图标/文字拆两元素，图标字体
    // 不能与正文混排）
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
                .size(contentW - 20.0f, 18.0f)
                .text("启动代理将接管 Claude Code 配置（" +
                      settings_sync::defaultPaths().settingsFile.string() +
                      " 指向本地代理），停止代理时自动还原")
                .fontSize(t.sizes.fontSizeSmall)
                .lineHeight(18.0f)
                .color(t.colors.textSecondary)
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

            // 共享区块钉面板底（位置/高度见文件顶部常量）。退化守门只有两种
            // 从定义推不出的「高度预算不足」：① 区块被压扁到 < 固定部分 +
            // 提示行（标题/空态行与按钮重叠）；② 列表保底视口被挤没。按统一
            // 原则记 ERROR 可见，不做 assert——退化域低于支持的最小窗，GUI
            // 不该为此崩，且 Release 会把 assert 编掉
            const float sharedH = sharedBlockH(bodyH);
            const float sharedBlockTop = sharedBlockBottomY(bodyH) - sharedH;
            if (sharedH < kSharedFixedH + kSharedInfoH) {
                LOG_ERROR("[layout] 共享配置区块被压扁（标题/空态行与按钮重叠）: "
                          "sharedH=" + std::to_string(sharedH) + " < " +
                          std::to_string(kSharedFixedH + kSharedInfoH) +
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
                    .color(t.colors.textTertiary)
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
                // virtualList：仅可见行进 slot 窗口重建，行 pitch = 96 +
                // cardGap，overscan 一个视口高。视口高 = 区块顶 − 间距 − 列表
                // 起点（锚定式推导），列表恒吃剩余高；区块高的上界已保证
                // listH ≥ 列表保底，旧 96px clamp 不再需要
                const float rowPitch = 96.0f + cardGap;
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
                        // 行容器 = 裸 stack：card 组件自带底色会与卡内交互层
                        // .bg 叠成双底色，故只保留 .bg 作唯一底；事件挂点与
                        // 卡装饰都在 drawProfileCard
                        const std::string cardId =
                            "claude.profile." + std::to_string(idx);
                        inner.stack(rowId + ".card")
                            .size(contentWidth, 96.0f)
                            .content([&] {
                                // 右键菜单锚点 = PointerEvent 原始坐标经
                                // menuAnchorForPointer 换算（页面坐标，
                                // onContextMenu 通道已除 dpiScale）；st 为
                                // 引用：写回存活到下一帧的模块状态
                                inner.stack(cardId + ".wrap")
                                    .size(contentWidth, 96.0f)
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
                // 共享区块不并入 virtualList 行窗（等高槽位无非等高末行支持），
                // 垫在列表下方钉面板底；位于视口 clip 之外，末卡滚到底不会被
                // 盖住 / 误点
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

            // 底部三键等分（新建 / 导入当前配置 / 从 cc-switch 导入），
            // 12px 间距
            const float btnW = (leftW - 40.0f - 24.0f) * (1.0f / 3.0f);
            const float btnGap = 12.0f;
            components::button(ui, "claude.profiles.new")
                .position(20.0f, bottomButtonRowY(bodyH))
                .size(btnW, 40.0f)
                .text("+ 新建 Profile")
                .fontSize(t.sizes.fontSizeSmall)
                .colors(t.colors.surfaceElevated, t.colors.hover,
                        t.colors.active)
                .textColor(t.colors.textPrimary)
                .radius(t.sizes.radius)
                .border(1.0f, t.colors.borderDefault)
                .onClick([onNew = actions.onNewProfile] { onNew(); })
                .build();
            components::button(ui, "claude.profiles.import")
                .position(20.0f + btnW + btnGap, bottomButtonRowY(bodyH))
                .size(btnW, 40.0f)
                .text("导入当前配置")
                .icon(0xF019)  // fa-download
                .fontSize(t.sizes.fontSizeSmall)
                .colors(t.colors.surfaceElevated, t.colors.hover,
                        t.colors.active)
                .textColor(t.colors.textPrimary)
                .iconColor(t.colors.textPrimary)
                .radius(t.sizes.radius)
                .border(1.0f, t.colors.borderDefault)
                .onClick([onImport = actions.onImportCurrent] { onImport(); })
                .build();
            components::button(ui, "claude.profiles.import.ccswitch")
                .position(20.0f + 2.0f * (btnW + btnGap),
                          bottomButtonRowY(bodyH))
                .size(btnW, 40.0f)
                .text("从 cc-switch 导入")
                .fontSize(t.sizes.fontSizeSmall)
                .colors(t.colors.surfaceElevated, t.colors.hover,
                        t.colors.active)
                .textColor(t.colors.textPrimary)
                .radius(t.sizes.radius)
                .border(1.0f, t.colors.borderDefault)
                .onClick([onImportCc = actions.onImportCcSwitch] {
                    if (onImportCc) onImportCc();
                })
                .build();
        })
        .build();
}

// 共享配置区块：标题行 + 片段卡片 +「+ 新建片段」。正常 / 空 / 错三路径都钉
// 面板底；块高由调用方按 sharedBlockH 传入（单一来源），块内元素一律相对
// 块顶从 0 起算（绝对位置由外层 stack 的 .position 决定）
void drawSharedConfigBlock(eui::Ui& ui, float width, float blockH,
                           const core::theme::Tokens& t,
                           const components::theme::ThemeColorTokens& themeTokens,
                           ProfileListState& st,
                           const std::vector<SharedConfig>& sharedConfigs,
                           const std::string& sharedConfigsError,
                           const ProfileListActions& actions) {
    ui.text("claude.shared.title")
        .position(0.0f, 0.0f)
        .size(width, 20.0f)
        .text("共享配置片段")
        .fontSize(13.0f)
        .lineHeight(20.0f)
        .fontWeight(600)
        .color(t.colors.textPrimary)
        .build();

    const float contentTop = kSharedHeaderBottom;
    const float contentH = std::max(0.0f, blockH - kSharedFixedH);
    // 片段集 / 空态 / 错误态任一变化 → 块内滚动偏移归零（签名跨数据集残留会
    // 跳回旧偏移）；签名计算放在分支之前，否则「错误 → 恢复」走不到归零
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
        ui.text("claude.shared.empty")
            .position(0.0f, contentTop).size(width, kSharedInfoH)
            .text("暂无片段，点击下方新建")
            .fontSize(11.0f).lineHeight(kSharedInfoH)
            .color(t.colors.textTertiary)
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
                    // 行容器 = 定高 stack：滚动内容是流式 column（条目按自身
                    // 高度排），行距由容器高多出的 kSharedCardGap 提供
                    inner.stack(cid + ".item")
                        .size(contentWidth, kSharedCardH + kSharedCardGap)
                        .content([&] {
                            inner.rect(cid + ".bg")
                                .position(0.0f, 0.0f)
                                .size(contentWidth, kSharedCardH)
                                .states(t.colors.surface, t.colors.hover,
                                        t.colors.active)
                                .radius(t.sizes.radiusLarge)
                                .border(1.0f, t.colors.borderDefault)
                                .onClick([onEdit = actions.requestEditShared, sc] {
                                    onEdit(sc);   // 按值拷贝（sc 引用 compose 局部副本，跨帧悬垂）
                                })
                                .build();
                            inner.text(cid + ".name")
                                .position(16.0f, 8.0f)
                                .size(contentWidth - 32.0f, 20.0f)
                                .text(sc.name)
                                .fontSize(13.0f).lineHeight(20.0f)
                                .fontWeight(600)
                                .color(t.colors.textPrimary)
                                .build();
                            inner.text(cid + ".keys")
                                .position(16.0f, 32.0f)
                                .size(contentWidth - 32.0f, 14.0f)
                                .text(sc.extraSettingsJson.empty()
                                          ? std::string("高级属性 JSON 预设（未配置）")
                                          : std::string("高级属性 JSON 预设 · 绑定它的 Profile 写入时覆盖同名键"))
                                .fontSize(11.0f).lineHeight(14.0f)
                                .color(t.colors.textSecondary)
                                .build();
                        })
                        .build();
                }
            })
            .build();
    }

    components::button(ui, "claude.shared.new")
        .position(0.0f, blockH - kSharedBtnH)
        .size(width, kSharedBtnH)
        .text("+ 新建片段")
        .fontSize(t.sizes.fontSizeSmall)
        .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
        .textColor(t.colors.textPrimary)
        .radius(t.sizes.radius)
        .border(1.0f, t.colors.borderDefault)
        .onClick([onNew = actions.onNewShared] { onNew(); })
        .build();
}

// Profile 右键菜单：动作经 ProfileListActions 委托到模块既有按钮同源逻辑，
// 本函数不写业务。常驻构建（关闭态由组件内 opacity 收起）：无右键目标时
// 强制置关开合标志再返回——目标与开合标志同生共灭，防「隐形菜单卡开」
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
    // 回调不捕获 self 指针（profiles 是 compose 局部副本，点击发生在下帧 →
    // 悬垂）：按值拷贝 Profile，actions 五个闭包一并按值拷入
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
