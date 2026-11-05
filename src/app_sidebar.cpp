// 常驻侧边栏 + 折叠/展开实现（接口见 app_sidebar.h）。折叠态：宽 208→72，只显居中图标，
// label 不渲染；动画为瞬时切换（EUI-NEO AnimProperty 无 Width 档）。
#include "app_assembly.h"
#include "app_registry.h"  // 分组父项元数据（登记表 parent 列）
#include "app_sidebar.h"

#include "core/component_theme.h"
#include "core/database.h"
#include "core/logging.h"
#include "core/router.h"
#include "core/theme.h"
#include "core/window_effect_state.h"  // 侧栏底 alpha 按窗口效果档位（UAT2 B 批）
#include "core/window_geometry.h"  // 窗体记忆账本（UAT6：compose 每帧记账还原态尺寸）
#include "module_agent/agent_module.h"
#include "module_memory/memory_module.h"  // 需-9：用量告警每帧判定驱动（M3 同款）
#include "platform/window_visibility.h"  // 主窗可见态（平台接口隔离，业务层零 Win32）
#include "settings_config.h"
#include <components/scrollview.h>  // need186 §11：导航滚动区（EUI 全站 scrollView idiom）
#include <ui_kit/logo.h>  // 图标批 2026-10-20：brand 行/标题栏几何 logo（C 方案终端窗 >_）
#include <ui_kit/metrics.h>
#include <ui_kit/selection_bar.h>  // W3：SelectionBar 选中态 helper（§4.3）
#include <ui_kit/splash.h>
#include <ui_kit/toast.h>
#include <ui_kit/window_ring.h>  // 池168：窗口绿白间隔圆角虚框 ring（调研报告方案 a）

#include <eui/app.h>  // app::closeMainWindow（--minimized 首帧藏主窗）
#include <eui/types.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <optional>
#include <set>
#include <utility>
#include <vector>  // §11 行规划表 navPlan

namespace app {

namespace {

// 进程内折叠状态单值；默认展开，启动恢复由 main.cpp 调 setSidebarCollapsed（不回写）
bool g_sidebarCollapsed = false;

// 分组父项展开集合（空 = 全折叠 = 默认态）；进程内单值，不落库。存 PageId 而非下标：
// 登记表增行不移动既有分组身份。渲染与 toggle 共用本轮唯一状态（sidebarGroupExpanded）。
std::set<::core::PageId> g_expandedGroups;

// 113-6 自动展开去重闩：只在「当前页变化」时补展开一次；同页手动收起不被
// 每帧强制回展。复位跟随 resetSidebarGlobals（与展开集合一并回厂）。
::core::PageId g_autoExpandedForPage = ::core::PageId::Home;

// 应用级 toast 通知：托盘失效入口点击的解释反馈。调用方写状态，compose 顶层每帧读状态绘制
ui_kit::ToastState g_appNotice;

// 自绘标题栏布局开关的测试钉住缝（nullopt = 跟平台能力走，见 customTitlebarActive）
std::optional<bool> g_titlebarModeOverride;

// 关闭钮 hover 态：EUI-NEO .onHover 回调写、compose 读。红底需要图标色随 hover 翻白，
// 而 .states() 只能给 rect 上色——关闭叉是独立 text 元素，色值须在此跨帧传递
bool g_closeButtonHovered = false;

// ── 动态背景（UAT6 主窗活体流质）：进程级状态（测试缝复位见 reset）──
// 配置运行时镜像：首帧惰性读 SettingsStore（fluidBackgroundEnabled）一次，
// 此后设置页经 setFluidBackgroundEnabled 写（持久化由设置侧 persist 落库）。
std::optional<bool> g_fluidEnabled;
// 窗可见态测试覆写（nullopt = 跟平台查询走）；生产路径恒 nullopt
std::optional<bool> g_fluidVisibleMode;
// 相位源（秒）：71ce7d4c 起走壁钟 elapsed（fluidPhaseSeconds() =
// steady_clock::now() - g_fluidEpoch），不再由帧泵 onFrame 累加。旧 g_fluidPhase
// 只写死状态已随 W3.5 审计 B（F4）移除。注意：壁钟模式下最小化/藏托盘期间相位
// 照走、休眠恢复会跳变——原「隐藏冻结 + dt 钳 0.1s」语义待 elapsed helper 收口。
static std::chrono::steady_clock::time_point g_fluidEpoch = std::chrono::steady_clock::now();

// 确定性 0~1 hash（splash.cpp 墨染同款 sin 散列取小数）：卫星的角度/距离/大小
// 全由索引 hash 决定——帧间稳定不闪烁，只随相位缓慢漂移（有机噪声感）
float fluidHash01(int n) {
    const float x = std::sin(static_cast<float>(n) * 127.1f) * 43758.5453f;
    return x - std::floor(x);
}

// 浅色档三色轮替（同族不同明度）：流质晕专用（need172 §2.3「kLightHalo 同款三色
// 口径」，自 drawFluidBackground 上提）。v2（§10.3）起侧栏收缩按钮粒子不再共用此
// 表——第三色 #A7F3D0 与侧栏浅底近同亮度是粒子「观感弱」主因之一，改用下方专用表
const eui::Color kLightHalo[3] = {eui::Color("#07C160"),
                                  eui::Color("#34D399"),
                                  eui::Color("#A7F3D0")};

// 收缩按钮弥散粒子浅色档三色（need172 v2 §10.3：弃用近底色的最浅档，第三色改
// primaryHover 深档 #06AD56 拉开明度跨度；仅粒子用，流质晕 kLightHalo 不随之改）
const eui::Color kParticleLight[3] = {eui::Color("#07C160"),
                                      eui::Color("#34D399"),
                                      eui::Color("#06AD56")};

// 收缩按钮 hover/按压态跨帧单值（EUI states 只管底色，border/scale/图标色要按态
// 每帧换目标值——g_closeButtonHovered 先例）
bool g_toggleHovered = false;
bool g_togglePressed = false;
// 弥散绿粒子迸发起点（hover 进入沿写入一次；nullopt = 空闲零粒子零泵）
std::optional<std::chrono::steady_clock::time_point> g_toggleParticleStart;

// 导航滚动区 offset（need186 §11.2）：页面持有、compose 每帧 clamp 到
// [0, contentH−viewportH] 收敛（窗高/档位/分组收展变化后无动画收敛）；
// scrollView onChange 回写（滚轮/拖拽/点轨道），折叠/展开切换不触碰（viewport
// 两态同公式，offset 保持）。测试预置口 setNavScrollOffsetForTests
float g_navScrollOffset = 0.0f;

// 可见态门控查询（compose 每帧顶层调用，铁律防线）：最小化（iconic）或藏托盘
//（hidden）→ false。原生查询收在 platform::window_visibility（Windows =
// IsIconic/IsWindowVisible；macOS = GLFW_ICONIFIED/GLFW_VISIBLE），业务层零平台
// 宏。两平台的零成本兜底都是 EUI 主循环跳帧（hiddenToTray/iconified 分支不跑
// 主窗 update/compose/render），本查询再钉一道：不可见帧即便 compose 被调用
// 也不画流质、不挂泵、相位不推进；平台判定不了时接口按可见降级（fail-open）。
bool fluidWindowVisible() {
    if (g_fluidVisibleMode.has_value()) return *g_fluidVisibleMode;
    return ::platform::window_visibility::mainWindowVisible();
}

} // namespace

bool sidebarCollapsed() { return g_sidebarCollapsed; }

// 终端全屏隐藏态（clue92 老板终裁：侧栏 0 宽全隐；72px 折叠方案已作废）；
// 进入前折叠态快照供退出恢复
bool g_sidebarFsHidden = false;
bool g_preFullscreenCollapsed = false;

void setSidebarHiddenForFullscreen(bool hidden) {
    if (hidden && !g_sidebarFsHidden) {
        g_preFullscreenCollapsed = g_sidebarCollapsed;
    }
    g_sidebarFsHidden = hidden;
    g_sidebarCollapsed = hidden ? true : g_preFullscreenCollapsed;
}

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
    // 终端全屏态（老板 2026-11-05 再裁决，作废 v2 的 72px 折叠方案）：完全隐藏
    // （0 宽，不画 logo——老板嫌弃的是「半截 logo」本身）；终端页内容区随之全宽
    // （页宽=windowWidth−sidebarWidth()，本函数归零即自动生效）；退出恢复进屏前态
    if (g_sidebarFsHidden) return 0.0f;
    return g_sidebarCollapsed ? kSidebarWidthCollapsed : kSidebarWidthExpanded;
}

