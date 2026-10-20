// 常驻侧边栏 + 折叠/展开实现（接口见 app_sidebar.h）。折叠态：宽 208→72，只显居中图标，
// label 不渲染；动画为瞬时切换（EUI-NEO AnimProperty 无 Width 档）。
#include "app_sidebar.h"

#include "core/component_theme.h"
#include "core/database.h"
#include "core/logging.h"
#include "core/router.h"
#include "core/theme.h"
#include "module_agent/agent_module.h"
#include "settings_config.h"
#include <ui_kit/metrics.h>
#include <ui_kit/selection_bar.h>  // W3：SelectionBar 选中态 helper（§4.3）
#include <ui_kit/splash.h>
#include <ui_kit/toast.h>

#include <eui/types.h>

namespace app {

namespace {

// 进程内折叠状态单值；默认展开，启动恢复由 main.cpp 调 setSidebarCollapsed（不回写）
bool g_sidebarCollapsed = false;

// 应用级 toast 通知：托盘失效入口点击的解释反馈。调用方写状态，compose 顶层每帧读状态绘制
ui_kit::ToastState g_appNotice;

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
            // divider/brand/nav/toggle 都在其上。原"不自铺底以透出磨砂效果"的口径作废——
            // §4.8 明示侧栏要自己的微绿灰底（仅染侧栏，不动页面区/白卡）。
            ui.rect("app.sidebar.bg")
                .size(kWidth, screen.height)
                .color(wc.sidebarBgTinted)
                .build();
            // 右缘分割线：wechat.dividerPrimary（§4.7 一级 #E5E5E5，原 borderMuted 暖系）
            ui.rect("app.sidebar.divider")
                .position(kWidth - 1.0f, 0.0f)
                .size(1.0f, screen.height)
                .color(wc.dividerPrimary)
                .radius(t.sizes.radiusWeChat)
                .build();

            // brand 行：展开 = logo + "DevDesk" 横排，折叠 = 只显示居中 logo；两态共用 topPad 防跳动
            if (collapsed) {
                ui.text("app.sidebar.brand")
                    .position(0.0f, topPad)
                    .size(kWidth, kBrandH)
                    .icon(eui::utf8(0xF5FC))  // fa-laptop-code logo
                    .fontSize(t.sizes.fontSizePage)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(wc.primary)
                    .build();
            } else {
                ui.text("app.sidebar.brand.logo")
                    .position(padX, topPad)
                    .size(kIconW, kBrandH)
                    .icon(eui::utf8(0xF5FC))  // fa-laptop-code logo
                    .fontSize(t.sizes.fontSizePage)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(wc.primary)
                    .build();
                ui.text("app.sidebar.brand")
                    .position(padX + kIconW + 8.0f, topPad)
                    .size(kWidth - padX * 2.0f - kIconW - 8.0f, kBrandH)
                    .text("DevDesk")
                    .fontSize(t.sizes.fontSizePage)
                    .fontWeight(700)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(wc.textPrimary)
                    .build();
            }

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
    // 布局 row(常驻侧边栏, 页面内容)：侧边栏宽随折叠态 208/72，页面区拿剩余宽度
    ui.row("app.root")
        .size(screen.width, screen.height)
        .content([&] {
            drawSidebar(ui, screen);
            const float pageW = pageContentWidth(screen.width);
            // 页面区原点 x = 侧边栏实际占宽；模块把窗口坐标指针位置换算成页面坐标时经 Router 取本值
            router->setPageOriginX(sidebarWidth());
            ui.stack("app.page")
                .size(pageW, screen.height)
                .clip()
                .content([&] {
                    eui::Screen pageScreen;
                    pageScreen.width = pageW;
                    pageScreen.height = screen.height;
                    // 手搓 Screen 不填 scale 会退化成 1.0，页面内坐标换算与框架三处不同源
                    pageScreen.scale = screen.scale;
                    router->compose(ui, pageScreen);
                })
                .build();
        })
        .build();
    // 应用级 toast：row 之后顶层绘制盖过侧边栏/页面；每帧无条件调用、visible 控状态
    ui_kit::drawToast(ui, screen, "app.notice", g_appNotice,
                      core::theme::componentTokens(core::theme::current()),
                      ui_kit::ToastOptions{.durationSec = 4.5f, .okIcon = 0xF05A});
}

} // namespace app
