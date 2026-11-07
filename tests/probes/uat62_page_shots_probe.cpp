// uat62_page_shots_probe.cpp — UAT62 设计系统「全页面截图批」探针（真窗口 + app::compose）
//
// 交付物：kModuleRegistry 每个可导航行 × 每档窗口尺寸各一张 PNG（供「看图会话」逐页对设计稿审查）。
// 运行两次（每档一次，尺寸由 --size 传入，文件名后缀 = 窗口高）：
//   uat62_page_shots_probe.exe --size 1000x640 --out <输出目录>   → 01-home-640.png … 14-about-640.png
//   uat62_page_shots_probe.exe --size 1600x1000 --out <输出目录>  → 01-home-1000.png … 14-about-1000.png
// （分组父项行工厂为空、无页面可拍；跳过项不落图，编号沿用表行下标故可能出现空号）
// --out 缺省 = 当前工作目录下 UAT62-screenshots/；目录不存在自动建。
//
// 构图路径：真 app::compose（自绘标题栏 + 侧边栏 + Router 当前页，与真机同一条装配链）。
// 页面按 src/app_registry.h 的 kModuleRegistry 表序遍历（14 行 = PageId 枚举序 =
// 侧边栏序），实例全部来自表内 extern "C" 工厂（真模块，非桩）。
//
// 数据态 = 探针默认空状态（设计审查对象之一，不造假数据）：MdHeadlessEnv 给
// 空笔记目录 + 已迁移空库（注册为 appDatabase）+ 关笔记目录 git 自动提交，
// 各模块 onStart 全部走「空库/空目录」默认分支。
//
// 落图不用 captureIfRequested 而直接 readFrame + writeRgbaToPng：同一设施低一层，
// 但每页落图前可在 RGBA 缓冲上做**全黑帧 / 纯色帧判据**（readFrame 已翻轴为
// top-down；翻轴本身的正确性由 screenshot_probe 的往返断言钉死，本探针不重复验）。
//
// 链接面（CMake 里与 module_toggle_sidebar_probe / h13_fluid_background_probe 同组）：
// 额外编译 src/app_sidebar.cpp + src/app_assembly.cpp + src/module_toggle_bridge.cpp。
// app::dslAppConfig() 桩 + app::requestShow/requestUpdate 真调用一次（内联定义落地）。
#include "probes/probe_harness.h"

#include <eui/detail/dsl_app_impl.h>  // app::requestShow/requestUpdate 内联实现
#include "app_assembly.h"             // app_assembly.cpp 符号（app_sidebar 引用）
#include "app_registry.h"             // kModuleRegistry（14 行登记表 + 工厂）
#include "app_sidebar.h"              // 真 app::compose
#include "module_toggle_bridge.h"     // app_assembly 的桥符号
#include "test_support.h"             // MdHeadlessEnv / ScopedAppRouter

#include "core/module_interface.h"
#include "core/router.h"
#include "core/theme.h"
#include "module_settings/settings_config.h"  // applyWindowVisuals（窗口效果档）
#include "module_settings/settings_ui_logic.h"  // kSettingTabCount（B6 分 tab 落图）

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

// app:: 层链接桩（dsl_app_impl.h 引用 dslAppConfig；app::compose 用真实现，
// 来自本 probe 编译的 src/app_sidebar.cpp，不桩）
namespace app {
// F-03 亮暗对照：UAT62_DARK=1 → 暗主题（进程级一次性，先于首帧 compose）
namespace {
const bool g_uat62Dark = std::getenv("UAT62_DARK") != nullptr;
const struct Uat62ThemeInit {
    Uat62ThemeInit() {
        if (g_uat62Dark) core::theme::setDark();
        else core::theme::setLight();
        // 终端全屏态截图注入口（F-05；页面 env 同名约定）
    }
} g_uat62ThemeInit;
} // namespace

const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config =
        DslAppConfig{}.title("devdesk_uat62_page_shots_probe");
    return config;
}
}  // namespace app