// 测试缝：侧边栏宽度状态回厂（展开）+ 分组展开集合回厂（空 = 默认折叠），
// 纯内存不触发持久化副作用
void resetSidebarGlobals() {
    g_sidebarCollapsed = false;
    g_sidebarFsHidden = false;        // 终端全屏全隐旗标回厂（复审 P3：测试隔离）
    g_preFullscreenCollapsed = false; // 进全屏前折叠态快照回厂
    g_expandedGroups.clear();  // 空 = 全折叠（默认态）；复位语义不变
    g_autoExpandedForPage = ::core::PageId::Home;  // 113-6 闩回厂：复位后落位子页仍会自动展开
    g_toggleHovered = false;  // need172 按钮 hover/按压/粒子态回厂（测试隔离）
    g_togglePressed = false;
    g_toggleParticleStart.reset();
    g_navScrollOffset = 0.0f;  // §11 导航滚动 offset 回厂（测试隔离）
}

// 测试缝：预置导航滚动 offset（无头测试驱动「滚到底」场景）；写入值在下一次
// compose 按 [0, contentH−viewportH] clamp（§11.2 收敛语义）
void setNavScrollOffsetForTests(float offset) {
    g_navScrollOffset = std::max(0.0f, offset);
}

bool sidebarGroupExpanded(::core::PageId parent) {
    // 唯一真相：集合成员即展开，空集合（默认）即折叠。渲染与 toggle 都读这一个状态。
    return g_expandedGroups.find(parent) != g_expandedGroups.end();
}

void toggleSidebarGroup(::core::PageId parent) {
    if (!g_expandedGroups.insert(parent).second) {
        g_expandedGroups.erase(parent);
    }
}

// ---- 动态背景（UAT6 主窗活体流质）----

bool fluidBackgroundEnabled() {
    if (!g_fluidEnabled.has_value()) {
        // 首帧惰性读库一次并缓存（此后每帧零 DB 查询）；库未就绪（无头测试等）
        // 按默认值，不制造每帧 ERROR 日志
        ::core::data::SqliteDb* db = ::core::appDatabase();
        if (db != nullptr) {
            const module::settings::SettingsStore store(db);
            g_fluidEnabled = store.load().fluidBackgroundEnabled;
        } else {
            g_fluidEnabled =
                module::settings::SettingsConfig{}.fluidBackgroundEnabled;
        }
    }
    return *g_fluidEnabled;
}

void setFluidBackgroundEnabled(bool enabled) { g_fluidEnabled = enabled; }

float fluidPhaseSeconds() {
    using namespace std::chrono;
    return duration<float>(steady_clock::now() - g_fluidEpoch).count();
}

void setFluidWindowVisibleMode(std::optional<bool> mode) {
    g_fluidVisibleMode = std::move(mode);
}

void resetFluidBackgroundForTests() {
    g_fluidEnabled.reset();
    g_fluidVisibleMode.reset();
    g_fluidEpoch = std::chrono::steady_clock::now();
}

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

// 窗口钮热区：右缘两钮（关/最小），每钮 46×标题条高（UAT4：最大化钮按用户
// 拍板移除——最大化/还原只留标题栏双击一条路径）
constexpr float kWindowButtonW = 46.0f;

bool titlebarCaptionHit(const CaptionHitQuery& query) {
    // 拖拽区 = 标题条带内、右缘两钮热区左侧之外（品牌文本区可拖）。
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
    return query.x < query.width - kWindowButtonW * 2.0f;
}

