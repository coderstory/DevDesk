// src/app_sidebar.cpp
// 常驻侧边栏 + 折叠/展开实现（接口见 app_sidebar.h）
//
// 折叠态规格（2026-09-23）：
//   - 宽度 208 → 72；brand 展开 = logo 图标 + "DevDesk"（logo 在左），
//     折叠 = 只显示居中 logo 图标（2026-09-24：字母 D 换成 logo）
//   - 导航项只显图标（水平居中），label 不渲染，hover tooltip 补模块名
//     （components/tooltip 走 hoverOpacityFrom，非 hover 时透明度 0 且
//     不参与命中测试，不挡页面点击）
//   - 底部折叠开关按钮：展开态 fa-angles-left（收起）/ 折叠态 fa-bars（展开）
//   - 动画：EUI-NEO AnimProperty 无 Width 档（Frame 档会带动整棵子树位
//     移，页面区宽度又由本文件按目标态瞬时计算，两者错拍），登记为瞬时
//     切换——后续若上游补 Width 动画再接
#include "app_sidebar.h"

#include "core/component_theme.h"
#include "core/database.h"
#include "core/logging.h"
#include "core/router.h"
#include "core/theme.h"
#include "settings_config.h"
#include <ui_kit/metrics.h>
#include <ui_kit/splash.h>
#include <ui_kit/toast.h>

#include <eui/types.h>

