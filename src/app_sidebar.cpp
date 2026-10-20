// 常驻侧边栏 + 折叠/展开实现（接口见 app_sidebar.h）。折叠态：宽 208→72，只显居中图标，
// label 不渲染；动画为瞬时切换（EUI-NEO AnimProperty 无 Width 档）。
#include "app_sidebar.h"

#include "core/component_theme.h"
#include "core/database.h"
#include "core/logging.h"
#include "core/router.h"
#include "core/theme.h"
#include "core/window_effect_state.h"  // 侧栏底 alpha 按窗口效果档位（UAT2 B 批）
#include "module_agent/agent_module.h"
#include "settings_config.h"
#include <ui_kit/metrics.h>
#include <ui_kit/selection_bar.h>  // W3：SelectionBar 选中态 helper（§4.3）
#include <ui_kit/splash.h>
#include <ui_kit/toast.h>

#include <eui/types.h>

#include <functional>
#include <optional>
#include <utility>

namespace app {

namespace {

// 进程内折叠状态单值；默认展开，启动恢复由 main.cpp 调 setSidebarCollapsed（不回写）
bool g_sidebarCollapsed = false;

// 应用级 toast 通知：托盘失效入口点击的解释反馈。调用方写状态，compose 顶层每帧读状态绘制
ui_kit::ToastState g_appNotice;

// 自绘标题栏布局开关的测试钉住缝（nullopt = 跟平台能力走，见 customTitlebarActive）
std::optional<bool> g_titlebarModeOverride;

// 关闭钮 hover 态：EUI-NEO .onHover 回调写、compose 读。红底需要图标色随 hover 翻白，
// 而 .states() 只能给 rect 上色——关闭叉是独立 text 元素，色值须在此跨帧传递
bool g_closeButtonHovered = false;

} // namespace

bool sidebarCollapsed() { return g_sidebarCollapsed; }

void setSidebarCollapsed(bool collapsed, bool persist) {
    g_sidebarCollapsed = collapsed;
    if (!persist) return;
    core::data::SqliteDb* db = ::core::appDatabase();
    if (db == nullptr) {
        LOG_WARN("侧边栏折叠状态未持久化（应用数据库未就绪，不影响本次会话）");
        return;
    }
    // 持久化：load → 改单字段 → save（save 逐行写、其余行原样保留）；失败只记日志不回滚内存态
    const module::settings::SettingsStore store(db);
    auto cfg = store.load();
    cfg.sidebarCollapsed = collapsed;
    if (store.save(cfg).isFail()) {
        LOG_WARN("侧边栏折叠状态持久化失败（不影响本次会话）");
    }
}

float sidebarWidth() {
    return g_sidebarCollapsed ? kSidebarWidthCollapsed : kSidebarWidthExpanded;
}

// 测试缝：折叠状态回厂（展开），纯内存不触发持久化副作用
void resetSidebarGlobals() { g_sidebarCollapsed = false; }

void showAppNotice(const std::string& title, const std::string& message) {
    g_appNotice.show(title, message, false);
}

void clearAppNotice() {
    g_appNotice.dismiss();
    g_appNotice.title.clear();
    g_appNotice.body.clear();
}

bool appNoticeVisible() { return g_appNotice.open; }
// 无通知时一律空串：open=false 时内容必已清空
std::string appNoticeTitle() { return g_appNotice.open ? g_appNotice.title : std::string{}; }
std::string appNoticeMessage() { return g_appNotice.open ? g_appNotice.body : std::string{}; }

float pageContentWidth(float windowWidth) {
    return windowWidth - sidebarWidth();
}

// ---- 自绘标题栏（UAT3 D-R3-1）----

float titlebarHeight() {
    return 40.0f;
}

bool customTitlebarActive() {
    if (g_titlebarModeOverride.has_value()) {
        return *g_titlebarModeOverride;
    }
    return customWindowChromeSupported();
}

void setCustomTitlebarMode(std::optional<bool> mode) {
    g_titlebarModeOverride = std::move(mode);
}

// 窗口钮热区：右缘三钮（关/最大化/最小），每钮 46×标题条高
constexpr float kWindowButtonW = 46.0f;

bool titlebarCaptionHit(const CaptionHitQuery& query) {
    // 拖拽区 = 标题条带内、右缘三钮热区左侧之外（logo/品牌文本区可拖）。
    // 标题条贴边处的八向 resize 热区在 EUI-NEO 层优先于本回调，此处不管边缘
    if (!customTitlebarActive()) {
        return false;
    }
    if (query.y < 0.0f || query.y >= titlebarHeight()) {
        return false;
    }
    if (query.x < 0.0f || query.x > query.width) {
        return false;
    }
    return query.x < query.width - kWindowButtonW * 3.0f;
}

// 自绘标题条：左 logo+品牌文本、右三钮；不铺底色——半透/磨砂的 clearColor +
// DWM backdrop 直接透到标题条区（D-R3-1 的落点：undecorated 后窗口效果铺满全窗）。
// wechat 风格：灰字灰 hover；关闭钮 hover 红（theme warningRed #FA5151）+ 白叉
void drawTitlebar(eui::Ui& ui, const eui::Screen& screen) {
    const auto& t = ::core::theme::current();
    const auto& wc = t.colors.wechat;
    const eui::Color kTransparent{0.0f, 0.0f, 0.0f, 0.0f};
    const float h = titlebarHeight();

    // 单钮 = 透明 hit rect（hover/pressed 换底色）+ 居中图标 text
    const auto drawWindowButton = [&](const char* idSuffix, float bx,
                                      unsigned int glyph, bool redHover,
                                      const std::function<void()>& action) {
        const std::string id = std::string("app.titlebar.") + idSuffix;
        if (redHover) {
            ui.rect(id + ".hit")
                .position(bx, 0.0f)
                .size(kWindowButtonW, h)
                .states(kTransparent, wc.warningRed, wc.warningRed)
                .onHover([](bool hovered) { g_closeButtonHovered = hovered; })
                .onClick(action)
                .build();
        } else {
            ui.rect(id + ".hit")
                .position(bx, 0.0f)
                .size(kWindowButtonW, h)
                .states(kTransparent, wc.dividerSecondary, wc.selectedBg)
                .onClick(action)
                .build();
        }
        ui.text(id + ".icon")
            .position(bx, 0.0f)
            .size(kWindowButtonW, h)
            .icon(eui::utf8(glyph))
            .fontSize(12.0f)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(redHover && g_closeButtonHovered
                       ? eui::Color{1.0f, 1.0f, 1.0f, 1.0f}
                       : wc.textSecondary)
            .build();
    };

    ui.stack("app.titlebar")
        .position(0.0f, 0.0f)
        .size(screen.width, h)
        .content([&] {
            // 左：logo 图标 + 品牌文本（与折叠态侧栏 logo 同款 glyph）
            ui.text("app.titlebar.logo")
                .position(16.0f, 0.0f)
                .size(28.0f, h)
                .icon(eui::utf8(0xF5FC))  // fa-laptop-code
                .fontSize(t.sizes.fontSizeBody)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(wc.primary)
                .build();
            ui.text("app.titlebar.brand")
                .position(16.0f + 28.0f + 8.0f, 0.0f)
                .size(160.0f, h)
                .text("DevDesk")
                .fontSize(t.sizes.fontSizeBody)
                .fontWeight(700)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(wc.textPrimary)
                .build();

            // 右：三钮从右往左 = 关闭 / 最大化（还原图标随窗口态切换）/ 最小化
            const bool maximized = mainWindowMaximized();
            drawWindowButton("close", screen.width - kWindowButtonW * 1.0f,
                             0xF00D,  // fa-xmark
                             /*redHover=*/true, [] { closeMainWindow(); });
            drawWindowButton("maximize", screen.width - kWindowButtonW * 2.0f,
                             maximized ? 0xF2D3 : 0xF2D2,  // fa-window-restore / -maximize
                             /*redHover=*/false, [maximized] {
                                 if (maximized) {
                                     restoreMainWindow();
                                 } else {
                                     maximizeMainWindow();
                                 }
                             });
            drawWindowButton("minimize", screen.width - kWindowButtonW * 3.0f,
                             0xF2D1,  // fa-window-minimize
                             /*redHover=*/false, [] { iconifyMainWindow(); });
        })
        .build();
}

// 常驻侧边栏：row(侧边栏, 页面内容)，列出 Router 全部已注册模块，当前页高亮，点击 navigateTo。
// EUI-NEO components/sidebar 是右侧滑出抽屉，不适合常驻导航栏，故按 spec token 自绘
void drawSidebar(eui::Ui& ui, const eui::Screen& screen) {
    ::core::Router* router = ::core::appRouter();
    if (!router) return;

    const auto& t = ::core::theme::current();
    // 微信色令牌快捷引用（W1 新增槽位，§2.1）；新旧并存，W3 侧栏逐页切换到此
    const auto& wc = t.colors.wechat;
    // W0.5：透明语义常量（非可主题化色，W0.5 误标为待清理字面量）。
    // 用途：inactive hit 底 + toggle 底 + active 遮让 SelectionBar（让 SelectionBar 盖底）。
    // 不抽到 theme.h 的 wechat.transparent 槽位（W3 不动 theme.h），保留为局部语义常量。
    const eui::Color kTransparent{0.0f, 0.0f, 0.0f, 0.0f};
    const bool collapsed = g_sidebarCollapsed;
    const float kWidth = sidebarWidth();

    static const bool mounted = [] {
        LOG_INFO("侧边栏导航已挂载（" +
                 std::to_string(::core::appRouter()->modules().size()) +
                 " 个模块入口，宽 208/72 可折叠）");
        return true;
    }();
    (void)mounted;
    const float itemH = ui_kit::kNavItemH;
    const float gap = 4.0f;
    const float padX = t.sizes.spacingLarge;
    constexpr float kIconW = 28.0f;
    constexpr float kItemInset = 12.0f;  // 内容距高亮条左缘内边距

    // 顶部留白取页面主区 padding 同档 token（spacingXXLarge=40）：品牌块上缘与页面内容齐平
    const float topPad = t.sizes.spacingXXLarge;
    // 品牌块高 32（字面值，不用 fontSizePage 运行时值：fontScale 会缩放 fontSize* 而该框不随之变）
    constexpr float kBrandH = 32.0f;
    // 品牌块 → 导航列表间距 28（无对应 token 档位；token 只有 4/8/16/24/40）
    constexpr float kBrandNavGap = 28.0f;

    ui.stack("app.sidebar")
        .size(kWidth, screen.height)
        .content([&] {
            // 侧栏底色：微绿灰（D-12 E1，§4.8 sidebarBgTinted #E8F0EC），z 序在最底，
            // divider/brand/nav/toggle 都在其上，仅染侧栏、不动页面区/白卡。
            // UAT2 B 批：tint 的 alpha 随窗口效果档位（core::window_effect_state 单值，
            // 写口 = settings applyWindowVisuals）——磨砂 0.15 透 Acrylic backdrop、
            // 半透 0.5、off 实色 1.0；off/translucent 档保留 §4.8 微绿灰观感，
            // 磨砂档侧栏不再把 backdrop 挡死。
            const float sidebarBgAlpha = ::core::window_effect_state::surfaceAlpha(
                ::core::window_effect_state::current());
            ui.rect("app.sidebar.bg")
                .size(kWidth, screen.height)
                .color(eui::Color{wc.sidebarBgTinted.r, wc.sidebarBgTinted.g,
                                  wc.sidebarBgTinted.b, sidebarBgAlpha})
                .build();
            // 右缘分割线：wechat.dividerPrimary（§4.7 一级 #E5E5E5，原 borderMuted 暖系）
            ui.rect("app.sidebar.divider")
                .position(kWidth - 1.0f, 0.0f)
                .size(1.0f, screen.height)
                .color(wc.dividerPrimary)
                .radius(t.sizes.radiusWeChat)
                .build();

            // brand 行（UAT3 #9）：折叠/展开两态统一为居中 logo（icon 模式下 text 存
            // glyph 的 UTF-8 串）；品牌文本由自绘标题栏承载（app.titlebar.brand）。
            // 行高/topPad/间距保持防跳动
            ui.text("app.sidebar.brand")
                .position(0.0f, topPad)
                .size(kWidth, kBrandH)
                .icon(eui::utf8(0xF5FC))  // fa-laptop-code logo
                .fontSize(t.sizes.fontSizePage)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(wc.primary)
                .build();

            float y = topPad + kBrandH + kBrandNavGap;

            // 导航项渲染：提取为 lambda（work/system 两组共用，避免重复代码）。
            // active 态视觉走 SelectionBar（§4.3：2px 左绿竖线 + selectedBg + textPrimary），
            // hit 区透明+onClick 承担点击；inactive 透出侧栏底，hover 浅灰反馈。
            auto renderNavItem = [&](::core::Module* m) {
                const ::core::PageId page = m->pageId();
                const bool active = (page == router->currentPage());
                const std::string id =
                    std::string("app.sidebar.item.") +
                    ::core::pageIdToString(page);

                // 命中区：normal 透明（active 时 SelectionBar 盖底；inactive 透出侧栏底）；
                // hover/pressed：active 透明（不遮 SelectionBar），inactive 浅灰/深灰
                ui.rect(id + ".hit")
                    .position(padX, y)
                    .size(kWidth - padX * 2.0f, itemH)
                    .states(kTransparent,
                            active ? kTransparent : wc.dividerSecondary,
                            active ? kTransparent : wc.selectedBg)
                    .radius(t.sizes.radiusWeChat)
                    .onClick([router, page] { router->navigateTo(page); })
                    .build();

                // active 态叠 SelectionBar（§4.3：2px 左绿竖线 + selectedBg）。
                // selectionBar 不传文字——其 .text 节点 z 序浮在 icon/label 之上导致重叠；
                // 文字由下方 .label 画（icon 同理）。selectionBar 只负责 bg + 左 2px 绿竖线。
                if (active) {
                    ui_kit::selectionBar(ui, id + ".sel", std::string{},
                                         padX, y, kWidth - padX * 2.0f, itemH, t);
                }

                // 图标与文字分两个元素：Icon 字体（FontAwesome）不能和正文字体混排在一个 Text 里。
                // 图标：active=wechat.primary（§4.8 "图标同步变绿"），inactive=wechat.textSecondary。
                // 标签：active=wechat.textPrimary（§4.3 "深灰文字 #1A1A1A"），inactive=wechat.textSecondary。
                if (collapsed) {
                    ui.text(id + ".icon")
                        .position((kWidth - kIconW) * 0.5f, y)
                        .size(kIconW, itemH)
                        .icon(m->iconGlyph())
                        .fontSize(t.sizes.fontSizeBody)
                        .horizontalAlign(eui::HorizontalAlign::Center)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(active ? wc.primary : wc.textSecondary)
                        .build();
                } else {
                    ui.text(id + ".icon")
                        .position(padX + kItemInset, y)
                        .size(kIconW, itemH)
                        .icon(m->iconGlyph())
                        .fontSize(t.sizes.fontSizeBody)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(active ? wc.primary : wc.textSecondary)
                        .build();
                    ui.text(id + ".label")
                        .position(padX + kItemInset + kIconW, y)
                        .size(kWidth - padX * 2.0f - kItemInset - kIconW,
                              itemH)
                        .text(m->displayName())
                        .fontSize(t.sizes.fontSizeBody)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(active ? wc.textPrimary : wc.textSecondary)
                        .build();
                }

                y += itemH + gap;
            };

            // 分两组（§4.8）：work（Home/Markdown/Todo/ClaudeProxy/Agent）→ system（Settings/About）。
            // 按 PageId 枚举分类，不依赖 modules() 顺序；router 按 PageId 序注册，天然 work 在前。
            // 组间画 app.sidebar.groupdiv（wechat.dividerPrimary #E5E5E5，宽=kWidth-2*padX，高 1px）。
            auto isSystemGroup = [](::core::PageId page) {
                return page == ::core::PageId::Settings ||
                       page == ::core::PageId::About;
            };

            bool groupDividerDrawn = false;
            bool sawWork = false;
            for (auto* m : router->modules()) {
                if (!m) continue;
                const ::core::PageId page = m->pageId();
                const bool isSystem = isSystemGroup(page);

                // work → system 首次切换时画组间 divider（仅当两组都非空：sawWork && isSystem）
                if (!groupDividerDrawn && sawWork && isSystem) {
                    ui.rect("app.sidebar.groupdiv")
                        .position(padX, y)
                        .size(kWidth - padX * 2.0f, 1.0f)
                        .color(wc.dividerPrimary)
                        .build();
                    y += 1.0f + gap;  // divider 高 1px + 标准 gap 到下一项
                    groupDividerDrawn = true;
                }
                if (!isSystem) sawWork = true;

                renderNavItem(m);
            }

            // 折叠开关（底部固定）：展开态 fa-angles-left、折叠态 fa-bars；展开左对齐、折叠居中。
            // toggle 底色透明（W0.5：kTransparent 语义常量，非可主题化色）；
            // hover/active 走 wechat.dividerSecondary/wechat.selectedBg；radius→radiusWeChat（D-12 E2 统一 6）
            constexpr float kToggleSize = 36.0f;
            const float toggleY = screen.height - 56.0f;
            const float toggleX =
                collapsed ? (kWidth - kToggleSize) * 0.5f : padX;
            ui.rect("app.sidebar.toggle")
                .position(toggleX, toggleY)
                .size(kToggleSize, kToggleSize)
                .states(kTransparent, wc.dividerSecondary, wc.selectedBg)
                .radius(t.sizes.radiusWeChat)
                .onClick([] {
                    setSidebarCollapsed(!sidebarCollapsed(), true);
                })
                .build();
            ui.text("app.sidebar.toggle.icon")
                .position(toggleX, toggleY)
                .size(kToggleSize, kToggleSize)
                .icon(eui::utf8(collapsed ? 0xF0C9 : 0xF100))  // fa-bars / fa-angles-left
                .fontSize(t.sizes.fontSizeBody)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(wc.textSecondary)
                .build();
        })
        .build();
}

void compose(eui::Ui& ui, const eui::Screen& screen) {
    // 启动 splash：2.0s 动画 + 0.3s 淡出后切主界面；期间跳过布局
    if (ui_kit::drawSplashIfNeeded(ui, screen)) {
        return;
    }
    ::core::Router* router = ::core::appRouter();
    if (!router) return;
    // 智能体 run 结束通知拉取（fix round）：迁移检测与通知发放都不依赖智能体页可见性——
    // 每帧在此 poll（页内 compose 的 refreshData 只在 Agent 页跑）；取走语义 = 模块入队、
    // 这里拉空即止。app 通道单实例：后发顶替先发（同帧多通知只显示最后一条，可接受——
    // notifiedRunIds_ 保证每条至少入队一次，用户回页仍能在任务列表看到终态）。
    for (::core::Module* m : router->modules()) {
        if (m->pageId() != ::core::PageId::Agent) continue;
        if (auto* agent = dynamic_cast<::module::agent::AgentModule*>(m)) {
            agent->pollRunEnd();
            while (auto notice = agent->takeRunEndNotice()) {
                showAppNotice(notice->first, notice->second);
            }
        }
    }
    // 首帧一次性落真实 contentScale：窗口创建前查不到（首次 update 前恒 1.0）；Screen::scale 与
    // app::contentScale() 同源同值（物理 = 逻辑 × scale）
    static const bool scaleLogged = [&screen]() {
        LOG_INFO("首帧屏幕尺度：contentScale(Screen.scale)=" +
                 std::to_string(screen.scale) + "（逻辑画布 " +
                 std::to_string(static_cast<int>(screen.width + 0.5f)) + "x" +
                 std::to_string(static_cast<int>(screen.height + 0.5f)) +
                 "）——macOS 上 contentScale 是 Retina backing(2.0)，与窗口尺寸"
                 "单位（点）无关，别读成窗口缩放");
        return true;
    }();
    (void)scaleLogged;
    // 布局（UAT3 D-R3-1 自绘标题栏占位）：Windows 自绘壳 → 顶层先画 40px 标题条，
    // 根布局下移、侧边栏与页面内容同起点（页面顶与侧栏顶对齐）；macOS 保系统标题栏
    // → 零占位，几何与旧版一致。bodyScreen/根布局/页面区三处高度同源，不允许各算各的
    const bool chrome = customTitlebarActive();
    const float bodyTop = chrome ? titlebarHeight() : 0.0f;
    if (chrome) {
        drawTitlebar(ui, screen);
    }
    eui::Screen bodyScreen;
    bodyScreen.width = screen.width;
    bodyScreen.height = screen.height - bodyTop;
    bodyScreen.scale = screen.scale;
    ui.row("app.root")
        .position(0.0f, bodyTop)
        .size(screen.width, bodyScreen.height)
        .content([&] {
            drawSidebar(ui, bodyScreen);
            const float pageW = pageContentWidth(screen.width);
            // 页面区原点 x = 侧边栏实际占宽；模块把窗口坐标指针位置换算成页面坐标时经 Router 取本值
            router->setPageOriginX(sidebarWidth());
            ui.stack("app.page")
                .size(pageW, bodyScreen.height)
                .clip()
                .content([&] {
                    eui::Screen pageScreen;
                    pageScreen.width = pageW;
                    pageScreen.height = bodyScreen.height;
                    // 手搓 Screen 不填 scale 会退化成 1.0，页面内坐标换算与框架三处不同源
                    pageScreen.scale = screen.scale;
                    router->compose(ui, pageScreen);
                })
                .build();
        })
        .build();
    // 应用级 toast：row 之后顶层绘制盖过侧边栏/页面；每帧无条件调用、visible 控状态
    // M-2（UAT2 一审）：popup 类控件统一 wechat 皮肤（与五模块 toast 一致）
    ui_kit::drawToast(ui, screen, "app.notice", g_appNotice,
                      core::theme::componentTokensWeChat(core::theme::current()),
                      ui_kit::ToastOptions{.durationSec = 4.5f, .okIcon = 0xF05A});
}

} // namespace app