namespace {

// 输出文件名 slug（下标与 kModuleRegistry 表序一一对应；static_assert 钉长度）
// 2026-10 重排：调度在 Todo 之后、上线三行在 settings 之前
constexpr const char* kSlugs[] = {
    "home",     "markdown", "todo",          "schedule", "claude_proxy",
    "agent",    "memory",   "wallpaper",     "mcp",      "terminal",
    "release_group", "release_tasks", "release_config", "settings", "about",
};
static_assert(sizeof(kSlugs) / sizeof(kSlugs[0]) ==
                  sizeof(::app::registry::kModuleRegistry) /
                      sizeof(::app::registry::kModuleRegistry[0]),
              "kSlugs 与 kModuleRegistry 行数不一致（表序漂移）");

// 两数字符串（01..14）
std::string twoDigit(std::size_t n) {
    std::string s = std::to_string(n);
    return s.size() < 2 ? "0" + s : s;
}

// 全黑/纯色帧判据（采样 8px 网格；判据口径同 screenshot_probe ⑤）：
//  - 纯黑采样点占比 > 90% = 全黑帧（窗口没渲染 / 渲染后端没激活）
//  - 颜色桶（R/G/B 各取高 3 位）数 < 3 = 纯色帧（空窗 / 只有底色）
//  - bug185 补：明确非白采样占比 < 5% = 白底空帧——白底（浅色档 255）+ 细线
//    覆盖层（如窗口环 2px 线宽）的 AA 像素能凑满 3 桶骗过前两条（NOTES 实证
//    01-home-640 目测全白判绿）；真页面正文/图标/卡片远超 5%。
// 返回 true = 通过；失败时 stderr 打出两个实测值。
bool frameHasContent(const std::vector<unsigned char>& rgba, int width, int height) {
    std::size_t black = 0;
    std::size_t nonWhite = 0;
    std::size_t samples = 0;
    std::unordered_set<std::uint32_t> buckets;
    for (int y = 0; y < height; y += 8) {
        for (int x = 0; x < width; x += 8) {
            const unsigned char* px =
                rgba.data() +
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                 static_cast<std::size_t>(x)) *
                    4u;
            ++samples;
            if (px[0] < 10 && px[1] < 10 && px[2] < 10) {
                ++black;
            }
            const int dr = 255 - static_cast<int>(px[0]);
            const int dg = 255 - static_cast<int>(px[1]);
            const int db = 255 - static_cast<int>(px[2]);
            if (dr > 24 || dg > 24 || db > 24) {
                ++nonWhite;
            }
            buckets.insert((static_cast<std::uint32_t>(px[0] >> 5)) |
                           (static_cast<std::uint32_t>(px[1] >> 5) << 3) |
                           (static_cast<std::uint32_t>(px[2] >> 5) << 6));
        }
    }
    const bool blackFrame = samples > 0 && black * 100 / samples > 90;
    const bool solidFrame = buckets.size() < 3;
    const bool whiteSkinFrame =
        samples > 0 && nonWhite * 100 / samples < 5;  // bug185：白底+细线假绿
    if (blackFrame || solidFrame || whiteSkinFrame) {
        std::cerr << "  FAIL——帧内容判据不过：黑点占比 " << black << "/"
                  << samples << "，颜色桶 " << buckets.size()
                  << "，非白占比 " << nonWhite * 100 / samples << "%"
                  << (blackFrame  ? "（全黑帧）"
                      : solidFrame ? "（纯色帧）"
                                   : "（白底空帧）") << "\n";
        return false;
    }
    return true;
}

// bug185（2026-11-04）：捕获前暖场渲染遍数。retained layer 重建有稳定帧门槛
// （runtime_render.h：普通层连续 2 帧、几何 resize 层连续 16 帧未达标整层不画）；
// 真 app 每帧 render 无感，本探针每页只捕获一次——不跑满门槛就会拍到冷 pending
// 的空白内容层（整页近纯白只剩直画的窗口环，135% 小屏收缩/钳制改变 layer 几何
// 后大面积触发）。17 = 16 + 1 帧裕量。
constexpr int kCaptureRenderPasses = 17;

