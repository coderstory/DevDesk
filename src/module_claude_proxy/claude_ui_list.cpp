// src/module_claude_proxy/claude_ui_list.cpp
// R4（P-B4）：header / 引导条 / 左栏 Profile 列表 —— 逐字自 claude_module.cpp
// 的 compose 搬出（含注释），只把 this 成员换成显式入参与引用状态。
#include "claude_ui_list.h"

#include "provider_config.h"
#include "settings_sync.h"

#include <components/button.h>
#include <components/scrollview.h>

#include <algorithm>

namespace module::claude {

namespace {

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

    // 名称
    ui.text(id + ".name")
        .position(64.0f, y + 18.0f)
        .size(width - 130.0f, 20.0f)
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

    // base-url (mono, 12px, tertiary)
    ui.text(id + ".url")
        .position(64.0f, y + 44.0f)
        .size(width - 80.0f, 14.0f)
        .text(p.baseUrl)
        .fontSize(11.0f)
        .lineHeight(14.0f)
        .color(t.colors.textTertiary)
        .build();

    // model（default 槽位，未配则旧 model 列 fallback——§6.2 列表卡片）
    ui.text(id + ".model")
        .position(64.0f, y + 62.0f)
        .size(width - 80.0f, 14.0f)
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
            } else if (profiles.empty()) {
                ui.text("claude.profiles.empty")
                    .position(20.0f, cardY0 + 20.0f)
                    .size(leftW - 40.0f, 60.0f)
                    .text("暂无 Profile，点击下方按钮新建")
                    .fontSize(12.0f)
                    .lineHeight(20.0f)
                    .color(t.colors.textTertiary)
                    .build();
            } else {
                // 卡片列表套 scrollView（产品审计 2026-09-23
                // [26]）：此前固定 y=56+i*108 平铺，profile 数
                // 一多就画出左栏边界、叠在底部按钮上且无法滚
                // 动。仿 Todo 列内滚动 / settings 卡片区。
                // 滚动区底 = 底部按钮（bodyH-64）上方 12px
                const float scrollH = std::max(
                    0.0f, bodyH - 64.0f - 12.0f - cardY0);
                // contentKey = profile id 序列 hash（仿 md.tree，
                // W7 教训：数量相同但内容替换也要失效重测）
                size_t listKey = 0;
                for (const auto& p : profiles) {
                    listKey = listKey * 1315423911u +
                              static_cast<size_t>(p.id);
                }
                components::scrollView(ui, "claude.profiles.scroll")
                    .position(0.0f, cardY0)
                    .size(leftW - 24.0f, scrollH)
                    .gap(cardGap)
                    .step(96.0f + cardGap)
                    .theme(themeTokens)
                    .contentKey("claude.profiles.scroll." +
                                std::to_string(listKey))
                    .offset(st.profilesScrollOffset_)
                    .onChange([sp = &st](float v) {
                        sp->profilesScrollOffset_ = v;
                    })
                    .content([&](eui::Ui& inner, float contentWidth,
                                 float viewportHeight) {
                        (void)viewportHeight;
                        for (size_t i = 0; i < profiles.size(); ++i) {
                            const std::string cardId =
                                "claude.profile." + std::to_string(i);
                            // 行包 stack（position 只在 stack 内
                            // 生效，仿 todo 列卡片）
                            inner.stack(cardId + ".wrap")
                                .size(contentWidth, 96.0f)
                                .content([&] {
                                    drawProfileCard(
                                        inner, cardId, 0.0f,
                                        contentWidth, t, profiles[i], st);
                                })
                                .build();
                        }
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

} // namespace module::claude
