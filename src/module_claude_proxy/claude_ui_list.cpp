// src/module_claude_proxy/claude_ui_list.cpp
// R4（P-B4）：header / 引导条 / 左栏 Profile 列表 —— 逐字自 claude_module.cpp
// 的 compose 搬出（含注释），只把 this 成员换成显式入参与引用状态。
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
#include <functional>

namespace module::claude {

namespace {

// ============ 共享配置区块的几何（批 2，设计 §2.10；单一来源，区块高度与
// 绘制共用同一组常量，供 drawProfileListPanel 撑滚动容器高 / 绘制用）============
// 2026-09-28（macOS 布局统一方案 §1 组 B，P4）——区块**钉面板底**。
// 用户原话：「新建片段的按钮原本底部对齐的，现在没了」。
// **口径（2026-09-29 审计校正，别写成「恢复原样」）**：用户记忆里的「原本贴底」
// 在代码史上**不成立** —— 改前形态（a0f9c32 起）是「Profile 列表滚动内容尾部」，
// 只有列表填满视口并滚到底时才在底部，空/错态分支更是写死 `cardY0+96` 的绝对
// 坐标；9811225（merge「master 合入 ui/rest」的冲突消解）又把它改成「绝对定位
// 在列表下方」的脚注，高度让位式 `listH = clamp(scrollH − sharedH − 12,
// min(96, scrollH), scrollH)` 只在 `sharedH ≤ scrollH − 108` 时（≤4 个片段）
// 碰巧把它顶到底；96px 保底一生效区块就被推下去：≥6 个时「+ 新建片段」被底部
// 按钮压住点不到，≥7 个落到面板外，≥8 个被 app.page clip。
// 所以**钉底是本次的新设计**（修复 ≥5 片段起的可达性回归），不是回退到某个旧形态。
// 现在改为**锚定**（统一原则：容器可用高只有一个来源 = bodyH；贴底元素锚容器底）：
//   区块底 = bodyH − 底部按钮行高 − kSharedBottomGap（锚点，不随片段数变）
//   区块高 = min(自然高, 不侵占列表保底视口的上界)
//   列表视口 = 区块顶 − 间距 − 列表起点（吃剩余高）
// 片段多到装不下时在**区块内部滚**（卡片仍然全部可达可编辑），「+ 新建片段」是
// 区块的固定尾部 → 恒可见可点（用户报过的两个症状一起消掉）。
constexpr float kSharedHeaderBottom = 30.0f;   // 标题行 + 下边距
constexpr float kSharedCardH = 56.0f;
constexpr float kSharedCardGap = 8.0f;
constexpr float kSharedBtnTopGap = 12.0f;
constexpr float kSharedBtnH = 34.0f;
constexpr float kSharedInfoH = 18.0f;          // 错误 / 空态行高
constexpr float kSharedBottomGap = 12.0f;      // 区块底 → 底部按钮行顶
constexpr float kSharedListMinH = 96.0f;       // 列表视口保底高（旧口径不变）
constexpr float kListTopY = 56.0f;             // 列表 / 区块所在列的起点 y
constexpr float kBottomBtnRowH = 64.0f;        // 底部按钮行（新建/导入）预留高

// 区块固定部分（与 drawSharedConfigBlock 的绘制几何一致）：标题行 + 按钮上间距
// + 按钮高 —— 卡片区 / 提示行占用剩下的高
constexpr float kSharedFixedH =
    kSharedHeaderBottom + kSharedBtnTopGap + kSharedBtnH;

// 底部按钮行上沿（面板内绝对 y）
float bottomButtonRowY(float bodyH) { return bodyH - kBottomBtnRowH; }

// 区块**锚点**：贴底时区块底沿的绝对 y（= 底部按钮行上沿 − 间距）
float sharedBlockBottomY(float bodyH) {
    return bottomButtonRowY(bodyH) - kSharedBottomGap;
}

// 区块可见高上界：区块顶不得高于「列表保底视口之下」——列表视口吃剩余高，
// 故上界 = 区块底 − 列表起点 − 列表保底 − 间距
float sharedBlockMaxH(float bodyH) {
    return std::max(0.0f, sharedBlockBottomY(bodyH) - kListTopY -
                              kSharedListMinH - kSharedBottomGap);
}

// 区块自然高（内容全展开时）：固定部分 + 卡片区（或错误/空态行）
float sharedBlockNaturalH(size_t snippetCount, bool hasError) {
    float h = kSharedFixedH;
    if (hasError || snippetCount == 0) {
        h += kSharedInfoH;
    } else {
        h += static_cast<float>(snippetCount) * (kSharedCardH + kSharedCardGap);
    }
    return h;
}

// 区块实际高：自然高与上界取小（取到上界 = 卡片区要滚）
float sharedBlockH(float bodyH, size_t snippetCount, bool hasError) {
    return std::min(sharedBlockNaturalH(snippetCount, hasError),
                    sharedBlockMaxH(bodyH));
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
        // 真机反馈 ⑧（2026-09-29）：胶囊里的「ACTIVE」没垂直居中 —— 文字框与
        // 胶囊同为 20 高、同 y，差的是**文本没做垂直居中**：runtime 只在
        // verticalAlign=Center 时把 ink 中心对齐到 frame 中心（EUI-NEO
        // runtime_render.h renderText），否则 ink 从行框顶沿起排，胶囊里看着
        // 就偏上。只加对齐，胶囊尺寸/位置/配色与字号行高一律不动。
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
            // 同一胶囊缺陷一并修（⑧ 的同类项：文字框与胶囊同 y 同高，缺的是
            // 垂直居中）——两个胶囊徽标的文字口径保持一致
            .verticalAlign(eui::VerticalAlign::Center)
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
            const float cardY0 = kListTopY;
            const float cardGap = 12.0f;

            // 共享配置区块（P4）：**钉面板底** —— 底沿 = 底部按钮行上沿 − 间距
            // （锚点，不随片段数变），高 = min(自然高, 不侵占列表保底视口)，
            // 顶 = 底 − 高。列表视口吃剩余高（见下方 listH）。
            // 退化守门（审计 F-06 改写）：旧写法 `assert(sharedBlockTop + sharedH
            // <= bodyH)` 是**身份式恒真** —— blockTop 按定义 = 底 − sharedH，左边
            // 恒等于 bodyH − 76，永不可能触发，零保护力。真正的退化有两种，都不
            // 能从定义推出，故留这两条 + 下面的 listH 保底检查：
            //   ① 区块被压扁到 < 固定部分 + 提示行（kSharedMinH=94）→ 标题行 /
            //      空态行与「+ 新建片段」重叠；触发域 bodyH < 334（客户区 < 528）
            //   ② 列表保底视口被挤没（listH < kSharedListMinH）；触发域 bodyH < 240
            // 两者都是「高度预算不足」，按统一原则必须**可见**：记 ERROR（不做
            // assert/abort —— 退化域低于支持的最小窗，GUI 不该为此崩，且 Release
            // 会把 assert 编掉、问题在两种构建下同样该现形）。
            const bool sharedHasError = !sharedConfigsError.empty();
            const float sharedH = sharedBlockH(bodyH, sharedConfigs.size(),
                                               sharedHasError);
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
                // 批 2：无 scroll 时共享区块同样钉面板底（P4 前这里写死
                // cardY0+96 —— 空/错态下区块根本不贴底，是用户「按钮飘在
                // 中间」的另一条路径）
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
                // 批 2：无 scroll 时共享区块同样钉面板底（P4，见上一分支注释）
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
                // 卡片列表套 virtualList（UI 现代化 M3-2）：手绘平铺换
                // EUI virtualList——行容器换 card 组件（彩色卡片区）且仅
                // 可见行进 slot 窗口重建。行距保留旧观感：行内容 96 +
                // cardGap 间距 = 108 pitch。滚动语义 profilesScrollOffset_
                // 沿用（翻页式列表行数小，overscan 一个视口高让整段渲染，
                // 消除 slot 数量感）。
                // 2026-09-28：视口对齐父面板 20px 内缩网格（position 20 /
                // 宽 leftW-40 = 父内容宽），item 自适应父宽；旧 x=0/leftW-24
                // 与标题/按钮的 20px 网格错位且右缘差 4px
                // 跨模块剩余审计修（2026-09-28）：视口高度让出共享区块高 +
                // 12px 间距——区块由此整体钉在视口 clip 之外；修复前视口按全高、
                // 区块只压自己的绝对 Y，后建区块叠在带 clip 的视口底带上，
                // ≥5 profile 时末 1-2 张卡被盖、滚到底显示不全、点击误开片段编辑
                // P4（2026-09-28）：**从「让位式 clamp」改成「锚定式推导」**——
                // 视口 = 区块顶 − 间距 − 列表起点（区块顶本身由面板底锚定推出），
                // 于是列表恒吃到「面板底 − 区块高」的全部剩余高，区块高又已被
                // sharedBlockMaxH 限死在不侵占列表保底视口之内 → 旧式那个
                // `clamp(x, min(96, scrollH), scrollH)` 的 96px 保底不再需要
                //（它的作用正是「列表至少要 96」，现在由区块高的上界保证）
                const float rowPitch = 96.0f + cardGap;
                const float listH = std::max(
                    0.0f, sharedBlockTop - kSharedBottomGap - cardY0);
                // 退化守门 ②（审计 F-06）：列表保底视口被挤没 —— 区块高的上界
                // 本应保证 listH ≥ kSharedListMinH，只有 bodyH 极小（< 240，
                // sharedBlockMaxH 被 max(0,·) 夹到 0）时才破，届时必须可见
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
                // 区块不并入行窗，垫在列表下方。P4 起**钉面板底**（位置/高度
                // 见本函数开头的量）：列表视口高已让出区块高（listH），区块整体
                // 位于视口 clip 之外（间距 12px），末卡滚到底也不会被区块盖住/
                // 误点；片段装不下时在区块内部滚，「+ 新建片段」恒可见可点
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

            // 底部按钮：新建 + 导入当前配置（读
            // ~/.claude/settings.json 预填新建表单）
            const float btnW = (leftW - 40.0f - 12.0f) * 0.5f;
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
                .position(20.0f + btnW + 12.0f, bottomButtonRowY(bodyH))
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
//   - profiles 正常：垫在 virtualList 下方，区块底**钉在面板内容底**，列表
//     视口吃剩余高（见 drawProfileListPanel 的 listH）
//   - profiles 空/错（无 scroll）：同样钉底（占位不再写死 y）
// 高度由调用方按 sharedBlockH 算出并传入（**单一来源**：调用方要知道它才能排
// 列表视口）；块内元素一律**相对块顶**从 0 起算（审计 F-08：此前有个恒传 0 的
// `y` 形参、文档还写成「面板内绝对坐标」，与实现相反 —— 块的绝对位置由外层
// `claude.shared.block` stack 的 .position 决定，块函数不该再持一个偏移）。
// 卡片装不下时在块内滚（「+ 新建片段」是固定尾部，恒可见可点）。
// 几何常量与高度算式见文件顶部匿名命名空间。
void drawSharedConfigBlock(eui::Ui& ui, float width, float blockH,
                           const core::theme::Tokens& t,
                           const components::theme::ThemeColorTokens& themeTokens,
                           ProfileListState& st,
                           const std::vector<SharedConfig>& sharedConfigs,
                           const std::string& sharedConfigsError,
                           const ProfileListActions& actions) {
    // 标题行（与 Profile 列表标题同款字重，字号略小一档以示从属）
    ui.text("claude.shared.title")
        .position(0.0f, 0.0f)
        .size(width, 20.0f)
        .text("共享配置片段")
        .fontSize(13.0f)
        .lineHeight(20.0f)
        .fontWeight(600)
        .color(t.colors.textPrimary)
        .build();

    // 内容区：上顶 = 标题行底，下底 = 按钮行上沿（= blockH − 按钮高 − 间距）
    const float contentTop = kSharedHeaderBottom;
    const float contentH = std::max(0.0f, blockH - kSharedFixedH);
    // 片段集签名（**含空态与错误态**）：片段集 / 空态 / 错误态任一变化 → 块内
    // 滚动偏移**归零**（审计 F-09）。contentKey 只触发重新测高、不动偏移，不归零
    // 时「滚到底 → 删到 1 个 → 再加回 8 个」会跳回旧偏移（渲染侧每帧 clamp 兜住了
    // 「不会画错」，但状态跨数据集残留）。签名放在分支之前：否则「错误 → 恢复」
    // 这条路径根本走不到归零代码
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
        // 片段卡片：name + 键数（K 键集大小即「N 个键」）；点击开编辑弹窗。
        // 装不下时滚动区生效（块内滚，区块整体仍钉面板底）
        components::scrollView(ui, "claude.shared.scroll")
            .position(0.0f, contentTop)
            .size(width, contentH)
            .gap(0.0f)
            .step(kSharedCardH + kSharedCardGap)
            .theme(themeTokens)
            // contentKey：片段集变了就重测内容高（沿用 md/settings 口径）
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
                    // 高度顺序排，不能靠绝对 y），行距由容器高度多出的
                    // kSharedCardGap 提供
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
                                .text(std::to_string(sharedFrozenKeys(sc).size()) +
                                      " 个键 · 绑定它的 Profile 冻结这些键")
                                .fontSize(11.0f).lineHeight(14.0f)
                                .color(t.colors.textSecondary)
                                .build();
                        })
                        .build();
                }
            })
            .build();
    }

    // 「+ 新建片段」= 区块的**固定尾部**（贴区块底 = 贴面板内容底）：片段再多
    // 也压不住它（块内滚吃掉了多出来的高度）
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