// TODO(bug185 临时诊断，验完即删)：树遍历统计。
void walkTreeIds(const core::dsl::Element* e, std::size_t& total,
                 std::string& ids) {
    if (e == nullptr) return;
    ++total;
    if (ids.size() < 500) ids += (ids.empty() ? "" : ",") + e->id;
    for (const auto& c : e->children) walkTreeIds(c.get(), total, ids);
}

}  // namespace

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    std::string outDir = "UAT62-screenshots";  // 缺省 = 当前工作目录下
    // 2026-11-01：字号缩放档位（--font-scale 0.85~1.30，缺省 1.0）。
    // 用途：对照「哪些文本不响应设置里的字号修改」——同页跑两档，
    // 像素 diff 就能定位漏缩放的文本，不用靠目测。
    float fontScale = 1.0f;
    // 2026-11-01：侧栏折叠档（--sidebar-collapsed）。折叠态侧栏只有 72px，
    // 顶部品牌 logo 的占比与展开态完全不同 —— 展开态看着协调，折叠态可能失衡。
    bool sidebarCollapsed = false;
    // 2026-11-01：窗口效果档（--window-effect off|translucent|frosted）。
    // 此前所有离屏截图都是「关闭」档 —— 半透/磨砂两档的页面底 alpha、侧栏铺色
    // 与卡片半透叠加都没被看过，等于两个档位从未验收过。
    std::string windowEffect = "off";
    // B6/F-69-4：设置页分 tab 落图（缺省开）。设置页 5 tab 卡集不同，用户报
    // 「模块 tab 全乱」那一屏不在常规十页清单里；--no-settings-tabs 可关掉。
    bool captureSettingsTabs = true;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--out" && i + 1 < argc) {
            outDir = argv[++i];
        } else if (std::string(argv[i]) == "--font-scale" && i + 1 < argc) {
            fontScale = std::stof(argv[++i]);
        } else if (std::string(argv[i]) == "--no-settings-tabs") {
            captureSettingsTabs = false;
        } else if (std::string(argv[i]) == "--sidebar-collapsed") {
            sidebarCollapsed = true;
        } else if (std::string(argv[i]) == "--window-effect" && i + 1 < argc) {
            windowEffect = argv[++i];
        }
    }
    // 注：切档时机在每页渲染中间（模拟用户在设置里改字号），且必须在
    // router.startAll() 之后——settings 模块 onStart 会按配置文件 fontScalePercent
    // 调 setFontScale，把探针指定的档位覆盖回默认。
    auto applyFontScale = [fontScale]() {
        if (fontScale != 1.0f) core::theme::setFontScale(fontScale);
    };

    if (!probe::initializeWindowBackend()) return 1;
    auto window =
        probe::makeWindow("uat62_page_shots", args.windowW, args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }

    // 数据隔离（探针默认空状态）：空笔记目录 + 已迁移空库注册 appDatabase +
    // 关笔记目录 git 自动提交。全模块 onStart 吃这份空底座，不触真实用户数据。
    test_support::MdHeadlessEnv mdEnv("uat62shots_", "shots", "shots.db");

    // 14 行真模块：实例来自 kModuleRegistry 表内工厂，注册序 = 表序 = 侧边栏序
    //（分组父项/不支持壁纸的平台工厂返回 nullptr，registerModule 对 nullptr 幂等跳过）
    ::core::Router router;
    std::vector<std::unique_ptr<::core::Module>> mods;
    for (const auto& e : ::app::registry::kModuleRegistry) {
        std::unique_ptr<::core::Module> m(e.factory());
        router.registerModule(m.get());
        mods.push_back(std::move(m));
    }
    test_support::ScopedAppRouter scopedRouter(&router);
    router.startAll();
    // 折叠档：必须在 startAll 之后设（settings onStart 会按配置恢复折叠态）
    if (sidebarCollapsed) {
        app::setSidebarCollapsed(true, false);
    }
    // UAT62_DARK 补偿（2026-11-07）：settings onStart 会按临时配置把主题覆回
    // 默认档，静态 init 的 setDark 被冲掉（批7 raw-dark 实拍为亮色即此因）——
    // 与 fontScale/windowEffect 同款「探针指定值必须晚于 settings onStart」时序。
    if (app::g_uat62Dark) core::theme::setDark();
    // 窗口效果档同理：settings onStart 会按配置文件恢复档位，探针指定值必须
    // 晚于它，否则被覆盖回默认（关闭档）—— 半透/磨砂档就永远截不到。
    module::settings::applyWindowVisuals(windowEffect);

    core::dsl::Runtime runtime;
    runtime.initialize(window);
    // 内联定义落地本 TU（见文件头说明）：GLFW 初始化后调用安全
    app::requestUpdate();
    app::requestShow();

    probe::FrameCapture capture(window);
    if (!capture.ok()) {
        std::cerr << "uat62_page_shots: FrameCapture 建失败: "
                  << capture.error() << "\n";
        router.stopAll();
        runtime.shutdown(false);
        core::releaseInputQueue(window);
        core::window::destroyWindow(window);
        probe::terminateWindowBackend();
        return 3;
    }

    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    if (ec) {
        std::cerr << "uat62_page_shots: 建输出目录失败 " << outDir << ": "
                  << ec.message() << "\n";
        router.stopAll();
        runtime.shutdown(false);
        core::releaseInputQueue(window);
        core::window::destroyWindow(window);
        probe::terminateWindowBackend();
        return 4;
    }

    // compose 尺寸跟随实际窗口（2026-11-04 bug185）：--size 只是创建请求，小屏放
    // 不下收缩后请求值 ≠ 实际值；按请求值 compose 会与实际渲染表面错位（真机不可
    // 达态，render-cache/blit 随机产出整页白帧）。真 app 的 compose 尺寸恒随实际
    // 窗口（dsl_app_impl.h app::update），探针必须同口径。PNG 文件名后缀仍用请求
    // 高度（--size 契约标签）；实际落图像素尺寸以每行输出的 (pw x ph) 为准。
    const auto [w, h] = probe::windowLogicalSize(window);
    bool passed = true;
    std::size_t captured = 0;              // 实际落图页数（跳过项不计入 PASS 文案）
    std::vector<std::string> skipped;      // 平台能力不支持而无法建实例的表行 slug

    for (std::size_t i = 0; i < std::size(::app::registry::kModuleRegistry);
         ++i) {
        const auto& e = ::app::registry::kModuleRegistry[i];
        // 空工厂行（平台能力不支持，如非 Windows 的 wallpaper）没有实例可渲染：
        // 跳过 navigate/落图，否则会回退 Home 拍出重复页并谎报该页已覆盖。
        if (mods[i] == nullptr) {
            skipped.push_back(kSlugs[i]);
            std::cerr << "uat62_page_shots: 跳过 " << kSlugs[i]
                      << "（平台能力不支持，工厂返回 nullptr）\n";
            continue;
        }
        // 终端页（终端线 58462d40 引入）：交互型会话页——静态页照探针起不了终端
        // 会话，只能拍到「未起会话」稀疏壳态（实测非白占比 1%），白底判据对它
        // 恒误红。视觉验收归终端线自己的 QA 手测清单（其 Phase2 交付物）；与
        // release_group 同款响亮跳过口径，不静默、不降全局判据。
        if (kSlugs[i] == std::string("terminal")) {
            skipped.push_back(kSlugs[i]);
            std::cerr << "uat62_page_shots: 跳过 " << kSlugs[i]
                      << "（交互型终端会话页不适用静态页照，视觉验收归终端线 QA "
                         "手测清单）\n";
            continue;
        }
        router.navigateTo(e.page);
        // 稳帧（bug185 修订 2026-11-04）：不再固定 24 帧。原「400ms @60fps 盖过
        // 仓库最长过渡时长」被更长的容器级淡入过渡突破后，捕获会冻在透明度近 0
        // 的中间态（render 只重画不推进动画钟，多遍 render 也救不回），拍出
        // 「白底 + ring + 极淡内容」伪帧。改为最少 24 帧 + 追到 animating 归零
        // （封顶 240 帧 = 4s 虚拟时），与真 app「过渡播完才到稳态」同语义。
        eui::Ui* lastUi = nullptr;
        int settleFrames = 0;
        for (int f = 0; f < 240; ++f) {
            probe::composeModuleFrame(
                runtime, window, "shots", w, h,
                [&](eui::Ui& ui, const eui::Screen& screen) {
                    app::compose(ui, screen);
                    lastUi = &ui;
                });
            settleFrames = f + 1;
            if (f + 1 >= 24 && !runtime.isAnimating()) break;
        }
        // TODO(bug185 临时诊断，验完即删)：捕获时刻的树快照。
        if (std::getenv("UAT62_DBG_STATS") != nullptr) {
            const core::dsl::Element* root =
                lastUi != nullptr ? lastUi->find("app.root") : nullptr;
            const core::dsl::Element* page =
                lastUi != nullptr ? lastUi->find("app.page") : nullptr;
            auto frameStr = [](const core::dsl::Element* e) -> std::string {
                if (e == nullptr) return "-";
                return std::to_string(e->frame.x) + "," +
                       std::to_string(e->frame.y) + " " +
                       std::to_string(e->frame.width) + "x" +
                       std::to_string(e->frame.height);
            };
            const auto snapshots = 0;
            std::size_t totalElems = 0;
            std::string ids;
            if (root != nullptr) walkTreeIds(root, totalElems, ids);
            (void)snapshots;
            std::cerr << "DBGTREE page=" << kSlugs[i]
                      << " settle=" << settleFrames
                      << " animating=" << runtime.isAnimating()
                      << " elems=" << totalElems
                      << " rootKids="
                      << (root != nullptr
                              ? std::to_string(root->children.size())
                              : "-")
                      << " pageKids="
                      << (page != nullptr
                              ? std::to_string(page->children.size())
                              : "-")
                      << " root=" << frameStr(root)
                      << " page=" << frameStr(page)
                      << " ids=[" << ids << "]\n";
        }
        // 2026-11-01：切档复现「应用已开着，用户在设置里改字号」的真实路径。
        // 只在「首次就按目标档渲染」时对照，会漏掉 runtime 复用实例导致的不响应——
        // 真机是同一进程内改档，runtime 按 id 复用旧实例。
        if (fontScale != 1.0f) {
            applyFontScale();
            for (int f = 0; f < 24; ++f) {
                probe::composeModuleFrame(
                    runtime, window, "shots", w, h,
                    [&](eui::Ui& ui, const eui::Screen& screen) {
                        app::compose(ui, screen);
                    });
            }
        }
        const std::string path = outDir + "/" + twoDigit(i + 1) + "-" +
                                 kSlugs[i] + "-" +
                                 std::to_string(args.windowH) +
                                 (fontScale == 1.0f ? ""
                                                    : "-fs" +
                                                          std::to_string(
                                                              static_cast<int>(
                                                                  fontScale *
                                                                  100.0f))) +
                                 (sidebarCollapsed ? "-sc" : "") +
                                 (windowEffect == "off"
                                      ? ""
                                      : "-" + windowEffect) +
                                 ".png";
        std::vector<unsigned char> rgba;
        int pw = 0;
        int ph = 0;
        if (!capture.readFrame(runtime, rgba, pw, ph, kCaptureRenderPasses)) {
            passed = false;
            continue;
        }
        // TODO(bug185 临时诊断，验完即删)：render 统计——misses>0 = retained layer
        // 稳定门槛从未跑满；misses=0 且 fullPaint=1 = 内容不在树里（compose 侧）。
        if (std::getenv("UAT62_DBG_STATS") != nullptr) {
            const core::render::RenderFrameStats& st =
                core::render::lastRenderFrameStats();
            std::cerr << "DBGSTATS page=" << kSlugs[i]
                      << " fullPaint=" << st.fullPaint
                      << " cache=" << st.usedRenderCache
                      << " recreated=" << st.renderCacheRecreated
                      << " misses=" << st.retainedLayerMisses
                      << " rebuilds=" << st.retainedLayerRebuilds
                      << " direct=" << st.renderDirectPasses
                      << " clears=" << st.clearCalls << "\n";
        }
        // TODO(bug185 临时诊断，验完即删)：分区亮度（rgba 已翻轴 top-down）。
        if (std::getenv("UAT62_DBG_STATS") != nullptr && pw > 0) {
            auto bandMean = [&](int x0, int x1, int y0, int y1) {
                double sum = 0.0;
                int n = 0;
                for (int yy = y0; yy < y1; yy += 5)
                    for (int xx = x0; xx < x1; xx += 5) {
                        const unsigned char* p =
                            rgba.data() +
                            (static_cast<std::size_t>(yy) * pw + xx) * 4u;
                        sum += (p[0] + p[1] + p[2]) / 3.0;
                        ++n;
                    }
                return n > 0 ? sum / n : -1.0;
            };
            const int sx = static_cast<int>(220.0f / w * pw);
            std::cerr << "DBGBAND page=" << kSlugs[i]
                      << " sidebar=" << bandMean(4, sx, 8, ph - 8)
                      << " page=" << bandMean(sx + 4, pw - 4, 8, ph - 8)
                      << " full=" << bandMean(0, pw, 0, ph) << "\n";
        }
        if (!frameHasContent(rgba, pw, ph)) {
            passed = false;
        }
        if (!probe::writeRgbaToPng(path, rgba.data(), pw, ph)) {
            passed = false;
            continue;
        }
        ++captured;
        std::cout << "uat62_page_shots: " << path << " (" << pw << "x" << ph
                  << ")\n";
    }

    // settings 分 tab 落图（B6/F-69-4）：设置页 5 个 tab 的卡集完全不同（通用 /
    // 模块插拔 / 日志 / 同步 / 项目卡），而主循环每人只拍默认 tab —— 用户报
    // 「模块 tab 全乱」的那一屏根本不在 uat62 十页清单里。这里用真命中分发点
    // tab hit 区后再拍，slug 沿用 pages 语义：settings-tab<N>。
    if (captureSettingsTabs) {
        std::size_t settingsIdx = std::size(::app::registry::kModuleRegistry);
        for (std::size_t i = 0; i < std::size(::app::registry::kModuleRegistry);
             ++i) {
            if (std::string(kSlugs[i]) == "settings") settingsIdx = i;
        }
        if (settingsIdx < std::size(::app::registry::kModuleRegistry) &&
            mods[settingsIdx] != nullptr) {
            router.navigateTo(::app::registry::kModuleRegistry[settingsIdx].page);
            eui::Ui* ui = nullptr;
            for (int f = 0; f < 240; ++f) {
                probe::composeModuleFrame(
                    runtime, window, "shots", w, h,
                    [&](eui::Ui& u, const eui::Screen& screen) {
                        ui = &u;
                        app::compose(u, screen);
                    });
                if (f + 1 >= 24 && !runtime.isAnimating()) break;
            }
            for (int tab = 0; tab < module::settings::kSettingTabCount; ++tab) {
                const core::dsl::Element* hit =
                    ui != nullptr
                        ? ui->find("settings.tabs." + std::to_string(tab) +
                                   ".hit")
                        : nullptr;
                if (hit == nullptr) {
                    std::cerr << "uat62_page_shots: settings.tabs." << tab
                              << ".hit 不存在（设置页 tab 栏缺失？）\n";
                    passed = false;
                    continue;
                }
                // 真命中：queue press+release → pollOnce 消费 → 立即 recompose
                probe::click(window, runtime,
                             hit->frame.x + hit->frame.width * 0.5f,
                             hit->frame.y + hit->frame.height * 0.5f);
                for (int f = 0; f < 240; ++f) {
                    probe::composeModuleFrame(
                        runtime, window, "shots", w, h,
                        [&](eui::Ui& u, const eui::Screen& screen) {
                            ui = &u;
                            app::compose(u, screen);
                        });
                    if (f + 1 >= 24 && !runtime.isAnimating()) break;
                }
                if (fontScale != 1.0f) {
                    applyFontScale();
                    for (int f = 0; f < 24; ++f) {
                        probe::composeModuleFrame(
                            runtime, window, "shots", w, h,
                            [&](eui::Ui& u, const eui::Screen& screen) {
                                ui = &u;
                                app::compose(u, screen);
                            });
                    }
                }
                const std::string path =
                    outDir + "/" + twoDigit(settingsIdx + 1) + "-settings-tab" +
                    std::to_string(tab) + "-" +
                    std::to_string(args.windowH) +
                    (fontScale == 1.0f
                         ? ""
                         : "-fs" + std::to_string(static_cast<int>(
                                        fontScale * 100.0f))) +
                    ".png";
                std::vector<unsigned char> rgba;
                int pw = 0;
                int ph = 0;
                // need224 批3：判据失败也落图（-debug 后缀）——白帧/空帧肉眼可诊，
                // 探针不静默吞帧；判据语义不变（失败仍置 passed=false）。
                const bool ok = capture.readFrame(runtime, rgba, pw, ph,
                                                  kCaptureRenderPasses) &&
                    frameHasContent(rgba, pw, ph) &&
                    probe::writeRgbaToPng(path, rgba.data(), pw, ph);
                if (!ok) {
                    if (!rgba.empty()) {
                        const std::string dbg = path.substr(0, path.size() - 4) +
                            "-debug.png";
                        probe::writeRgbaToPng(dbg, rgba.data(), pw, ph);
                        std::cerr << "uat62_page_shots: 判据失败，诊断图 " << dbg
                                  << "\n";
                    }
                    passed = false;
                    continue;
                }
                ++captured;
                std::cout << "uat62_page_shots: " << path << " (" << pw << "x"
                          << ph << ")\n";
            }
        }
    }

    router.stopAll();
    runtime.shutdown(false);
    core::releaseInputQueue(window);
    core::window::destroyWindow(window);
    probe::terminateWindowBackend();
    if (!passed) {
        std::cerr << "uat62_page_shots: FAIL\n";
        return 1;
    }
    std::cout << "uat62_page_shots: PASS（" << args.windowW << "x"
              << args.windowH << "，" << captured << " 页落图";
    if (!skipped.empty()) {
        std::cout << "；跳过 " << skipped.size() << " 页:";
        for (const auto& slug : skipped) std::cout << " " << slug;
    }
    std::cout << "）\n";
    return 0;
}
