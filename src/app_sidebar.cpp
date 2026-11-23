// 常驻侧边栏 + 折叠/展开实现（接口见 app_sidebar.h）。折叠态：宽 208→72，只显居中图标，
// label 不渲染；动画为瞬时切换（EUI-NEO AnimProperty 无 Width 档）。
#include "app_assembly.h"
#include "app_registry.h"  // 分组父项元数据（登记表 parent 列）
#include "app_sidebar.h"
#include "app_sidebar_internal.h"  // TU 内部缝：fluidHash01 / drawTitlebar（拆 TU 共用）

#include "core/component_theme.h"
#include "core/database.h"
#include "core/logging.h"
#include "core/router.h"
#include "core/theme.h"
#include "core/window_effect_state.h"  // 侧栏底 alpha 按窗口效果档位（UAT2 B 批）
#include "core/window_geometry.h"  // 窗体记忆账本（UAT6：compose 每帧记账还原态尺寸）
#include "core/window_layout.h"  // need:296 D4：回退态 inset/窗圆角三平台访问器
#include "module_agent/agent_module.h"
#include "module_memory/memory_module.h"  // 需-9：用量告警每帧判定驱动（M3 同款）
#include "platform/window_visibility.h"  // 主窗可见态（平台接口隔离，业务层零 Win32）
#include "quit_save_bridge.h"  // 退出丢稿确认弹窗（need:291）主窗 compose 尾部绘制
#include "wallpaper_delete_bridge.h"  // 壁纸素材删除确认弹窗 + 结果 drain（need356）
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
#include <core/render/render_backend.h>  // need217：fluid-fps 日志聚合 RenderFrameStats

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
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

// 收缩按钮弥散粒子浅色档三色（need172 v2 §10.3：弃用近底色的最浅档，第三色改
// primaryHover 深档 #06AD56 拉开明度跨度；仅粒子用，流质晕 kLightHalo 不随之改）
const eui::Color kParticleLight[3] = {eui::Color("#07C160"),
                                      eui::Color("#34D399"),
                                      eui::Color("#06AD56")};

// 收缩按钮 hover/按压/focus 态跨帧单值（EUI states 只管底色，border/scale/图标色
// 要按态每帧换目标值——g_closeButtonHovered 先例）
bool g_toggleHovered = false;
bool g_togglePressed = false;
bool g_toggleFocused = false;  // need172 v3：focusable 热区 onFocusChanged 写、compose 读
// 弥散绿粒子迸发起点（hover 进入沿写入一次；nullopt = 空闲零粒子零泵）
std::optional<std::chrono::steady_clock::time_point> g_toggleParticleStart;

// 导航滚动区 offset（need186 §11.2）：页面持有、compose 每帧 clamp 到
// [0, contentH−viewportH] 收敛（窗高/档位/分组收展变化后无动画收敛）；
// scrollView onChange 回写（滚轮/拖拽/点轨道），折叠/展开切换不触碰（viewport
// 两态同公式，offset 保持）。测试预置口 setNavScrollOffsetForTests
float g_navScrollOffset = 0.0f;

// bug368 方案 C（设计 §3.2）：当前项自动滚入的沿触发闩。路由切换（page 变化）、
// 窗口高变化（viewport/content 变化）、分组收展（行数变化）同处一个几何签名，
// 签名与页面任一变化才执行一次滚入——否则用户手动滚走后每帧都会被拉回当前项。
::core::PageId g_navAutoScrollPage = ::core::PageId::Home;
std::string g_navAutoScrollSig;
// 测试缝 setNavScrollOffsetForTests 预置 offset 的那一帧跳过自动滚入（只更新闩），
// 否则「预置滚到底」场景会被当前项滚入改成顶部——测试专用，生产路径永不置位。
bool g_navAutoScrollSuspend = false;
// chevron 交互态（hover 变色 / 按压缩放，设计 §8）
bool g_chevronUpHovered = false;
bool g_chevronDownHovered = false;
bool g_chevronUpPressed = false;
bool g_chevronDownPressed = false;

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
    g_toggleHovered = false;  // need172 按钮 hover/按压/focus/粒子态回厂（测试隔离）
    g_togglePressed = false;
    g_toggleFocused = false;
    g_toggleParticleStart.reset();
    g_navScrollOffset = 0.0f;  // §11 导航滚动 offset 回厂（测试隔离）
    g_navAutoScrollPage = ::core::PageId::Home;
    g_navAutoScrollSig.clear();
    g_navAutoScrollSuspend = false;
    g_chevronUpHovered = false;
    g_chevronDownHovered = false;
    g_chevronUpPressed = false;
    g_chevronDownPressed = false;
}

