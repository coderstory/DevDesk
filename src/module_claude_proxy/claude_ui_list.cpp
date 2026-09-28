// src/module_claude_proxy/claude_ui_list.cpp
// R4（P-B4）：header / 引导条 / 左栏 Profile 列表 —— 逐字自 claude_module.cpp
// 的 compose 搬出（含注释），只把 this 成员换成显式入参与引用状态。
#include "claude_ui_list.h"

#include "claude_ui_logic.h"
#include "provider_config.h"
#include "settings_sync.h"

#include <components/button.h>
#include <components/contextmenu.h>
#include <components/virtuallist.h>

#include <core/router.h>
#include <core/ui_geometry.h>

#include <algorithm>

namespace module::claude {

namespace {

// ============ 共享配置区块的几何（批 2，设计 §2.10；单一来源，区块高度与
// 绘制共用同一组常量，供 drawProfileListPanel 撑滚动容器高 / 绘制用）============
constexpr float kSharedHeaderBottom = 30.0f;   // 标题行 + 下边距
constexpr float kSharedCardH = 56.0f;
constexpr float kSharedCardGap = 8.0f;
constexpr float kSharedBtnTopGap = 12.0f;
constexpr float kSharedBtnH = 34.0f;
constexpr float kSharedInfoH = 18.0f;          // 错误 / 空态行高

// 区块总高（与 drawSharedConfigBlock 的几何一致；滚动容器按此撑高）
float sharedBlockHeight(size_t snippetCount, bool hasError) {
    float h = kSharedHeaderBottom + kSharedBtnTopGap + kSharedBtnH;
    if (hasError || snippetCount == 0) {
        h += kSharedInfoH;
    } else {
        h += static_cast<float>(snippetCount) * (kSharedCardH + kSharedCardGap);
    }
    return h;
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
        // M2（2026-09-28，UI 现代化）：行 hover/选中颜色过渡——列表项基调
        // 0.1s OutQuad；选中/激活的 bg 与 border 同帧目标跳变也走动画
        // （Color 管 states 合成色，Border 管选中边框 1↔2px/色）
        .transition(core::Transition::make(0.1f, core::Ease::OutQuad))
        .animate(core::AnimProperty::Color | core::AnimProperty::Border)
        .radius(t.sizes.radiusLarge)
        .border(borderW, borderColor)
        .onClick([sp = &st, pid = p.id] { sp->selectedId_ = pid; })
        .build();

    // 头像（圆形 - 用 rect 模拟）
    ui.rect(id + ".avatar")
        .position(16.0f, y + 16.0f)
        .size(40.0f, 40.0f)
        .color(t.colors.surfaceElevated)
        .radius(20.0f)
        .border(1.0f, t.colors.borderDefault)
        .build();
    // 头像首字
    const std::string initial = p.name.empty()
        ? std::string("?")
        : std::string(1, p.name[0]);
    ui.text(id + ".avatar.text")
        .position(16.0f, y + 16.0f)
        .size(40.0f, 40.0f)
        .text(initial)
        .fontSize(16.0f)
        .lineHeight(40.0f)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .color(t.colors.textPrimary)
        .fontWeight(600)
        .build();

    // 名称（右缘 = ACTIVE 徽标左缘 width-60 前 4px；旧 width-130 把徽标
    // 探测区留得过宽，名称行被无谓截短 —— 2026-09-28 自适应父宽后改窄）
    // 徽标仅在激活时占位，超长文本仍会从盒尾溢出被徽标盖住（与 URL 同款）
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
        ui.text(id + ".active.text")
            .position(width - 60.0f, y + 16.0f)
            .size(48.0f, 20.0f)
            .text("ACTIVE")
            .fontSize(9.0f)
            .lineHeight(20.0f)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .color(t.colors.onPrimary)
            .fontWeight(700)
            .build();
    }

    // base-url (mono, 12px, tertiary)。宽 = width-64：右缘贴共享
    // 徽标左缘（width-60）前 4px；旧 width-80 偏保守、非绑定行白留
    //（2026-09-28 自适应父宽）
    ui.text(id + ".url")
        .position(64.0f, y + 44.0f)
        .size(width - 64.0f, 14.0f)
        .text(p.baseUrl)
        .fontSize(11.0f)
        .lineHeight(14.0f)
        .color(t.colors.textTertiary)
        .build();

    // 批 2（设计 §2.10「详情/列表」）：绑定了共享配置片段的卡片角标「共享」
    //（服务层保证无悬空 id：删片段同事务解绑 → sharedConfigId != 0 即可判定；
    // 画在 URL 行右侧、ACTIVE 徽标正下方，长 URL 溢出时徽标盖住尾部）
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
            .color(t.colors.textTertiary)
            .build();
    }