namespace app {

namespace {

// 进程内折叠状态单值；默认展开，启动恢复由 main.cpp 调
// setSidebarCollapsed(cfg.sidebarCollapsed)（不回写）
bool g_sidebarCollapsed = false;

// 应用级 toast 通知（审计 [7]）：托盘失效入口点击的解释反馈。
// 调用方（main.cpp）写状态，compose 顶层每帧读状态绘制；单实例。
// 状态位与绘制接线统一走 ui_kit（任务 D / R1），对外 API 不变。
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
    // 持久化：load → 改单字段 → save，保留其余配置（主题/字号/模块开关）。
    // SettingsStore::save 把 11 个顶层键放在**一个事务**里逐行写，其余行原样
    // 保留 —— 语义与旧的「重写整份 config.json」等价，但别的写者（主题切换 /
    // 设置页）不会因为本函数手慢而丢字段。失败只记日志不回滚内存态
    //（跟主题持久化同宽容度，下次启动回默认展开）
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

// 测试缝（TIN-07）：折叠状态回厂（展开）。setSidebarCollapsed 是生产写路径
//（persist=true 时落库），用例间回厂走这里——纯内存、不触发持久化副作用
void resetSidebarGlobals() { g_sidebarCollapsed = false; }

// ---- 应用级 toast 通知（审计 [7]：托盘失效入口点击的解释通道）----
void showAppNotice(const std::string& title, const std::string& message) {
    g_appNotice.show(title, message, false);
}

void clearAppNotice() {
    g_appNotice.dismiss();
    g_appNotice.title.clear();
    g_appNotice.body.clear();
}

bool appNoticeVisible() { return g_appNotice.open; }
// 无通知时一律空串：拆分前的 onAutoDismiss/onDismiss 回调直接调 clearAppNotice
//（连内容一起清），改走 ui_kit 后回调只落状态位，故在此保持同一可观察语义
//（open=false 时旧实现的内容必已被清空，二者输出逐字相同）。
std::string appNoticeTitle() { return g_appNotice.open ? g_appNotice.title : std::string{}; }
std::string appNoticeMessage() { return g_appNotice.open ? g_appNotice.body : std::string{}; }

float pageContentWidth(float windowWidth) {
    return windowWidth - sidebarWidth();
}

// ---- 常驻侧边栏（spec §2 预期效果 / §6.5.2 侧边栏规格）----
// 布局：row(侧边栏, 页面内容)。侧边栏列出 Router 已注册的全部模块
//（含设置），当前页高亮，点击 navigateTo。EUI-NEO components/sidebar 是
//右侧滑出抽屉（scrim + 关闭按钮，见 ../EUI-NEO/components/sidebar.h），
//不适合常驻导航栏，因此这里按 spec token 自绘（设计 token 属 DevDesk 自己的事）。
void drawSidebar(eui::Ui& ui, const eui::Screen& screen) {
    ::core::Router* router = ::core::appRouter();
    if (!router) return;
    const auto& t = ::core::theme::current();
    const bool collapsed = g_sidebarCollapsed;
    const float kWidth = sidebarWidth();

    // 渲染自证：首帧挂载日志（侧边栏每帧重绘，static 只记一次）
    static const bool mounted = [] {
        LOG_INFO("侧边栏导航已挂载（" +
                 std::to_string(::core::appRouter()->modules().size()) +
                 " 个模块入口，宽 208/72 可折叠）");
        return true;
    }();
    (void)mounted;

    const float itemH = ui_kit::kNavItemH;  // 导航项高 40（ui_kit/metrics.h）
    const float gap = 4.0f;            // 导航项 gap 4
    const float padX = t.sizes.spacingLarge;  // 左右 padding 16
    constexpr float kIconW = 28.0f;
    constexpr float kItemInset = 12.0f;  // 内容距高亮条左缘内边距（用户反馈文字贴边）

    // ---- 顶部留白（2026-09-28 用户真机反馈「侧边栏加一点上边距」）----
    // 取页面主区 padding 同档 token（spacingXXLarge = 40）：品牌块上缘与页面内容
    // 上缘齐平，不再比页面内容更贴顶。原值 24 是 Windows 期定的（那时窗口顶到内容
    // 顶的观感更小）；macOS 上窗口客户区顶就是屏幕可见区顶（标题栏在客户区之外、
    // 不占内容高），24 显得贴顶——所以这个值按 macOS 观感定，Windows 同步跟随
    //（共享算式，不做平台分支：同一份 token 在两端都是「页面 padding」的语义）。
    const float topPad = t.sizes.spacingXXLarge;
    // 品牌块高 32（既有字面值，这里提成常量只为让下方导航起点与它同源：
    // 字号 = fontSizePage 32 的图标/文字单行框；**不**改成本 token 的运行时值，
    // 因为 fontScale 会缩放 fontSize* 而该框历来不随之变，改了会顺带改观感）
    constexpr float kBrandH = 32.0f;
    // 品牌块 → 导航列表的既有间距 28：本次只下移顶部留白，不动这两块的相对间距
    //（避免一次改两处观感）。无对应 token 档位——token 只有 4/8/16/24/40。
    constexpr float kBrandNavGap = 28.0f;

    ui.stack("app.sidebar")
        .size(kWidth, screen.height)
        .content([&] {
            // 侧边栏底色 = 主窗口 clearColor（applyWindowVisuals 随主题/窗口
            // 效果档运行时跟随）——不自铺不透明底，半透/磨砂档下侧边栏与页面
            // 主体一致透出效果；色值与旧 t.colors.background 完全相同（off 档
            // 观感不变）。本改动自 fix/winfx-pet d923f0f 移植（侧边栏拆文件后
            // 的落点）
            ui.rect("app.sidebar.divider")
                .position(kWidth - 1.0f, 0.0f)
                .size(1.0f, screen.height)
                .color(t.colors.borderMuted)
                .radius(t.sizes.radius)
                .build();

            // brand 行（2026-09-24）：展开 = logo 图标 + "DevDesk" 横排
            //（logo 品牌色，在文字左侧）；折叠 = 只显示居中 logo 图标。
            // 两态共用 topPad（同一纵向位置，只差横向分支）——折叠态是同一块
            // 品牌区，若只改展开态的 y，切折叠会看到 logo 跳一下
            if (collapsed) {
                ui.text("app.sidebar.brand")
                    .position(0.0f, topPad)
                    .size(kWidth, kBrandH)
                    .icon(eui::utf8(0xF5FC))  // fa-laptop-code logo
                    .fontSize(t.sizes.fontSizePage)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.primary)
                    .build();
            } else {
                ui.text("app.sidebar.brand.logo")
                    .position(padX, topPad)
                    .size(kIconW, kBrandH)
                    .icon(eui::utf8(0xF5FC))  // fa-laptop-code logo
                    .fontSize(t.sizes.fontSizePage)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.primary)
                    .build();
                ui.text("app.sidebar.brand")
                    .position(padX + kIconW + 8.0f, topPad)
                    .size(kWidth - padX * 2.0f - kIconW - 8.0f, kBrandH)
                    .text("DevDesk")
                    .fontSize(t.sizes.fontSizePage)
                    .fontWeight(700)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.textPrimary)
                    .build();
            }