// profile 列表右键菜单（UI 现代化 §3.4 / M1-5；2026-09-30 batch-fix C 起
// 五项：编辑 / 建副本 / 复制配置 / 删除 / 设为当前）——动作经
// ProfileListActions 委托到 claude_module 既有按钮同源逻辑（openEditForm /
// doDuplicate / doCopyConfig / 删除确认 / doSetActive），本函数不写业务逻辑。
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
        {"编辑"}, {"建副本"}, {"复制配置"}, {"删除"}, {"设为当前"}};
    // 选中回调不捕获 self 指针（profiles 是 compose 局部副本，点击发生在
    // 下帧 → 悬垂）；按值拷一份 Profile 再派发（照 claude_ui_detail 的
    // shownCopy 手法）。actions 是 compose 传参（listActions() 临时值）→
    // 五个动作闭包一并按值拷入（std::function 拷贝安全，同既有按钮 onClick
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
                   onCopyCfg = actions.requestCopyConfig,
                   onDel = actions.requestDelete,
                   onSet = actions.requestSetActive,
                   p = *self](int index) {
            st.ctxMenuOpen_ = false;
            switch (index) {
                case 0: onEdit(p); break;
                case 1: onDup(p.id); break;
                case 2: onCopyCfg(p); break;   // batch-fix C：复制配置
                case 3: onDel(p.id, p.name); break;
                case 4: onSet(p.id); break;
                default: break;
            }
        })
        .onOpenChange([&st](bool v) { st.ctxMenuOpen_ = v; })
        .build();
}

} // namespace module::claude