// 测试缝：预置导航滚动 offset（无头测试驱动「滚到底」场景）；写入值在下一次
// compose 按 [0, contentH−viewportH] clamp（§11.2 收敛语义）。预置帧跳过自动
// 滚入（bug368：见 g_navAutoScrollSuspend 说明），否则当前项会把预置值拉回。
void setNavScrollOffsetForTests(float offset) {
    g_navScrollOffset = std::max(0.0f, offset);
    g_navAutoScrollSuspend = true;
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
    //   正常档 H≥712：topPad 32 / brandNavGap 28 / gap 4 / itemH 40（导航起点见
    //     navStart）、钮 40 / 预留区 56（§10.2；bug144/146 两轮放大定稿 dd0f28db）
    //   紧凑档 680≤H<712：16 / 16 / 4 / 40（起点 88；最小窗 1200×720 → H=680 落此
    //     档）、钮 36 / 预留区 52
    //   极紧档 H<680：12 / 12 / 2 / 36（起点 80）、钮 36 / 预留区 52
    // logo 不再随高三档写死（bug165 v3 = need241 §9.3，见下方公式）；钮径/预留区
    // 仍随高三档（两态一致不回退）。压缩顺序：先压品牌区上下留白 → 再压行距 →
    // 最后压行高。分档几何用常量不走 token：矮窗帧 theme::setWindowHeight 会重建
    // 字号 token，布局分档必须与字号档解耦。
    const float availH = screen.height;
    float topPad = 32.0f;
    float brandNavGap = 28.0f;
    float gap = 4.0f;
    float itemH = ui_kit::kNavItemH;  // 40（正常/紧凑档）
    float toggleSize = 40.0f;     // 2026-11-06 老板三轮实测：32→36→40（mac 观感仍偏小）
    float toggleZoneH = 56.0f;    // 预留区随钮同步 +4（钮径 + 上下各 8）
    if (availH < 680.0f) {
        topPad = 12.0f;
        brandNavGap = 12.0f;
        gap = 2.0f;
        itemH = 36.0f;
        toggleSize = 36.0f;
        toggleZoneH = 52.0f;
    } else if (availH < 712.0f) {
        topPad = 16.0f;
        brandNavGap = 16.0f;
        toggleSize = 36.0f;
        toggleZoneH = 52.0f;
    }
    // bug165（need241 设计 §9.3）：logo 随窗口自适应 = clamp(min(宽度档, 高度档),
    // 40, 64)，手动折叠态（72px 带）追加上限 48。宽度档按窗口逻辑宽 W（本函数
    // 的 screen 即 bodyScreen 全宽）：<1440→48 / 1440~1727→56 / ≥1728→64；高度
    // 档沿用既有阈值作短窗钳制：<680→40 / 680~711→44 / ≥712 不设限（封顶交钳位
    // 64）。解释偏离说明：设计表高度档顶值写 48，但 48 ≤ 宽度档下限 48 会让
    // min 恒等高度档、宽度档成死代码，与同节「大屏 logo 诉求是 bug165 本意」
    // 「钳 [40,64]」及老板「窗口拉大 logo 应自动变大」直接矛盾——按意图读法
    // 正常档不设限；W<1440 与短窗结果与设计表完全一致，仅 W≥1440 且正常档时
    // 为 56/64（即放大点）。
    const float widthTier = screen.width >= 1728.0f   ? 64.0f
                            : screen.width >= 1440.0f ? 56.0f
                                                      : 48.0f;
    // 高度档只作**极端矮窗**的保护（原 680/712 三档是导航行高的档位，logo 不该
    // 跟着被压到 44——2026-11-06 老板实测「侧栏展开态 mac logo 仍很小」：
    // 窗口 1470×737 → availH=697 落紧凑档 → min(56,44)=44，观感明显偏小）。
    // 改为只保留极紧档兜底（availH<680 → 48），其余沿用宽度档：
    //   W≥1728 → 64 / W≥1440 → 56 / 其他 → 48；矮窗最低 48 而非 44。
    const float heightTier = availH < 680.0f ? 48.0f : 64.0f;
    float logoSize = std::clamp(std::min(widthTier, heightTier), 40.0f, 64.0f);
    if (collapsed) logoSize = std::min(logoSize, 48.0f);  // §9.3 折叠态上限
    const float padX = t.sizes.spacingLarge;
    constexpr float kIconW = 28.0f;
    constexpr float kItemInset = 12.0f;  // 内容距高亮条左缘内边距

    // 品牌块高随 logo 同步（need241 §9.3 修正条款）：logo <56 时恒 56（既有锚点
    // 不塌，48 瓦上下各留 4px）；logo ≥56（大屏档）块加高至 logo+16（上下各 8px
    // 呼吸位），本档 topPad/brandNavGap 相应 −4 补偿
    float brandH = 56.0f;
    if (logoSize >= 56.0f) {
        brandH = logoSize + 16.0f;
        topPad -= 4.0f;
        brandNavGap -= 4.0f;
    }

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
            // (0, topPad, kWidth, brandH)；品牌文本由自绘标题栏承载。
            // bug165 v3（need241 §9.3）：logo 随窗口宽高自适应（公式见上方分档处），
            // 品牌块行高随 logo 同步（≥56 加高至 logo+16）；折叠态恒 ≤48，块高回 56。
            ui.stack("app.sidebar.brand")
                .position(0.0f, topPad)
                .size(kWidth, brandH)
                .content([&] {
                    ui_kit::drawDevDeskLogo(
                        ui, "app.sidebar.brand.mark",
                        (kWidth - logoSize) * 0.5f, (brandH - logoSize) * 0.5f,
                        logoSize, t.colors.primary, t.colors.surface);
                })
                .build();

            // 导航滚动区起点（§11.2）：brand 行下缘 = viewport 顶（品牌区锚定不滚）
            const float navStart = topPad + brandH + brandNavGap;

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
            // bug368 方案 C（设计 §3.1/§3.3）：navPlan 只规划业务行；系统组（Settings/
            // About）移出滚动流，作为独立底部锚定块（footer），永远可见不随滚。
            std::vector<NavRowPlan> navPlan;
            std::vector<::core::Module*> systemModules;  // 系统组底块行（注册序）
            float contentH = 0.0f;
            {
                std::set<::core::PageId> plannedGroups;
                const auto* currentEntry = registryEntry(router->currentPage());
                for (auto* m : router->modules()) {
                    if (!m) continue;
                    const ::core::PageId page = m->pageId();
                    if (isSystemGroup(page)) {
                        systemModules.push_back(m);
                        continue;  // 系统组不进滚动流（锚底块独立绘制）
                    }
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

            // bug368 方案 C（设计 §3.3 / §4.2）：四段式锚定——品牌（固定顶）→ 工作
            // 导航（唯一内滚区）→ 系统组底块（固定底）→ 折叠钮预留区（固定底）。
            // 工作视口 = availH − navStart − footerZoneH − toggleZoneH（801 档 490）；
            // footerZoneH = 8 呼吸 + 1 线 + 8 + 设置 40 + 4 + 关于 40 + 8 = 109，
            // 极紧档 itemH 36 → 99（设计 §4.2「极紧档减 10」）。无系统组项（模块
            // 全关的极端装配 / 桩 Router）不预留底块高度，空白不占版面（设计 §7
            // 「空 = 不画」同一口径）。
            const float footerZoneH =
                systemModules.empty() ? 0.0f : (itemH >= 40.0f ? 109.0f : 99.0f);
            const float footerTop =
                std::max(navStart, availH - toggleZoneH - footerZoneH);
            const float viewportH = std::max(0.0f, footerTop - navStart);
            const float navMaxOffset = std::max(0.0f, contentH - viewportH);

            // 当前项自动滚入（设计 §3.2）：路由切换 / 窗口高变化 / 分组收展后执行一次
            //（几何签名变化沿），规则见下；无动画，clamp [0, contentH−viewportH]。
            // 签名不变 = 用户手动滚动/chevron 点击的帧，不再拉回当前项。
            const auto* currentRowEntry = registryEntry(router->currentPage());
            float currentRowTop = -1.0f;
            float currentRowBottom = -1.0f;
            {
                float y = 0.0f;
                for (const auto& plan : navPlan) {
                    if (plan.groupHeader) {
                        if (currentRowEntry != nullptr &&
                            currentRowEntry->parent.has_value() &&
                            *currentRowEntry->parent == plan.entry->page) {
                            currentRowTop = y;
                            currentRowBottom = y + itemH;
                        }
                        y += itemH + gap;
                    } else {
                        if (plan.module != nullptr &&
                            plan.module->pageId() == router->currentPage()) {
                            currentRowTop = y;
                            currentRowBottom = y + itemH;
                        }
                        y += itemH + gap;
                    }
                }
            }
            const std::string navGeomSig =
                std::string(::core::pageIdToString(router->currentPage())) + "." +
                std::to_string(static_cast<int>(contentH)) + "." +
                std::to_string(static_cast<int>(viewportH)) + "." +
                std::to_string(navPlan.size());
            const bool navGeomChanged = navGeomSig != g_navAutoScrollSig ||
                                        g_navAutoScrollPage != router->currentPage();
            g_navAutoScrollSig = navGeomSig;
            g_navAutoScrollPage = router->currentPage();
            if (currentRowTop >= 0.0f && navGeomChanged && !g_navAutoScrollSuspend) {
                constexpr float kAutoScrollBuffer = 4.0f;
                if (currentRowTop - kAutoScrollBuffer < g_navScrollOffset) {
                    g_navScrollOffset = currentRowTop - kAutoScrollBuffer;
                }
                if (currentRowBottom + kAutoScrollBuffer >
                    g_navScrollOffset + viewportH) {
                    g_navScrollOffset =
                        currentRowBottom + kAutoScrollBuffer - viewportH;
                }
            }
            g_navAutoScrollSuspend = false;
            g_navScrollOffset =
                std::clamp(g_navScrollOffset, 0.0f, navMaxOffset);
            // contentKey：行数/档位/系统组有无任一变化即重测（MeasureCache 缓存
            // 其余帧的测量，避免每帧空跑一遍绘制）
            const std::string navContentKey =
                "sidebar.nav." + std::to_string(navPlan.size()) + "." +
                std::to_string(static_cast<int>(itemH)) + "." +
                (systemModules.empty() ? "n" : "s");

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
                                if (plan.groupHeader) {
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

            // 系统组底块（bug368 方案 C，设计 §5.2）：设置/关于锚底，不参与滚动，
            // 任何窗口高下常驻。原「业务→系统」组间 divider 移作本块上沿分隔线
            // （8px 呼吸 + 1px 线 + 8px）。行几何与导航项同源（同 renderNavItem），
            // 折叠态 hit x=16/w=40、图标居中自动生效。
            if (!systemModules.empty()) {
                ui.rect("app.sidebar.groupdiv")
                    .position(padX, footerTop + 8.0f)
                    .size(collapsed ? 40.0f : kWidth - padX * 2.0f, 1.0f)
                    .color(t.colors.borderDefault)
                    .build();
                float rowY = footerTop + 17.0f;  // 分隔线下 8px 起
                for (auto* m : systemModules) {
                    renderNavItem(ui, rowY, m, 0.0f);
                    rowY += itemH + gap;
                }
            }

            // 栅边 chevron（设计 §3.2 B2）：溢出提示，不引入滚动条视觉（bug188
            // 裁决保持）。落在行右缘 184 之外的 16px 栅边（折叠态 28..44），浮在
            // 滚动区之上、不参与滚动。显示条件：上 offset>0.5 / 下 offset<max−0.5；
            // 隐藏时 hitTestMode None（不可误点）+ opacity 0。单击滚 0.8×视口高。
            {
                const bool showUp = g_navScrollOffset > 0.5f;
                const bool showDown = g_navScrollOffset < navMaxOffset - 0.5f;
                constexpr float kChevronHitW = 16.0f;
                constexpr float kChevronHitH = 24.0f;
                constexpr float kChevronGlyph = 12.0f;
                const float chevronGlyphX = collapsed ? 30.0f : 186.0f;
                const float chevronHitX = collapsed ? 28.0f : 184.0f;
                auto drawChevron = [&](const std::string& id, float hitY,
                                       unsigned glyph, bool visible, bool hovered,
                                       bool pressed,
                                       std::function<void()> click) {
                    const float scale = pressed ? 0.92f : 1.0f;
                    const core::Transition tr = core::Transition::make(
                        pressed || hovered ? 0.10f : 0.20f,
                        hovered ? core::Ease::OutCubic : core::Ease::OutBack);
                    ui.rect(id + ".hit")
                        .position(chevronHitX, hitY)
                        .size(kChevronHitW, kChevronHitH)
                        .states(kTransparent,
                                t.colors.primarySoftBg,
                                t.colors.primarySoftBg)
                        .radius(6.0f)
                        .opacity(visible ? 1.0f : 0.0f)
                        .hitTestMode(visible ? core::dsl::HitTestMode::Layout
                                             : core::dsl::HitTestMode::None)
                        .onHover([id](bool h) {
                            if (id.find("up") != std::string::npos) {
                                g_chevronUpHovered = h;
                            } else {
                                g_chevronDownHovered = h;
                            }
                        })
                        .onPress([id](const auto&, const auto&) {
                            if (id.find("up") != std::string::npos) {
                                g_chevronUpPressed = true;
                            } else {
                                g_chevronDownPressed = true;
                            }
                        })
                        .onRelease([id](const auto&, const auto&) {
                            if (id.find("up") != std::string::npos) {
                                g_chevronUpPressed = false;
                            } else {
                                g_chevronDownPressed = false;
                            }
                        })
                        .onClick(std::move(click))
                        .build();
                    ui.text(id + ".glyph")
                        .position(chevronGlyphX, hitY + 6.0f)
                        .size(kChevronGlyph, kChevronGlyph)
                        .icon(eui::utf8(glyph))
                        .fontSize(kChevronGlyph)
                        .horizontalAlign(eui::HorizontalAlign::Center)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(hovered ? t.colors.primaryHover
                                       : t.colors.textTertiary)
                        .opacity(visible ? 1.0f : 0.0f)
                        .transformOrigin(0.5f, 0.5f)
                        .scale(scale)
                        .transition(tr)
                        .animate(core::AnimProperty::TextColor |
                                 core::AnimProperty::Opacity |
                                 core::AnimProperty::Transform)
                        .build();
                };
                const float viewportBottom = navStart + viewportH;
                drawChevron(
                    "app.sidebar.chevron.up", navStart - 2.0f, 0xF077, showUp,
                    g_chevronUpHovered, g_chevronUpPressed, [viewportH] {
                        g_navScrollOffset = std::max(
                            0.0f, g_navScrollOffset - 0.8f * viewportH);
                    });
                drawChevron(
                    "app.sidebar.chevron.down", viewportBottom - 22.0f, 0xF078,
                    showDown, g_chevronDownHovered, g_chevronDownPressed,
                    [viewportH, navMaxOffset] {
                        g_navScrollOffset = std::min(
                            navMaxOffset, g_navScrollOffset + 0.8f * viewportH);
                    });
            }

            // 折叠开关（底部固定）：need172 v2（设计文档 §10.2）+ v3（§2/§3）——直径
            // 按档 40/36 正圆，预留区 56/52 内垂直居中（toggleY = H−zone+4，上
            // 下各 8）；水平：折叠 (72−size)/2（16/18）、展开 padX=16 对齐导航左缘
            //（两态水平位移 ≤6px、垂直零位移，宽度无动画档瞬时切换）。码点字号
            // fontSizeSmall(12)（§10.5-3，随钮缩小降档）。常态 1px borderDefault
            // 圆框常显 + textSecondary 图标；hover：primarySoftBg 底 + primaryHover
            // 描边 + primary 图标 + scale 1.09（150ms）+ 单层阴影浮起（§3）；按压：
            // primary 描边 + scale 0.90、阴影消失；focus：1.5px primary 描边环；
            // 移出/松手回弹 280ms 强 OutBack（超调 ~1.11 再回落，老板令「果冻要
            // 明显」）。形变与变色走「每帧换目标值 + transition
            // 平滑」（jelly 坑①②：.animate 只是允许动，目标值必须 compose 期每帧
            // 重给）。EUI 每元素单 Transition，按压段 100ms Out 并入同曲线兜
            //（jelly.h 全段单档先例）。
            // 热区与绘制解耦（§10.2）：hover/点击命中 = 绘制区外扩 4px
            //（40/36 ≥32px 桌面最小可点约定），视觉缩小不牺牲命中率——交互回调
            // 全在独立透明热区 rect（active 导航项三态全透明仍可点中先例）。
            // 2026-11-06 老板第三轮实测修正三件：
            // ① 位置太靠左下 → toggleY 由「区顶+8」改为「区顶+区高一半居中后上提 4」，
            //    toggleX 展开态 padX(16) → padX+8（右移 8，不贴侧栏左缘）。
            // ② mac 尺寸仍小 → toggleSize 三档再放大（见上方档位定义）。
            // ③ 非 hover 态要有阴影 → 常态给一层很淡的渲染阴影（非全无），
            //    hover 加深上浮，pressed 收起。
            const float toggleY = availH - toggleZoneH + 4.0f;
            const float toggleX =
                collapsed ? (kWidth - toggleSize) * 0.5f : padX + 8.0f;
            const bool toggleHovered = g_toggleHovered;
            const bool togglePressed = g_togglePressed;
            const bool toggleFocused = g_toggleFocused;  // v3：focus 态
            const bool toggleActive = toggleHovered || togglePressed;
            // 果冻加明显（2026-11-05 老板令，need172 v3.1）：hover 1.09、按压 0.90，
            // 回弹 280ms 强 OutBack——松手回弹时从 0.90 冲过目标超调至 ~1.11 再回
            // 落（OutBack 标准超调 ~10% 行程），肉眼可见的弹性。仅此钮（jellyCard
            // 同一 idiom：坑①② 每帧换目标值 + transition/animate 平滑），不扩散
            // 导航项。
            const float toggleScale =
                togglePressed ? 0.90f : toggleHovered ? 1.09f : 1.0f;
            const core::Transition toggleTransition = core::Transition::make(
                toggleHovered ? 0.15f : 0.28f,
                toggleHovered ? core::Ease::OutCubic : core::Ease::OutBack);
            // 悬浮阴影（v3 §3/§9-4；2026-11-06 老板第三轮：常态也要有阴影）：
            //   常态 = 极淡贴地阴影（blur4/offsetY1/8%）—— 有「浮起」暗示但不抢眼
            //   hover = 加深上浮（blur8/offsetY2/10%）
            //   pressed = 收起（按下贴回）
            const core::Shadow toggleShadow =
                togglePressed
                    ? core::Shadow{}
                    : toggleHovered
                          ? core::Shadow{true, {0.0f, 2.0f}, 8.0f, 0.0f,
                                         eui::Color{0.102f, 0.102f, 0.102f, 0.10f}}
                          : core::Shadow{true, {0.0f, 1.0f}, 4.0f, 0.0f,
                                         eui::Color{0.102f, 0.102f, 0.102f, 0.08f}};
            ui.rect("app.sidebar.toggle")
                .position(toggleX, toggleY)
                .size(toggleSize, toggleSize)
                .color(toggleActive ? t.colors.primarySoftBg : kTransparent)
                .border(toggleFocused ? 1.5f : 1.0f,
                        // v3 focus 态：1.5px primary 描边环（focus+hover 叠加同值，
                        // pressed 优先级最高）；不染色底不变图标色
                        togglePressed || toggleFocused
                            ? t.colors.primary
                            : toggleHovered ? t.colors.primaryHover
                                            : t.colors.borderDefault)
                .radius(toggleSize * 0.5f)  // 半径=半宽 → 正圆
                .shadow(toggleShadow)
                .transformOrigin(0.5f, 0.5f)  // 缩放中心在按钮中心（jelly 坑③）
                .scale(toggleScale)
                .transition(toggleTransition)
                .animate(core::AnimProperty::Color | core::AnimProperty::Border |
                         core::AnimProperty::Transform |
                         core::AnimProperty::Shadow | core::AnimProperty::Blur)
                .build();
            ui.text("app.sidebar.toggle.icon")
                .position(toggleX, toggleY)
                .size(toggleSize, toggleSize)
                .icon(eui::utf8(collapsed ? 0xF0C9 : 0xF100))  // fa-bars / fa-angles-left
                // 图标 = 钮径 50%（钮放大到 40/36 后 56% 会显得拥挤；50% 视觉更平衡）
                .fontSize(core::theme::scaled(toggleSize * 0.50f))
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
            // 三态全透明（不参与视觉）。onFocusChanged 自带 focusable=true（dsl.h
            // 通用 builder）：v3 focus 态入口（键盘/程序化聚焦 → 1.5px primary 环）
            ui.rect("app.sidebar.toggle.hot")
                .position(toggleX - 4.0f, toggleY - 4.0f)
                .size(toggleSize + 8.0f, toggleSize + 8.0f)
                .states(kTransparent, kTransparent, kTransparent)
                .onFocusChanged(
                    [](bool focused) { g_toggleFocused = focused; })
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

            // 弥散绿粒子 v3（need172 §5 + 2026-11-05 老板反馈「太短暂」升版）：hover
            // 进入沿一次性迸发 12 颗（30° 均布 + hash ±10° 抖动），生命 1600ms +
            // 每颗 hash 延迟 0~80ms（迸发层次，≤1760ms 全灭尽自止）。透明度衰减
            // 放平缓（老板令②）：0.75×(1−InQuad(tt))——前段保浓度（半程仍 ~56%）、
            // 后段渐隐，替代旧 0.75×(1−OutCubic) 的早期即淡；位移仍 OutCubic（同
            // 行程铺到 1600ms，速度减半 = 「不嗖一下就没了」）。v3 几何（§5.1）：
            // dy×0.55 垂直压缩 + 中心钳包络 [3, W−3]×[foldLimit, H−3]（修复 v2 上
            // 向越让位线 ~16px / 展开态左向越缘 ~12px）；折叠态弥散降档 10~16px。
            // 起点 = 钮半径+2 向外弥散 14~26px（冲出圆框才可见），粒径 4~6px 正圆、
            // 末端缩放 0.7。颜色：浅色档 kParticleLight 三色轮替（第三色 #06AD56
            // 深档拉开明度）、深色档 primary/primaryHover 双色。性能口径不变（老板
            // 令③）：hover 驱动一次性迸发（每帧 ≤12 个矩形元素、1760ms 自止），
            // 非持续动画、无常驻帧泵；非活跃帧零粒子零泵。
            if (g_toggleParticleStart.has_value()) {
                const float elapsed =
                    std::chrono::duration<float>(
                        std::chrono::steady_clock::now() -
                        *g_toggleParticleStart)
                        .count();
                if (elapsed < 1.76f) {
                    const float cx = toggleX + toggleSize * 0.5f;
                    const float cy = toggleY + toggleSize * 0.5f;
                    // 让位线（§1.1）：foldLimit = H − 区高 − 4（粒子禁越线顶边）
                    const float foldLimit = availH - toggleZoneH - 4.0f;
                    for (int i = 0; i < 12; ++i) {
                        const float delay =
                            internal::fluidHash01(i * 23 + 7) * 0.08f;  // 0~80ms
                        if (elapsed <= delay) continue;  // 未起爆不建元素
                        const float tt =
                            std::clamp((elapsed - delay) / 1.6f, 0.0f, 1.0f);
                        const float eased = core::applyEase(core::Ease::OutCubic, tt);
                        const float fade = core::applyEase(core::Ease::InQuad, tt);
                        const float ang =
                            (i * 30.0f +
                             (internal::fluidHash01(i * 13 + 1) - 0.5f) * 20.0f) *
                            0.0174533f;  // deg→rad（30° 均布 + ±10° 抖动）
                        const float spread =
                            collapsed
                                ? 10.0f + 6.0f * internal::fluidHash01(i * 17 + 2)  // 10~16 折叠降档（§5.2）
                                : 14.0f + 12.0f * internal::fluidHash01(i * 17 + 2);  // 14~26 展开
                        const float pr =
                            2.0f + internal::fluidHash01(i * 19 + 3);  // 半宽 2~3（直径 4~6）
                        const float size =
                            pr * 2.0f * (1.0f - 0.3f * eased);  // 末端缩放 0.7
                        const float d = toggleSize * 0.5f + 2.0f +
                                        spread * eased;  // 起点半径=钮半径+2
                        // 垂直压缩 Ky=0.55：上向最大触达压入让位线内，横向全幅保
                        // 「弥漫」；中心钳包络兜底拦个别 1~4px 漂移（§5.1）
                        const float px = std::clamp(cx + std::cos(ang) * d,
                                                    3.0f, kWidth - 3.0f);
                        const float py = std::clamp(cy + std::sin(ang) * d * 0.55f,
                                                    foldLimit, availH - 3.0f);
                        const eui::Color& pc =
                            t.isDark ? ((i % 2 == 0) ? t.colors.primary
                                                     : t.colors.primaryHover)
                                     : kParticleLight[i % 3];
                        ui.rect("app.sidebar.toggle.particle." +
                                std::to_string(i))
                            .position(px - size * 0.5f, py - size * 0.5f)
                            .size(size, size)
                            .radius(size * 0.5f)  // 半径=半宽 → 正圆（fluid 卫星同款）
                            .color(pc)
                            .opacity(0.75f * (1.0f - fade))
                            .build();
                    }
                    // 帧泵（fluid driver 同款先例）：粒子位置每帧按壁钟重算，泵只
                    // 在 1760ms 生命期内保持出帧；到期帧起本分支整体不建（自止）
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

void compose(eui::Ui& ui, const eui::Screen& screen) {
    // --minimized 启动（UAT6 开机自启动）：首帧一次性走 close-to-tray 同款
    // WM_CLOSE→隐藏路径（点标题栏 X 藏托盘是同一入口 app::closeMainWindow；主窗
    // 句柄此刻已由 EUI-NEO 主循环回写，SendMessage 同步派发 → close callback 置
    // hideToTrayRequested，下一帧循环顶隐藏到托盘）。须在 splash 之前：旗标在身
    // 时 splash 不该露脸。取走即清：仅启动后第一帧生效，托盘左键/Show 正常唤起。
    if (app::assembly::consumePendingMinimizedStartup()) {
        app::closeMainWindow();
        // bug307：托盘态启动主窗首帧后不再 compose（EUI 主循环走托盘分支），
        // splash 照常启动会挂在「启动 splash 开始」永不结束（收尾只能由主窗帧
        // 触发，实测日志停 2 分钟零推进）。与 closeMainWindow 同点一次跳过：
        // splash 不启动，托盘点「显示主窗口」首帧直接画主界面。
        ui_kit::skipStartupSplash();
    }
    // bug283：主窗首帧记账——此后 odoc 送达一律按温开（不再 arm 托盘启动）。
    // 首帧无论是否走 close-to-tray 都算「生命周期已开」：托盘态启动时 compose
    // 照跑（EUI 渲染与 show 解耦），闩不落会让温开误抢隐藏。
    app::assembly::markMainWindowComposed();
    // F-01 第二实例送达路径 drain：单实例监听线程入队（platform 回调），这里主线程
    // 按送达序逐路径走 OS 打开漏斗（main 注入的 handleAppleEventOpenFile 同一漏斗）。
    // 取走即清（consumePendingMinimizedStartup 同款形态），每帧调用空队列零开销。
    app::assembly::drainPendingSecondInstanceFiles();
    // 壁纸删除结果 drain（need356 F-04）：worker 线程只入队，主线程在此逐条投
    // toast（showAppNotice 非线程安全，禁 worker 直调）。空队列零开销。
    for (auto& notice : app::wallpaper_delete::takeResults()) {
        showAppNotice(notice.first, notice.second);
    }
    // 窗体记忆记账（UAT6）：每帧记录还原态客户区逻辑尺寸 + 最大化标志（退出落盘的
    // 数据源）。须在 splash 早退之前——splash 帧也是主窗帧，启动即最大化的会话靠它
    // 记下最大化态（尺寸则由 main.cpp 的播种兜底）。最大化帧不记尺寸（保留还原态）
    ::core::window_geometry::noteFrame(screen.width, screen.height,
                                       app::mainWindowMaximized());
    // H19 窗高驱动字号封顶：每帧喂窗口逻辑高；封顶档变化时 theme 内部重建 token + 广播
    ::core::theme::setWindowHeight(screen.height);
    // 启动 splash（#196 裁决 2026-11-07）：总长 1.2s = 0.2s 淡入 + 0.9s 进度 +
    // 0.3s 交叉淡化（墨染 1.2s 全程播完）。全遮盖期（0~0.9s）返回 true → 跳过
    // 主界面布局；交叉淡化期（0.9~1.2s）返回 false → 主界面照画，splash 由
    // compose 尾部 drawSplashOverlay 盖顶递减（见本函数末尾）。
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
    // bodyTop gate 化（need:140 F-03，审计 P1-1 回退缝）：chrome=true（Windows
    // 自绘壳 / macOS NSView 自绘壳启用）→ titlebarHeight()=40，两端同值同档；
    // 回退 override=false → macOS 恢复系统标题栏（红绿灯原生钮 + inset 30 现状）。
    // 回退态 inset 三平台口径收口 window_layout 平台族（need:296 D4 上收 #ifdef，
    // 整体回退预案依托此缝）
    const float kSystemTitleBarInset =
        ::core::window_layout::systemTitleBarInset();
    // 全屏态（老板 2026-11-06）：mac 原生全屏下标题条整条退场，内容铺满全窗 →
    // bodyTop=0、不画标题条。非全屏保持 chrome ? 40 : 回退 inset。
    const bool inFullscreen = app::mainWindowFullscreen();
    const float bodyTop = inFullscreen ? 0.0f
                                       : (chrome ? titlebarHeight() : kSystemTitleBarInset);
    if (chrome && !inFullscreen) {
        internal::drawTitlebar(ui, screen);
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
            // y 同缝（bug245）：页面区顶 = 标题条带底，指针 y 是窗口绝对含标题带
            router->setPageOriginY(bodyTop);
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
    // 覆盖，放最后保证不被任何内容盖住。最大化态隐藏（need184③）。
    // 圆角按平台真实窗体圆角传入（2026-11-06 修 mac 贴边）：macOS = AppKit 原生
    // 16 逻辑pt（AppKit 内省实证，误测史与依据见 window_layout_macos.cpp，
    // bug145/池197）；Windows = DWM 固定 8 物理px 折算逻辑值。三平台口径收口
    // window_layout 平台族（need:296 D4 上收 #ifdef）。
    const float scale = app::contentScale();
    const float cornerRadiusLogical =
        ::core::window_layout::windowCornerRadiusLogical(scale);
    ui_kit::drawWindowRing(ui, screen.width, screen.height,
                           core::theme::current(), app::mainWindowMaximized(),
                           scale, cornerRadiusLogical);
    // #196 交叉淡化盖顶：主界面帧画完后，splash 以递减透明度叠顶（0.9~1.2s 窗口期；
    // 窗口期外零元素零开销）。放最顶：淡化期间 splash 覆盖含窗口边框在内的全部内容。
    // 退出丢稿确认弹窗（need:291）：guard 拦截退出后置开态，此处盖顶绘制
    //（dialog zIndex 1200——EUI 按子树 max zIndex 排兄弟，页内 toast z1100 抬高
    // 上界，取 1000 会被盖+点击截胡，实测依据见 quit_save_bridge.cpp 注释）；
    // 未开 = 零开销早退。splash 只在启动 1.2s 内存在，退出确认发生在其后，
    // 两者无共存窗口。
    app::quit_save::drawConfirmDialog(ui, screen);
    // 壁纸素材删除确认弹窗（need356 F-02）：同 #156 危险变体壳，z=1200 盖过页内 overlay。
    app::wallpaper_delete::drawDialog(ui, screen);
    ui_kit::drawSplashOverlay(ui, screen);
}

} // namespace app