            float y = topPad + kBrandH + kBrandNavGap;  // 40 + 32 + 28 = 100
            for (auto* m : router->modules()) {
                if (!m) continue;
                const ::core::PageId page = m->pageId();
                const bool active = (page == router->currentPage());
                const std::string id =
                    std::string("app.sidebar.item.") +
                    ::core::pageIdToString(page);

                // 命中区 + 悬停反馈（states: 常态/悬停/按下）。
                // 折叠态命中区 = 72 - 16×2 = 40 宽（图标 28 居中在内）
                ui.rect(id + ".hit")
                    .position(padX, y)
                    .size(kWidth - padX * 2.0f, itemH)
                    .states(active ? t.colors.primarySoftBg
                                   : eui::Color{0.0f, 0.0f, 0.0f, 0.0f},
                            t.colors.hover, t.colors.active)
                    .radius(t.sizes.radius)
                    .onClick([router, page] { router->navigateTo(page); })
                    .build();
                // 图标与文字分开两个元素：.icon() 用的 Icon 字体
                //（FontAwesome）不能和正文字体混排在一个 Text 里
                if (collapsed) {
                    // 折叠：图标水平居中（框位置居中 + 框内 horizontalAlign
                    // Center——2026-09-24 修复：glyph 默认贴框左缘导致视觉
                    // 偏左、右侧空白多），label 不渲染
                    ui.text(id + ".icon")
                        .position((kWidth - kIconW) * 0.5f, y)
                        .size(kIconW, itemH)
                        .icon(m->iconGlyph())
                        .fontSize(t.sizes.fontSizeBody)
                        .horizontalAlign(eui::HorizontalAlign::Center)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(active ? t.colors.primary
                                      : t.colors.textSecondary)
                        .build();
                } else {
                    ui.text(id + ".icon")
                        .position(padX + kItemInset, y)
                        .size(kIconW, itemH)
                        .icon(m->iconGlyph())
                        .fontSize(t.sizes.fontSizeBody)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(active ? t.colors.primary
                                      : t.colors.textSecondary)
                        .build();
                    ui.text(id + ".label")
                        .position(padX + kItemInset + kIconW, y)
                        .size(kWidth - padX * 2.0f - kItemInset - kIconW,
                              itemH)
                        .text(m->displayName())
                        .fontSize(t.sizes.fontSizeBody)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(active ? t.colors.primary
                                      : t.colors.textSecondary)
                        .build();
                }

                y += itemH + gap;
            }

            // 折叠开关（底部固定）：展开态 fa-angles-left（点 = 收起）、
            // 折叠态 fa-bars（点 = 展开）；展开左对齐导航、折叠水平居中
            constexpr float kToggleSize = 36.0f;
            const float toggleY = screen.height - 56.0f;
            const float toggleX =
                collapsed ? (kWidth - kToggleSize) * 0.5f : padX;
            ui.rect("app.sidebar.toggle")
                .position(toggleX, toggleY)
                .size(kToggleSize, kToggleSize)
                .states(eui::Color{0.0f, 0.0f, 0.0f, 0.0f}, t.colors.hover,
                        t.colors.active)
                .radius(t.sizes.radius)
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
                .color(t.colors.textSecondary)
                .build();
        })
        .build();
}