// 自绘标题条：左品牌文本、右两钮（关/最小）；不铺底色——半透/磨砂的 clearColor +
// DWM backdrop 直接透到标题条区（D-R3-1 的落点：undecorated 后窗口效果铺满全窗）。
// wechat 风格：灰字灰 hover；关闭钮 hover 红（theme warningRed #FA5151）+ 白叉
void drawTitlebar(eui::Ui& ui, const eui::Screen& screen) {
    const auto& t = ::core::theme::current();
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
                .states(kTransparent, t.colors.error, t.colors.error)
                .onHover([](bool hovered) { g_closeButtonHovered = hovered; })
                .onClick(action)
                .build();
        } else {
            ui.rect(id + ".hit")
                .position(bx, 0.0f)
                .size(kWindowButtonW, h)
                .states(kTransparent, t.colors.hover, t.colors.primarySoftBg)
                .onClick(action)
                .build();
        }
        ui.text(id + ".icon")
            .position(bx, 0.0f)
            .size(kWindowButtonW, h)
            .icon(eui::utf8(glyph))
            .fontSize(core::theme::scaled(12.0f))
            .horizontalAlign(eui::HorizontalAlign::Center)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(redHover && g_closeButtonHovered
                       ? eui::Color{1.0f, 1.0f, 1.0f, 1.0f}
                       : t.colors.textSecondary)
            .build();
    };

    ui.stack("app.titlebar")
        .position(0.0f, 0.0f)
        .size(screen.width, h)
        .content([&] {
            // 左：品牌文本（UAT4：标题条 20px logo 按用户拍板移除——品牌图形只留
            // 侧栏一处；起点对齐侧栏 padX=16，拖拽热区相应左扩）
            ui.text("app.titlebar.brand")
                .position(16.0f, 0.0f)
                .size(160.0f, h)
                .text("DevDesk")
                .fontSize(t.sizes.fontSizeBody)
                .fontWeight(700)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(t.colors.textPrimary)
                .build();

            // 右：两钮从右往左 = 关闭 / 最小化（UAT4：最大化钮移除，标题栏
            // 双击保留最大化/还原；关闭走 closeMainWindow = 与系统 X 同链，
            // 托盘模式下隐藏到托盘——EUI-NEO 侧修复）
            drawWindowButton("close", screen.width - kWindowButtonW * 1.0f,
                             0xF00D,  // fa-xmark
                             /*redHover=*/true, [] { closeMainWindow(); });
            drawWindowButton("minimize", screen.width - kWindowButtonW * 2.0f,
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

    // 113-6：当前页落在分组子页时父组自动展开（进程级状态，不落库）。挂在
    // drawSidebar 顶层 = router 负载导航与侧栏点击两路径共用；点击路径本就
    // 展开着组，此处对其是幂等 no-op。仅在页面切换帧补一次，用户随后手动
    // 收起不被强制回展（g_autoExpandedForPage 闩）。
    const ::core::PageId currentPageNow = router->currentPage();
    if (currentPageNow != g_autoExpandedForPage) {
        for (const auto& entry : ::app::registry::kModuleRegistry) {
            if (entry.page == currentPageNow && entry.parent.has_value()) {
                g_expandedGroups.insert(*entry.parent);
                break;
            }
        }
        g_autoExpandedForPage = currentPageNow;
    }

    // bug252（clue92 老板终裁：全屏 = 侧栏 0 宽全隐、不画 logo）：0 宽时整棵侧栏
    // 子树早退。若照旧绘制，折叠态图标/品牌 logo 会按负 x 落位（(kWidth−logoSize)/2
    // = −24），左缘露出「半截 logo」——老板否掉 72px 折叠方案的残留形态正是它。
    // 113-6 自动展开闩已在上方推进，退出全屏后导航态不跳账。
    if (sidebarWidth() <= 0.0f) return;
    const float kWidth = sidebarWidth();

    const auto& t = ::core::theme::current();
    // W0.5：透明语义常量（非可主题化色，W0.5 误标为待清理字面量）。
    // 用途：inactive hit 底 + toggle 底 + active 遮让 SelectionBar（让 SelectionBar 盖底）。
    // 不抽到 theme.h 的 wechat.transparent 槽位（W3 不动 theme.h），保留为局部语义常量。
    const eui::Color kTransparent{0.0f, 0.0f, 0.0f, 0.0f};
    const bool collapsed = g_sidebarCollapsed;

    static const bool mounted = [] {
        // 口径 = 登记表条目数（一行一个 Router 页面，含分组父项与子页），不写死数字、
        // 不取 modules().size()（后者只数已启用实例，会随模块开关变动且漏掉分组父项）
        constexpr std::size_t kPageEntryCount =
            sizeof(::app::registry::kModuleRegistry) /
            sizeof(::app::registry::kModuleRegistry[0]);
        LOG_INFO("侧边栏导航已挂载（" + std::to_string(kPageEntryCount) +
                 " 个模块入口，宽 220/72 可折叠）");
        return true;
    }();
    (void)mounted;
    // need171 高度自适应三档（2026-11-03 设计文档 §2.2）+ v2 动态尺寸（§10.1/§10.2）：
    // 判定基准 = 侧栏可用高 H（drawSidebar 收到的 screen.height：窗高 − 自绘标题条
    // 40 / macOS 系统标题栏 30，compose 的 bodyScreen 已统一扣除）。离散三档、档位
    // 切换无过渡动画：
    //   正常档 H≥712：topPad 32 / brandNavGap 28 / gap 4 / itemH 40（导航起点 116）；
    //     logo 48（§10.1）、钮 32 / 预留区 48（§10.2）
    //   紧凑档 680≤H<712：16 / 16 / 4 / 40（起点 88；最小窗 1200×720 → H=680 落此
    //     档）；logo 44、钮 28 / 预留区 44
    //   极紧档 H<680：12 / 12 / 2 / 36（起点 80）；logo 40、钮 28 / 预留区 44
    // 压缩顺序：先压品牌区上下留白 → 再压行距 → 最后压行高；品牌行 56 恒不缩
    //（锚点不塌），logo/钮径随档（两态一致不回退——bug165 裁决保持）。分档几何用
    // 常量不走 token：矮窗帧 theme::setWindowHeight 会重建字号 token，布局分档必须
    // 与字号档解耦。
    const float availH = screen.height;
    float topPad = 32.0f;
    float brandNavGap = 28.0f;
    float gap = 4.0f;
    float itemH = ui_kit::kNavItemH;  // 40（正常/紧凑档）
    float logoSize = 48.0f;   // §10.1：正常档 48（两态一致）
    float toggleSize = 32.0f;     // §10.2：正常档直径 32
    float toggleZoneH = 48.0f;    // §10.2：正常档底部预留区 48
    if (availH < 680.0f) {
        topPad = 12.0f;
        brandNavGap = 12.0f;
        gap = 2.0f;
        itemH = 36.0f;
        logoSize = 40.0f;
        toggleSize = 28.0f;
        toggleZoneH = 44.0f;
    } else if (availH < 712.0f) {
        topPad = 16.0f;
        brandNavGap = 16.0f;
        logoSize = 44.0f;
        toggleSize = 28.0f;
        toggleZoneH = 44.0f;
    }
    const float padX = t.sizes.spacingLarge;
    constexpr float kIconW = 28.0f;
    constexpr float kItemInset = 12.0f;  // 内容距高亮条左缘内边距

    // 品牌块高 56 恒定（三档都不缩：锚点不塌）
    constexpr float kBrandH = 56.0f;

    ui.stack("app.sidebar")
        .size(kWidth, screen.height)
        .content([&] {
            // 侧栏底色（sidebarBg：light #F0FDF7 / dark #0F1F18），z 序在最底，
            // divider/brand/nav/toggle 都在其上，仅染侧栏、不动页面区/白卡。
            // UAT2 B 批：tint 的 alpha 随窗口效果档位（core::window_effect_state 单值，
            // 写口 = settings applyWindowVisuals）——磨砂 0.15 透 Acrylic backdrop、
            // 半透 0.5、off 实色 1.0；off/translucent 档保留 §4.8 微绿灰观感，
            // 磨砂档侧栏不再把 backdrop 挡死。
            const float sidebarBgAlpha = ::core::window_effect_state::surfaceAlpha(
                ::core::window_effect_state::current());
            ui.rect("app.sidebar.bg")
                .size(kWidth, screen.height)
                .color(eui::Color{t.colors.sidebarBg.r, t.colors.sidebarBg.g,
                                  t.colors.sidebarBg.b, sidebarBgAlpha})
                .build();
            // 右缘分割线：borderDefault
            ui.rect("app.sidebar.divider")
                .position(kWidth - 1.0f, 0.0f)
                .size(1.0f, screen.height)
                .color(t.colors.borderDefault)
                .radius(t.sizes.radiusSmall)
                .build();

            // brand 行（UAT3 #9 + 图标批 2026-10-20）：折叠/展开两态统一为居中几何
            // logo——C 方案终端窗 >_（ui_kit::drawDevDeskLogo 矢量绘制，几何契约与
            // assets/icon.ico 同源；fa-laptop-code 字形退役）。行元素仍是整行 box
            // (0, topPad, kWidth, kBrandH)：test_app_sidebar 钉的 brand frame.y=40
            // 防跳动断言不动；品牌文本由自绘标题栏承载。
            // UAT5：logo 36→48 按用户拍板放大（「不够大」），行高 32→56 随之加高
            //（48 瓦在 56 行内居中上下各留 4px，不再溢出），首个导航起点
            // 32+56+28=116 连锁（test_app_sidebar 各导航 y 断言同步）。
            ui.stack("app.sidebar.brand")
                .position(0.0f, topPad)
                .size(kWidth, kBrandH)
                .content([&] {
                    // bug165 两态一致裁决（2026-11-03 §2.1）+ v2 动态三档（§10.1）：
                    // logo 随可用高 H 分档 48/44/40（正常/紧凑/极紧），两态（展开/
                    // 折叠）同值不回退；kBrandH 56 与导航起点（116/88/80）均不变，
                    // test_app_sidebar 既有 brand/导航 y 断言零波及。
                    ui_kit::drawDevDeskLogo(
                        ui, "app.sidebar.brand.mark",
                        (kWidth - logoSize) * 0.5f, (kBrandH - logoSize) * 0.5f,
                        logoSize, t.colors.primary, t.colors.surface);
                })
                .build();

            // 导航滚动区起点（§11.2）：brand 行下缘 = viewport 顶（品牌区锚定不滚）
            const float navStart = topPad + kBrandH + brandNavGap;

            // 导航项渲染：提取为 lambda（work/system 两组共用，避免重复代码）。
            // active 态视觉走 SelectionBar（§4.3：2px 左绿竖线 + selectedBg + textPrimary），
            // hit 区透明+onClick 承担点击；inactive 透出侧栏底，hover 浅灰反馈。
            // 登记表行查找：分组树只认登记表 parent 列（唯一元数据源，不另建清单）
            auto registryEntry = [](::core::PageId page)
                -> const ::app::registry::ModuleEntry* {
                for (const auto& e : ::app::registry::kModuleRegistry) {
                    if (e.page == page) return &e;
                }
                return nullptr;
            };

            // 底部折叠开关预留区（§11.1 转义）：旧「让位线守卫藏行」全面退役——
            // 被藏内容不可达违背 need186 老板令（显示不全必须可滚）。守卫公式
            // kNavFoldLimit = H − zone − gap 转义为滚动区 viewport 下界锚
            //（viewportBottom = H − toggleZoneH）：行物理画不进按钮预留区由
            // viewport 裁剪天然保障（141 回归缝职责继承），被裁行滚动触达。
            //
            // 行规划（§11.1）：先把全部导航行规划成表（规划/绘制同源，contentH 与
            // 行几何零漂移），再在滚动画布上按表绘制；溢出即滚，不再越线不画。
            struct NavRowPlan {
                bool divider = false;  // 组间分割线占位（1px + gap）
                bool groupHeader = false;
                ::core::Module* module = nullptr;  // 导航项（groupHeader 时为空）
                const ::app::registry::ModuleEntry* entry = nullptr;  // 父项元数据
                bool expanded = false;
                bool holdsCurrent = false;
                float indent = 0.0f;
            };
            // 二级菜单缩进（仅展开态生效；折叠态图标居中，缩进无意义且会把命中区/
            // 选中条推出图标——折叠态一律按 0 参与 hit/sel 几何）
            constexpr float kChildIndent = 16.0f;
            auto isSystemGroup = [](::core::PageId page) {
                return page == ::core::PageId::Settings ||
                       page == ::core::PageId::About;
            };
            std::vector<NavRowPlan> navPlan;
            float contentH = 0.0f;
            bool navHasDivider = false;
            {
                bool dividerPlanned = false;
                bool sawWork = false;
                std::set<::core::PageId> plannedGroups;
                const auto* currentEntry = registryEntry(router->currentPage());
                for (auto* m : router->modules()) {
                    if (!m) continue;
                    const ::core::PageId page = m->pageId();
                    const bool isSystem = isSystemGroup(page);
                    // work → system 首次切换时规划组间 divider（仅当两组都非空：
                    // sawWork && isSystem；注册表恒有两组，实际恒规划）
                    if (!dividerPlanned && sawWork && isSystem) {
                        NavRowPlan d;
                        d.divider = true;
                        navPlan.push_back(d);
                        contentH += 1.0f + gap;
                        dividerPlanned = true;
                        navHasDivider = true;
                    }
                    if (!isSystem) sawWork = true;
                    // 有父级声明的模块 → 先规划父项（同组只规划一次）再规划子项；
                    // 无父级 → 原平铺路径。展开态唯一判定 = sidebarGroupExpanded
                    //（与 toggleSidebarGroup 同一状态源，默认折叠）
                    const auto* row = registryEntry(page);
                    if (row != nullptr && row->parent.has_value()) {
                        const auto* parent = registryEntry(*row->parent);
                        if (parent != nullptr) {
                            const bool expanded =
                                sidebarGroupExpanded(parent->page);
                            const bool holdsCurrent =
                                currentEntry != nullptr &&
                                currentEntry->parent.has_value() &&
                                *currentEntry->parent == parent->page;
                            if (plannedGroups.insert(parent->page).second) {
                                NavRowPlan h;
                                h.groupHeader = true;
                                h.entry = parent;
                                h.expanded = expanded;
                                h.holdsCurrent = holdsCurrent;
                                navPlan.push_back(h);
                                contentH += itemH + gap;
                            }
                            if (expanded) {
                                NavRowPlan c;
                                c.module = m;
                                c.indent = kChildIndent;
                                navPlan.push_back(c);
                                contentH += itemH + gap;
                            }
                            continue;
                        }
                    }
                    NavRowPlan f;
                    f.module = m;
                    navPlan.push_back(f);
                    contentH += itemH + gap;
                }
            }
            contentH = std::max(0.0f, contentH - gap);  // 末尾行距不入内容高

            // §11.2 滚动区几何（两态×三档同公式）：viewport = navStart → H−zone；
            // §11.2 offset 收敛：窗高/档位/分组收展变化后 clamp 到
            // [0, contentH−viewportH]，无动画
            const float viewportH =
                std::max(0.0f, availH - toggleZoneH - navStart);
            const float navMaxOffset =
                std::max(0.0f, contentH - viewportH);
            g_navScrollOffset =
                std::clamp(g_navScrollOffset, 0.0f, navMaxOffset);
            // contentKey：行数/档位/有无 divider 任一变化即重测（MeasureCache 缓存
            // 其余帧的测量，避免每帧空跑一遍绘制）
            const std::string navContentKey =
                "sidebar.nav." + std::to_string(navPlan.size()) + "." +
                std::to_string(static_cast<int>(itemH)) + "." +
                (navHasDivider ? "d" : "n");

            // 分组父项行：与导航项同几何；不可导航，点击只切换展开/收起。
            // 折叠侧栏下父项同样只画居中图标，子项按展开态继续列出。
            // holdsCurrent = 当前页是本分组的子页：父项保持「当前页在此组内」的
            // 选中态（默认折叠/手动收起后子项隐藏，选中信息只由父项承载）。
            // y 由调用方（画布行循环）按规划表推进，行不再越线让位（§11.1 藏行退役）。
            auto renderGroupHeader = [&](eui::Ui& inner, float y,
                                         const NavRowPlan& plan) {
                const ::app::registry::ModuleEntry& group = *plan.entry;
                const bool expanded = plan.expanded;
                const bool holdsCurrent = plan.holdsCurrent;
                const std::string id =
                    std::string("app.sidebar.item.") +
                    ::core::pageIdToString(group.page);
                inner.rect(id + ".hit")
                    .position(padX, y)
                    .size(kWidth - padX * 2.0f, itemH)
                    .states(kTransparent,
                            holdsCurrent ? kTransparent : t.colors.hover,
                            holdsCurrent ? kTransparent : t.colors.primarySoftBg)
                    .radius(t.sizes.radiusSmall)
                    .onClick([g = group.page] { toggleSidebarGroup(g); })
                    .build();
                // 选中条只在「子项没被列出」时画：组展开时子项自己画一条，父项再画
                // 就是一二级同时亮（B8 G2）；组收起时父项是当前页唯一入口，指示必须
                // 留着（折叠侧栏同理——折叠侧栏下子项仍按展开态列出，几何与父项重叠）。
                if (holdsCurrent && !expanded) {
                    ui_kit::selectionBar(inner, id + ".sel", std::string{}, padX,
                                         y, kWidth - padX * 2.0f, itemH, t);
                }
                if (collapsed) {
                    inner.text(id + ".icon")
                        .position((kWidth - kIconW) * 0.5f, y)
                        .size(kIconW, itemH)
                        .icon(group.iconGlyph)
                        .fontSize(t.sizes.fontSizeWeChat)
                        .horizontalAlign(eui::HorizontalAlign::Center)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(holdsCurrent ? t.colors.primary
                                            : t.colors.textSecondary)
                        .build();
                } else {
                    // 盒宽 20→14 且左移 6：字形本身窄，居中在 20px 盒里偏左又贴右缘，
                    // 读起来像被切掉（B8 G1）。
                    constexpr float kChevronW = 14.0f;
                    inner.text(id + ".icon")
                        .position(padX + kItemInset, y)
                        .size(kIconW, itemH)
                        .icon(group.iconGlyph)
                        .fontSize(t.sizes.fontSizeWeChat)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(holdsCurrent ? t.colors.primary
                                            : t.colors.textSecondary)
                        .build();
                    inner.text(id + ".label")
                        .position(padX + kItemInset + kIconW, y)
                        .size(kWidth - padX * 2.0f - kItemInset - kIconW -
                                  kChevronW - 8.0f,
                              itemH)
                        .text(group.shortName)
                        // 142：分组头文本与其它侧栏条目同接线（fontSizeWeChat 16 档，
                        // token 重建时已乘 fontScale）——此前用 fontSizeBody 14 档，
                        // 与平铺项/二级项恒差一档，字号缩放后观感即「缩放没覆盖本组」
                        .fontSize(t.sizes.fontSizeWeChat)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(holdsCurrent ? t.colors.textPrimary
                                            : t.colors.textSecondary)
                        .build();
                    inner.text(id + ".chevron")
                        .position(kWidth - padX - kChevronW - 6.0f, y)
                        .size(kChevronW, itemH)
                        .icon(eui::utf8(expanded ? 0xF077 : 0xF078))  // chevron-up/down
                        .fontSize(t.sizes.fontSizeWeChat - 3.0f)  // 随 label 同档（基准−3 口径不变）
                        .horizontalAlign(eui::HorizontalAlign::Left)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(t.colors.textTertiary)
                        .build();
                }
            };

            auto renderNavItem = [&](eui::Ui& inner, float y, ::core::Module* m,
                                     float indent) {
                const ::core::PageId page = m->pageId();
                const bool active = (page == router->currentPage());
                const std::string id =
                    std::string("app.sidebar.item.") +
                    ::core::pageIdToString(page);
                const float effIndent = collapsed ? 0.0f : indent;

                // 命中区：normal 透明（active 时 SelectionBar 盖底；inactive 透出侧栏底）；
                // hover/pressed：active 透明（不遮 SelectionBar），inactive 浅灰/深灰
                inner.rect(id + ".hit")
                    .position(padX + effIndent, y)
                    .size(kWidth - padX * 2.0f - effIndent, itemH)
                    .states(kTransparent,
                            active ? kTransparent : t.colors.hover,
                            active ? kTransparent : t.colors.primarySoftBg)
                    .radius(t.sizes.radiusSmall)
                    .onClick([router, page] { router->navigateTo(page); })
                    .build();

                // active 态叠 SelectionBar（§4.3：3×18 左绿竖条 + 白卡胶囊）。
                // selectionBar 不传文字——其 .text 节点 z 序浮在 icon/label 之上导致重叠；
                // 文字由下方 .label 画（icon 同理）。selectionBar 只负责胶囊 + 竖条。
                // accentOffsetX=-10：竖条退到胶囊外侧的栅边留白里（原型 ::before 同位），
                // 贴胶囊左缘那条读起来像表格行高亮。
                if (active) {
                    ui_kit::selectionBar(inner, id + ".sel", std::string{},
                                         padX + effIndent, y,
                                         kWidth - padX * 2.0f - effIndent, itemH, t,
                                         -10.0f);
                }

                // 图标与文字分两个元素：Icon 字体（FontAwesome）不能和正文字体混排在一个 Text 里。
                // active 统一 primaryText：primary 作 16px 图标只有 2.38:1，非文本对比也过不了 3:1。
                if (collapsed) {
                    inner.text(id + ".icon")
                        .position((kWidth - kIconW) * 0.5f, y)
                        .size(kIconW, itemH)
                        .icon(m->iconGlyph())
                        .fontSize(t.sizes.fontSizeWeChat)
                        .horizontalAlign(eui::HorizontalAlign::Center)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(active ? t.colors.primaryText : t.colors.textSecondary)
                        .build();
                } else {
                    inner.text(id + ".icon")
                        .position(padX + kItemInset + effIndent, y)
                        .size(kIconW, itemH)
                        .icon(m->iconGlyph())
                        .fontSize(t.sizes.fontSizeWeChat)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(active ? t.colors.primaryText : t.colors.textSecondary)
                        .build();
                    inner.text(id + ".label")
                        .position(padX + kItemInset + kIconW + effIndent, y)
                        .size(kWidth - padX * 2.0f - kItemInset - kIconW - effIndent,
                              itemH)
                        .text(m->displayName())
                        .fontSize(t.sizes.fontSizeWeChat)
                        .fontWeight(active ? 600 : 400)  // 原型 .nav-item.active 600
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(active ? t.colors.primaryText : t.colors.textSecondary)
                        .build();
                }
            };

            // 导航滚动区（need186 §11）：EUI scrollView 全站 idiom——行表在画布
            //（固定 contentH 的 stack）上按规划绘制，viewport 裁剪溢出、滚轮/拖拽
            // 触达（组件内置惯性/跟手）。滚轮 step = 行步进 itemH+gap（§11.4：
            // 44/38，一格一行）。bug188（老板 2026-11-05 裁决）：滚动能力保留、
            // 滚动条任何视觉全无——scrollbarWidth(0) 使滚动子组件及 track/thumb
            // 零宽构建，滚轮/惯性/拖拽走滚动根 scrollState 与滚动条宽度解耦；折
            // 叠 72 态同链生效，全屏 0 宽态在上方 sidebarWidth()<=0 早退整树不建。
            // 设计稿顶部/底部 16px 渐隐遮罩不做：EUI 无渐变 API，纯 DSL 不可达
            //（偏差已记池）。
            components::scrollView(ui, "app.sidebar.nav")
                .position(0.0f, navStart)
                .size(kWidth, viewportH)
                .gap(0.0f)  // 行距由画布内绝对坐标承载（列 gap 不参与）
                .step(itemH + gap)
                .theme(core::theme::componentTokensWeChat(t))
                .scrollbarOverlay(true)
                .scrollbarWidth(0.0f)  // bug188：滚动条视觉全无（轨道/滑块零宽不渲染）
                .offset(g_navScrollOffset)
                .onChange([](float v) { g_navScrollOffset = v; })
                .contentKey(navContentKey)
                .content([&](eui::Ui& inner, float innerW, float) {
                    // 画布：单一流式子件、固定高 contentH（测量值=规划值，零漂移）；
                    // 行在画布内绝对定位（画布局部坐标 = 旧侧栏 y − navStart）
                    inner.stack("app.sidebar.nav.canvas")
                        .size(innerW, contentH)
                        .content([&] {
                            float y = 0.0f;
                            for (const auto& plan : navPlan) {
                                if (plan.divider) {
                                    inner.rect("app.sidebar.groupdiv")
                                        .position(padX, y)
                                        .size(innerW - padX * 2.0f, 1.0f)
                                        .color(t.colors.borderDefault)
                                        .build();
                                    y += 1.0f + gap;
                                } else if (plan.groupHeader) {
                                    renderGroupHeader(inner, y, plan);
                                    y += itemH + gap;
                                } else {
                                    renderNavItem(inner, y, plan.module,
                                                  plan.indent);
                                    y += itemH + gap;
                                }
                            }
                        })
                        .build();
                })
                .build();

            // 折叠开关（底部固定）：need172 v2（设计文档 §10.2）——直径按档 32/28
            // 正圆，预留区 48/44 内垂直居中（toggleY = H−40 / H−36，上下各 8）；
            // 水平：折叠 (72−size)/2（20/22）、展开 padX=16 对齐导航左缘。码点字号
            // fontSizeSmall(12)（§10.5-3，随钮缩小降档）。常态 1px borderDefault
            // 圆框常显 + textSecondary 图标；hover：primarySoftBg 底 + primaryHover
            // 描边 + primary 图标 + scale 1.04（150ms）；按压：primary 描边 +
            // scale 0.96；移出/松手回弹 180ms OutBack。形变与变色走「每帧换目标值
            // + transition 平滑」（jelly 坑①②：.animate 只是允许动，目标值必须
            // compose 期每帧重给）。EUI 每元素单 Transition，按压段 100ms Out 并入
            // 同曲线兜（jelly.h 全段单档先例）。
            // 热区与绘制解耦（§10.2）：hover/点击命中 = 绘制区外扩 4px
            //（40/36 ≥32px 桌面最小可点约定），视觉缩小不牺牲命中率——交互回调
            // 全在独立透明热区 rect（active 导航项三态全透明仍可点中先例）。
            const float toggleY = availH - toggleZoneH + 8.0f;  // 区顶 + 8 居中
            const float toggleX =
                collapsed ? (kWidth - toggleSize) * 0.5f : padX;
            const bool toggleHovered = g_toggleHovered;
            const bool togglePressed = g_togglePressed;
            const bool toggleActive = toggleHovered || togglePressed;
            const float toggleScale =
                togglePressed ? 0.96f : toggleHovered ? 1.04f : 1.0f;
            const core::Transition toggleTransition = core::Transition::make(
                toggleHovered ? 0.15f : 0.18f,
                toggleHovered ? core::Ease::OutCubic : core::Ease::OutBack);
            ui.rect("app.sidebar.toggle")
                .position(toggleX, toggleY)
                .size(toggleSize, toggleSize)
                .color(toggleActive ? t.colors.primarySoftBg : kTransparent)
                .border(1.0f, togglePressed  ? t.colors.primary
                              : toggleHovered ? t.colors.primaryHover
                                              : t.colors.borderDefault)
                .radius(toggleSize * 0.5f)  // 半径=半宽 → 正圆
                .transformOrigin(0.5f, 0.5f)  // 缩放中心在按钮中心（jelly 坑③）
                .scale(toggleScale)
                .transition(toggleTransition)
                .animate(core::AnimProperty::Color | core::AnimProperty::Border |
                         core::AnimProperty::Transform)
                .build();
            ui.text("app.sidebar.toggle.icon")
                .position(toggleX, toggleY)
                .size(toggleSize, toggleSize)
                .icon(eui::utf8(collapsed ? 0xF0C9 : 0xF100))  // fa-bars / fa-angles-left
                .fontSize(t.sizes.fontSizeSmall)  // §10.2：fontSizeSmall(12)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(toggleActive ? t.colors.primary : t.colors.textSecondary)
                .transformOrigin(0.5f, 0.5f)
                .scale(toggleScale)  // 图标随按钮同形变（同几何同中心）
                .transition(toggleTransition)
                .animate(core::AnimProperty::TextColor |
                         core::AnimProperty::Transform)
                .build();
            // 热区（绘制区外扩 4px，§10.2）：置于视觉元素之后 = 本组最顶层命中；
            // 三态全透明（不参与视觉）
            ui.rect("app.sidebar.toggle.hot")
                .position(toggleX - 4.0f, toggleY - 4.0f)
                .size(toggleSize + 8.0f, toggleSize + 8.0f)
                .states(kTransparent, kTransparent, kTransparent)
                .onHover([](bool hovered) {
                    if (hovered && !g_toggleHovered) {
                        // hover 进入沿触发一次（验收 5/10：停在上面不循环，移出再进重触发）
                        g_toggleParticleStart = std::chrono::steady_clock::now();
                    }
                    g_toggleHovered = hovered;
                })
                .onPress(
                    [](const auto&, const auto&) { g_togglePressed = true; })
                .onRelease(
                    [](const auto&, const auto&) { g_togglePressed = false; })
                .onClick([] {
                    setSidebarCollapsed(!sidebarCollapsed(), true);
                })
                .build();

            // 弥散绿粒子 v2（need172 §10.3）：hover 进入沿一次性迸发 12 颗（30° 均布
            // + hash ±10° 抖动），生命 800ms + 每颗 hash 延迟 0~80ms（迸发层次，
            // ≤880ms 全灭尽自止），透明度 0.75→0、末端缩放 0.7；起点 = 钮半径+2
            // 向径向外弥散 14~26px（冲出圆框才可见），粒径 4~6px 正圆。颜色：浅色档
            // kParticleLight 三色轮替（第三色 #06AD56 深档拉开明度）、深色档
            // primary/primaryHover 双色。性能口径：hover 驱动一次性迸发（≤12 个
            // 矩形元素、880ms 自止），非持续动画、无常驻帧泵；非活跃帧零粒子零泵。
            if (g_toggleParticleStart.has_value()) {
                const float elapsed =
                    std::chrono::duration<float>(
                        std::chrono::steady_clock::now() -
                        *g_toggleParticleStart)
                        .count();
                if (elapsed < 0.88f) {
                    const float cx = toggleX + toggleSize * 0.5f;
                    const float cy = toggleY + toggleSize * 0.5f;
                    for (int i = 0; i < 12; ++i) {
                        const float delay =
                            fluidHash01(i * 23 + 7) * 0.08f;  // 0~80ms
                        if (elapsed <= delay) continue;  // 未起爆不建元素
                        const float tt =
                            std::clamp((elapsed - delay) / 0.8f, 0.0f, 1.0f);
                        const float eased = core::applyEase(core::Ease::OutCubic, tt);
                        const float ang =
                            (i * 30.0f +
                             (fluidHash01(i * 13 + 1) - 0.5f) * 20.0f) *
                            0.0174533f;  // deg→rad（30° 均布 + ±10° 抖动）
                        const float spread =
                            14.0f + 12.0f * fluidHash01(i * 17 + 2);  // 14~26px
                        const float pr =
                            2.0f + fluidHash01(i * 19 + 3);  // 半宽 2~3（直径 4~6）
                        const float size =
                            pr * 2.0f * (1.0f - 0.3f * eased);  // 末端缩放 0.7
                        const float d = toggleSize * 0.5f + 2.0f +
                                        spread * eased;  // 起点半径=钮半径+2
                        const eui::Color& pc =
                            t.isDark ? ((i % 2 == 0) ? t.colors.primary
                                                     : t.colors.primaryHover)
                                     : kParticleLight[i % 3];
                        ui.rect("app.sidebar.toggle.particle." +
                                std::to_string(i))
                            .position(cx + std::cos(ang) * d - size * 0.5f,
                                      cy + std::sin(ang) * d - size * 0.5f)
                            .size(size, size)
                            .radius(size * 0.5f)  // 半径=半宽 → 正圆（fluid 卫星同款）
                            .color(pc)
                            .opacity(0.75f * (1.0f - eased))
                            .build();
                    }
                    // 帧泵（fluid driver 同款先例）：粒子位置每帧按壁钟重算，泵只
                    // 在 880ms 生命期内保持出帧；到期帧起本分支整体不建（自止）
                    ui.stack("app.sidebar.toggle.particle.driver")
                        .size(1.0f, 1.0f)
                        .onFrame([](float) {})
                        .build();
                } else {
                    g_toggleParticleStart.reset();  // 生命到期：回空闲零元素零泵
                }
            }
        })
        .build();
}

// ── 动态背景（UAT6 主窗活体流质）：纯 DSL 层组合 ──
// UAT6.1：7 层半径缩至原 60% 的小晕 + 每层 2 颗卫星；相位/周期错开分布，
// 观感从「几大团」变「多小晕流动」。帧泵与门控同旧版。z 序最底，正文可读
// 性零影响。B5 起 14 层、角速度 0.070~0.166 rad/s（提速 ×2），整圈 38~90s。
void drawFluidBackground(eui::Ui& ui, const eui::Screen& screen) {
    if (!fluidBackgroundEnabled()) return;  // 开关关：零元素零泵（零成本）
    if (!fluidWindowVisible()) return;      // 铁律：最小化/藏托盘 → 不挂泵不推进

    const auto& t = ::core::theme::current();

    constexpr int kHalos = 14;           // B5（2026-11-02）：7→14 小晕，铺满窗口不挤成一坨
    constexpr float kMaxAlpha = 0.19f;   // 单层峰值（2026-11-01 重构 0.11→0.19：0.11 透不出卡面）
    constexpr int kSatsPerHalo = 3;      // 卫星/层（2→3 更细碎，液体感更强）
    // 层参数表（锚点 = 窗口比例；速度 rad/s；相位 rad 防共漂；半径 = 短边比例）
    // B5 提速 ×2：速度翻倍后漂移周期 ~75s→~38s，振幅同步下调（0.16/0.13→0.13/0.10）
    // 防「在绕圈」观感（方案 §10.6 拍板③，一次改完不分步）。
    constexpr float kAnchorX[kHalos] = {
        0.50f, 0.50f, 0.24f, 0.76f, 0.12f, 0.88f, 0.36f,   // 原 7
        0.62f, 0.38f, 0.68f, 0.32f, 0.05f, 0.95f, 0.44f};  // 新 7 补空白区
    constexpr float kAnchorY[kHalos] = {
        0.46f, 0.56f, 0.30f, 0.72f, 0.80f, 0.18f, 0.88f,   // 原 7
        0.38f, 0.66f, 0.88f, 0.24f, 0.52f, 0.42f, 0.08f};  // 新 7
    constexpr float kSpeedX[kHalos] = {
        0.084f, 0.072f, 0.142f, 0.116f, 0.102f, 0.134f, 0.088f,
        0.126f, 0.098f, 0.158f, 0.110f, 0.146f, 0.092f, 0.138f};
    constexpr float kSpeedY[kHalos] = {
        0.070f, 0.098f, 0.128f, 0.166f, 0.076f, 0.110f, 0.144f,
        0.120f, 0.084f, 0.152f, 0.096f, 0.136f, 0.104f, 0.078f};
    constexpr float kPhase[kHalos] = {
        0.0f, 1.7f, 3.1f, 4.9f, 2.3f, 5.8f, 4.0f,
        0.9f, 2.6f, 3.7f, 5.2f, 1.4f, 6.1f, 4.5f};
    // 半径扩到 14 个固定值（不引 hash：帧间稳定不闪）。极值 0.372/0.124 → 极值比
    // 恒 3.0（探针按此断言）。最大圆放 j=3 而非 j=0：alpha 随 j 递减，j=0 最浓，
    // 最大圆放最前会与最浓 alpha 叠加显得突兀（方案 §10.6 建议）。
    constexpr float kRadiusFrac[kHalos] = {
        0.240f, 0.124f, 0.264f, 0.372f, 0.216f, 0.204f, 0.228f,  // 原 7（最大移到 j=3）
        0.190f, 0.330f, 0.280f, 0.150f, 0.350f, 0.210f, 0.300f}; // 新 7

    const float phase = fluidPhaseSeconds();
    const float minDim = std::min(screen.width, screen.height);
    for (int j = 0; j < kHalos; ++j) {
        // 漂移中心 = 锚点 + 异频异相振幅（层间不共振，慢到只有「液面在流」而无轨迹感）
        const float cx = screen.width * kAnchorX[j] +
                         screen.width * 0.13f *
                             std::sin(kSpeedX[j] * phase + kPhase[j]);
        const float cy = screen.height * kAnchorY[j] +
                         screen.height * 0.10f *
                             std::cos(kSpeedY[j] * phase + kPhase[j]);
        const float r = minDim * kRadiusFrac[j];
        // 内浓外淡：14 层时原式 (1 - 0.13j) 在 j=13 算出负 alpha（圆不可见/渲染异常），
        // 改每档 0.05 + 下限 0.30（方案 §10.6）。
        const float alpha =
            kMaxAlpha * std::max(0.30f, 1.0f - 0.05f * static_cast<float>(j));
        // 浅色档三色轮替：同族不同明度 → 液面深浅流动，而非一整块绿
        //（kLightHalo 已上提至匿名 namespace，与侧栏按钮粒子共用）。
        // 暗色档沿用双色（浅绿压深底会发灰光）。
        const eui::Color& color =
            t.isDark ? ((j % 2 == 0) ? t.colors.primary : t.colors.primaryHover)
                     : kLightHalo[j % 3];
        ui.rect("app.fluid.halo." + std::to_string(j))
            .position(cx - r, cy - r)
            .size(r * 2.0f, r * 2.0f)
            .color(color)
            .radius(r)  // 半径 = 半宽 → 正圆（splash 墨染同款手法）
            .opacity(alpha)
            .build();
        // 确定性 hash 卫星：层心外慢轨道漂移的墨点，角度/距离/大小全由索引
        // hash 定死，帧间稳定不闪烁（漂移只来自相位项）
        for (int b = 0; b < kSatsPerHalo; ++b) {
            const float ang = fluidHash01(j * 31 + b * 7 + 2) * 6.2831853f +
                              phase * (0.020f + 0.011f * fluidHash01(j * 13 + b * 5 + 3));
            const float dist =
                r * (0.62f + 0.30f * fluidHash01(j * 17 + b * 3 + 4));
            const float br = r * (0.055f + 0.040f * fluidHash01(j * 19 + b * 11 + 5));
            ui.rect("app.fluid.sat." + std::to_string(j) + "." +
                    std::to_string(b))
                .position(cx + std::cos(ang) * dist - br,
                          cy + std::sin(ang) * dist - br)
                .size(br * 2.0f, br * 2.0f)
                .color(color)
                .radius(br)
                .opacity(alpha * 0.45f)
                .build();
        }
    }
    // 帧泵（md.tree.anim.driver / splash.ink.anim.driver 同款先例）：常驻 1x1 空
    // onFrame 元素——只借它让 runtime 置 animating_/composeRequested_ 保持持续
    // 出帧（流质背景须逐帧重渲）。相位推进不再在此回调：71ce7d4c 起相位走壁钟
    // fluidPhaseSeconds()，回调体只写死 g_fluidPhase 已随 W3.5 审计 B（F4）清空。
    ui.stack("app.fluid.anim.driver")
        .size(1.0f, 1.0f)
        .onFrame([](float) {})
        .build();
}

void compose(eui::Ui& ui, const eui::Screen& screen) {
    // --minimized 启动（UAT6 开机自启动）：首帧一次性走 close-to-tray 同款
    // WM_CLOSE→隐藏路径（点标题栏 X 藏托盘是同一入口 app::closeMainWindow；主窗
    // 句柄此刻已由 EUI-NEO 主循环回写，SendMessage 同步派发 → close callback 置
    // hideToTrayRequested，下一帧循环顶隐藏到托盘）。须在 splash 之前：旗标在身
    // 时 splash 不该露脸。取走即清：仅启动后第一帧生效，托盘左键/Show 正常唤起。
    if (app::assembly::consumePendingMinimizedStartup()) {
        app::closeMainWindow();
    }
    // 窗体记忆记账（UAT6）：每帧记录还原态客户区逻辑尺寸 + 最大化标志（退出落盘的
    // 数据源）。须在 splash 早退之前——splash 帧也是主窗帧，启动即最大化的会话靠它
    // 记下最大化态（尺寸则由 main.cpp 的播种兜底）。最大化帧不记尺寸（保留还原态）
    ::core::window_geometry::noteFrame(screen.width, screen.height,
                                       app::mainWindowMaximized());
    // H19 窗高驱动字号封顶：每帧喂窗口逻辑高；封顶档变化时 theme 内部重建 token + 广播
    ::core::theme::setWindowHeight(screen.height);
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
    // 需-9 用量告警判定驱动（M3 同款装配层每帧 poll）：判定与投递不依赖记忆页
    // 可见性（PanelState 内部 5s 节流；出口 = toast 桥 + platform::notify 在模块内）。
    for (::core::Module* m : router->modules()) {
        if (m->pageId() != ::core::PageId::Memory) continue;
        if (auto* mem = dynamic_cast<::module::memory::MemoryModule*>(m)) {
            mem->pollUsageAlerts();
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

    // 动态背景（UAT6 活体流质）：app 根层、所有内容前画（z 序最底；splash 早退
    // 在先 → 启动 splash 帧不画，splash 观感不动）。门控见 drawFluidBackground
    //——开关关 / 最小化 / 藏托盘的帧零元素零泵（省电零开销），恢复可见继续。
    drawFluidBackground(ui, screen);
    const bool chrome = customTitlebarActive();
    // macOS：内容区已延伸进标题栏（FSCV，内容从窗口最顶开始）→ 系统标题栏那一条
    // 仍浮在左上角（红绿灯 + 窗口标题），内容必须让出它，否则压住侧栏品牌块。
    // Windows 走自绘标题栏（chrome=true），自带 40px 占位，无需在此让位。
#ifdef __APPLE__
    // macOS 系统标题栏那一条的高度（红绿灯所在）；内容需让出它。
    const float kSystemTitleBarInset = 30.0f;
#else
    const float kSystemTitleBarInset = 0.0f;
#endif
    const float bodyTop = chrome ? titlebarHeight() : kSystemTitleBarInset;
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
    // 2026-11-01 F-09：时长与图标全站统一（此前此处 4.5s / 0xF05A 是全站唯一例外）
    ui_kit::drawToast(ui, screen, "app.notice", g_appNotice,
                      core::theme::componentTokensWeChat(core::theme::current()));
    // 池168（2026-11-04 老板撤销绿白虚框改细黑边）：窗口边框——全窗最外缘非交互
    // 覆盖，放最后保证不被任何内容盖住；Windows 即真实窗框位置，macOS 为系统窗框
    // 内侧内描边。圆角按 DWM 8 物理px/contentScale 折算贴窗体圆角。最大化态隐藏
    // （need184③，fork 已暴露 mainWindowMaximized）。
    ui_kit::drawWindowRing(ui, screen.width, screen.height,
                           core::theme::current(), app::mainWindowMaximized(),
                           app::contentScale());
}

} // namespace app