    // model（default 槽位，未配则旧 model 列 fallback——§6.2 列表卡片）。
    // 宽 = width-64（同 name/url；model 行下方无徽标，右缘留 16px 边距）
    ui.text(id + ".model")
        .position(64.0f, y + 62.0f)
        .size(width - 64.0f, 14.0f)
        .text(std::string("model: ") +
              effectiveModelsConfig(p.modelsConfigJson, p.model).def.model)
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
    // 审计 M6：header 右侧操作区宽（标题列 = contentW - 本值）——header 行
    // gap=0，两段必须相加等于 contentW；字面量写死两处改一处忘一处就右缘错位
    constexpr float kHeaderActionsW = 360.0f;

    // ========== 顶部 Header ==========
    ui.row("claude.header")
        .size(contentW, headerH)
        .alignItems(eui::Align::CENTER)
        .content([&] {
            // 左侧标题 + 状态
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

                            // 状态指示器（真实状态）
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

            // 右侧操作
            ui.row("claude.actions")
                .size(kHeaderActionsW, 40.0f)
                .justifyContent(eui::Align::END)
                .alignItems(eui::Align::CENTER)
                .gap(t.sizes.spacingSmall)
                .content([&] {
                    // 停止/启动代理（真实开关）
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
    // 引导条（§6.4）：代理未运行且本会话从未启动过时一次性提示接管语义
    //（ⓘ 换 FA：Icon 字体不能与正文混排，图标/文字拆两个元素）
    ui.row("claude.guide.wrap")
        .size(contentW, 18.0f)
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
    // ---- 左栏：Profile 列表 ----
    ui.column("claude.profiles")
        .size(leftW, bodyH)
        .gap(0.0f)
        .content([&] {
            ui.rect("claude.profiles.bg")
                .size(leftW, bodyH)
                // 左栏 Profile 列表 = 结构性面板 → background
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
            // 顶部栏
            ui.text("claude.profiles.title")
                .position(20.0f, 20.0f)
                .size(leftW - 40.0f, 20.0f)
                .text("Profile 列表")
                .fontSize(t.sizes.fontSizeBody)
                .lineHeight(20.0f)
                .fontWeight(600)
                .color(t.colors.textPrimary)
                .build();

            // 卡片列表（真实数据；空时显示占位）
            const float cardY0 = 56.0f;
            const float cardGap = 12.0f;
            if (!profilesError.empty()) {
                ui.text("claude.profiles.error")
                    .position(20.0f, cardY0 + 20.0f)
                    .size(leftW - 40.0f, 60.0f)
                    .text("Profile 列表加载失败：" + profilesError)
                    .fontSize(12.0f)
                    .lineHeight(20.0f)
                    .color(t.colors.error)
                    .build();
                // 批 2：无 scroll 时共享区块垫在错误态下方（绝对坐标；
                // stack 定位对齐 overlay 的 20px 内缩，与 Profile 标题同行列）
                ui.stack("claude.shared.block")
                    .position(20.0f, cardY0 + 96.0f)
                    .size(leftW - 40.0f,
                          sharedBlockHeight(sharedConfigs.size(),
                                            !sharedConfigsError.empty()))
                    .content([&] {
                        drawSharedConfigBlock(ui, 0.0f, leftW - 40.0f, t,
                                              sharedConfigs,
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
                // 批 2：无 scroll 时共享区块垫在空态下方（绝对坐标）
                ui.stack("claude.shared.block")
                    .position(20.0f, cardY0 + 96.0f)
                    .size(leftW - 40.0f,
                          sharedBlockHeight(sharedConfigs.size(),
                                            !sharedConfigsError.empty()))
                    .content([&] {
                        drawSharedConfigBlock(ui, 0.0f, leftW - 40.0f, t,
                                              sharedConfigs,
                                              sharedConfigsError, actions);
                    })
                    .build();
            } else {
                // 卡片列表套 virtualList（UI 现代化 M3-2）：手绘平铺换
                // EUI virtualList——行容器换 card 组件（彩色卡片区）且仅
                // 可见行进 slot 窗口重建。行距保留旧观感：行内容 96 +
                // cardGap 间距 = 108 pitch。滚动语义 profilesScrollOffset_
                // 沿用（翻页式列表行数小，overscan 一个视口高让整段渲染，
                // 消除 slot 数量感）。滚动区底 = 底部按钮（bodyH-64）
                // 上方 12px（同旧 scrollView 口径）。
                // 2026-09-28：视口对齐父面板 20px 内缩网格（position 20 /
                // 宽 leftW-40 = 父内容宽），item 自适应父宽；旧 x=0/leftW-24
                // 与标题/按钮的 20px 网格错位且右缘差 4px
                const float scrollH = std::max(
                    0.0f, bodyH - 64.0f - 12.0f - cardY0);
                const float rowPitch = 96.0f + cardGap;
                components::virtualList(ui, "claude.profiles.scroll")
                    .position(20.0f, cardY0)
                    .size(leftW - 40.0f, scrollH)
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
                        // 行容器 = 裸 stack（2026-09-28 去掉 card 组件
                        // 骨架）：card（CardStyle）自带 surface 底 +
                        // panelShadow 阴影，与卡内交互层 .bg（drawProfileCard
                        // 的 theme 底 + hover/选中 states）叠成「白底 + 右下
                        // 偏移阴影」双底色（用户反馈）。只保留 .bg 作唯一底
                        //（走 theme：surface / primarySoftBg + states）。
                        // 事件挂点（右键 wrap / 点击 .bg）原样保留在卡内
                        // 交互层；卡装饰（头像/名称/ACTIVE 徽标）随
                        // drawProfileCard 进 content。
                        const std::string cardId =
                            "claude.profile." + std::to_string(idx);
                        inner.stack(rowId + ".card")
                            .size(contentWidth, 96.0f)
                            .content([&] {
                                // 右键菜单入口（UI 现代化 M1-5）：记录目标
                                // profile + 锚点 = PointerEvent 原始坐标经
                                // core::menuAnchorForPointer 统一换算（页面
                                // 坐标；onContextMenu 通道已 /dpiScale →
                                // dpiScale=1.0）。滚动偏移传 0——指针即视觉
                                // 位置，滚动下锚点天然正确。
                                // st 为引用：写回存活到下一帧的模块状态
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
                // 批 2：共享配置区块（master 合并保留，设计 §2.10「片段列表
                // 入口」）。virtualList 行窗为等高槽位、无非等高末行支持 →
                // 区块不并入行窗，垫在列表下方作粘性脚注（几何同上方空/错态
                // 分支：absolute 定位，块内元素相对块顶）。列表高度让出区块高
                // + 12px 间距（至少保 96px）；片段过多时区块向下溢出盖过底部
                // 按钮区——与空/错态分支同款行为
                const float sharedH =
                    sharedBlockHeight(sharedConfigs.size(),
                                      !sharedConfigsError.empty());
                const float listH =
                    std::clamp(scrollH - sharedH - 12.0f,
                               std::min(96.0f, scrollH), scrollH);
                ui.stack("claude.shared.block")
                    .position(20.0f, cardY0 + listH + 12.0f)
                    .size(leftW - 40.0f, sharedH)
                    .content([&] {
                        drawSharedConfigBlock(ui, 0.0f, leftW - 40.0f, t,
                                              sharedConfigs,
                                              sharedConfigsError, actions);
                    })
                    .build();
            }

            // 底部按钮：新建 + 导入当前配置（读
            // ~/.claude/settings.json 预填新建表单）
            const float btnW = (leftW - 40.0f - 12.0f) * 0.5f;
            components::button(ui, "claude.profiles.new")
                .position(20.0f, bodyH - 64.0f)
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
                .position(20.0f + btnW + 12.0f, bodyH - 64.0f)
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
        })
        .build();
}

// ============ 共享配置区块（批 2，设计 §2.10「片段列表入口」）============
// 标题行 + 片段卡片（name + N 个键）+「+ 新建片段」按钮。drawProfileListPanel
// 三路径调用（合并后）：
//   - profiles 正常：垫在 virtualList 下方作粘性脚注（y = 绝对坐标；列表高度
//     让出区块高，见 else 分支）
//   - profiles 空/错（无 scroll）：垫在空态/错误态下方（y = 绝对坐标）
// 几何常量与 sharedBlockHeight 见文件顶部匿名命名空间（单一来源）。
float drawSharedConfigBlock(eui::Ui& ui, float y, float width,
                            const core::theme::Tokens& t,
                            const std::vector<SharedConfig>& sharedConfigs,
                            const std::string& sharedConfigsError,
                            const ProfileListActions& actions) {
    // 标题行（与 Profile 列表标题同款字重，字号略小一档以示从属）
    ui.text("claude.shared.title")
        .position(0.0f, y)
        .size(width, 20.0f)
        .text("共享配置片段")
        .fontSize(13.0f)
        .lineHeight(20.0f)
        .fontWeight(600)
        .color(t.colors.textPrimary)
        .build();

    float cy = y + kSharedHeaderBottom;
    if (!sharedConfigsError.empty()) {
        ui.text("claude.shared.error")
            .position(0.0f, cy).size(width, kSharedInfoH)
            .text("共享配置片段加载失败：" + sharedConfigsError)
            .fontSize(11.0f).lineHeight(kSharedInfoH)
            .color(t.colors.error)
            .build();
        cy += kSharedInfoH;
    } else if (sharedConfigs.empty()) {
        ui.text("claude.shared.empty")
            .position(0.0f, cy).size(width, kSharedInfoH)
            .text("暂无片段，点击下方新建")
            .fontSize(11.0f).lineHeight(kSharedInfoH)
            .color(t.colors.textTertiary)
            .build();
        cy += kSharedInfoH;
    } else {
        // 片段卡片：name + 键数（K 键集大小即「N 个键」）；点击开编辑弹窗
        for (size_t i = 0; i < sharedConfigs.size(); ++i) {
            const SharedConfig& sc = sharedConfigs[i];
            const std::string cid = "claude.shared.card." + std::to_string(i);
            ui.rect(cid + ".bg")
                .position(0.0f, cy).size(width, kSharedCardH)
                .states(t.colors.surface, t.colors.hover, t.colors.active)
                .radius(t.sizes.radiusLarge)
                .border(1.0f, t.colors.borderDefault)
                .onClick([onEdit = actions.requestEditShared, sc] {
                    onEdit(sc);   // 按值拷贝（sc 引用 compose 局部副本，跨帧悬垂）
                })
                .build();
            ui.text(cid + ".name")
                .position(16.0f, cy + 8.0f).size(width - 32.0f, 20.0f)
                .text(sc.name)
                .fontSize(13.0f).lineHeight(20.0f)
                .fontWeight(600)
                .color(t.colors.textPrimary)
                .build();
            ui.text(cid + ".keys")
                .position(16.0f, cy + 32.0f).size(width - 32.0f, 14.0f)
                .text(std::to_string(sharedFrozenKeys(sc).size()) +
                      " 个键 · 绑定它的 Profile 冻结这些键")
                .fontSize(11.0f).lineHeight(14.0f)
                .color(t.colors.textSecondary)
                .build();
            cy += kSharedCardH + kSharedCardGap;
        }
    }
    components::button(ui, "claude.shared.new")
        .position(0.0f, cy + kSharedBtnTopGap)
        .size(width, kSharedBtnH)
        .text("+ 新建片段")
        .fontSize(t.sizes.fontSizeSmall)
        .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
        .textColor(t.colors.textPrimary)
        .radius(t.sizes.radius)
        .border(1.0f, t.colors.borderDefault)
        .onClick([onNew = actions.onNewShared] { onNew(); })
        .build();
    return cy + kSharedBtnTopGap + kSharedBtnH;
}

// profile 列表右键菜单（UI 现代化 §3.4 / M1-5）：
// 编辑 / 复制 / 删除 / 设为当前 —— 动作经 ProfileListActions 委托到
// claude_module 既有按钮同源逻辑（openEditForm / doDuplicate / 删除确认 /
// doSetActive），本函数不写业务逻辑。
// 常驻构建（关闭态由组件内 opacity 收起，同 todo/md 右键模式）：
// 无右键目标时强制置关开合标志再返回（4523e09 的「隐形菜单卡开」同款守卫
// —— 目标与开合标志同生共灭），目标存在但菜单关闭时 normal 渲染。
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
        {"编辑"}, {"复制"}, {"删除"}, {"设为当前"}};
    // 选中回调不捕获 self 指针（profiles 是 compose 局部副本，点击发生在
    // 下帧 → 悬垂）；按值拷一份 Profile 再派发（照 claude_ui_detail 的
    // shownCopy 手法）。actions 是 compose 传参（listActions() 临时值）→
    // 四个动作闭包一并按值拷入（std::function 拷贝安全，同既有按钮 onClick
    // 的 [onNew = actions.onNewProfile] 捕获形态）。
    components::contextMenu(ui, "claude.list.ctx")
        .open(st.ctxMenuOpen_)
        .screen(screen.width, screen.height)
        .position(st.ctxMenuX_, st.ctxMenuY_)
        .autoWidth()   // M4-2 已合入（上游 7164d55）
        .items(items)
        .theme(themeTokens)
        .onSelect([&st,
                   onEdit = actions.requestEdit,
                   onDup = actions.requestDuplicate,
                   onDel = actions.requestDelete,
                   onSet = actions.requestSetActive,
                   p = *self](int index) {
            st.ctxMenuOpen_ = false;
            switch (index) {
                case 0: onEdit(p); break;
                case 1: onDup(p.id); break;
                case 2: onDel(p.id, p.name); break;
                case 3: onSet(p.id); break;
                default: break;
            }
        })
        .onOpenChange([&st](bool v) { st.ctxMenuOpen_ = v; })
        .build();
}

} // namespace module::claude