void compose(eui::Ui& ui, const eui::Screen& screen) {
    // 启动 splash（ui_kit）：极光波 shader + logo + 进度条，2.0s 动画 +
    // 0.3s 淡出后切主界面；splash 期间跳过主界面布局（不依赖 router）
    if (ui_kit::drawSplashIfNeeded(ui, screen)) {
        return;
    }
    ::core::Router* router = ::core::appRouter();
    if (!router) return;
    // 首帧一次性落真实 contentScale（2026-09-28 组 A 的日志口径）：启动尺寸决策
    // 发生在窗口创建之前，那时 contentScale 还查不到（EUI-NEO 首次 update 之前
    // app::contentScale() 恒 1.0），于是旧日志里的「缩放 1.000000」被读成"Retina
    // 上没做折算"的错误前提。compose 收到的 Screen::scale 与 app::contentScale()
    // 同源同值（EUI-NEO app.h 的 U1 尺度契约：物理 = 逻辑 × scale），这里是最早
    // 能看到真值的地方。每帧重绘只记一次（static 自初始化）。
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
    // 布局：row(常驻侧边栏, 页面内容)——spec §2"左侧侧边栏切换工具，右侧主区"；
    // 侧边栏宽随折叠态 208/72，页面区拿剩余宽度（折叠后 +136px）
    ui.row("app.root")
        .size(screen.width, screen.height)
        .content([&] {
            drawSidebar(ui, screen);
            const float pageW = pageContentWidth(screen.width);
            // 页面区原点 x = 侧边栏实际占宽：模块把窗口坐标的指针位置换算成
            // 页面坐标（右键菜单定位等）时经 Router 取本值（见 router.h）
            router->setPageOriginX(sidebarWidth());
            ui.stack("app.page")
                .size(pageW, screen.height)
                .clip()
                .content([&] {
                    // 页面模块按"去掉侧边栏后的宽度"布局（本帧的 Screen 带真值，
                    // scale 一并透传——见下）
                    eui::Screen pageScreen;
                    pageScreen.width = pageW;
                    pageScreen.height = screen.height;
                    // 尺度真值随框架传入的那份 Screen 一起带上（2026-09-28 组 A）：
                    // 手搓 Screen 不填 scale 会退化成 1.0（EUI-NEO Screen 默认值），
                    // 页面内做指针/坐标换算的模块拿它就与框架三处（Screen /
                    // PointerEvent / DragEvent）不同源。**改动前**零消费点——
                    // 复现口径是改动前的提交，不是当前工作区：
                    //   git grep -n "\.scale" 2839c99 -- src   # 输出为空
                    //（当前工作区 grep 会命中本文件 3 处 = 注释 + 读 + 写，属预期，
                    //  不是"有消费点"的反证）→ 本次零行为变化，只是把"今后误用会
                    // 立刻现形"的前提装上，而不是继续靠"macOS 碰巧没差"。
                    pageScreen.scale = screen.scale;
                    router->compose(ui, pageScreen);
                })
                .build();
        })
        .build();
    // 应用级 toast（审计 [7]）：row 之后顶层绘制盖过侧边栏/页面；每帧无条件
    // 调用、visible 控状态（同各模块 drawToast 模式），自动消失回写 clear。
    // 应用级通知用 info 图标、无失败态，时长 4.5s（现状逐字保留）
    ui_kit::drawToast(ui, screen, "app.notice", g_appNotice,
                      core::theme::componentTokens(core::theme::current()),
                      ui_kit::ToastOptions{.durationSec = 4.5f, .okIcon = 0xF05A});
}

} // namespace app
