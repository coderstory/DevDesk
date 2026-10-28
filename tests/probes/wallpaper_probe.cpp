// wallpaper_probe.cpp — 壁纸模块 P1 真窗口 probe（真 Runtime 帧节拍 + 真 DOM）
//
// 钉 P1 模块开关链路在真实渲染下成立：真 app::compose（本 probe 额外编译
// src/app_sidebar.cpp）+ 真 app::assembly 装配 + 真实注册表 kModuleRegistry +
// 真 dispatchModuleToggle + 真设置模块卡片，只有页面模块实例按需替换为桩。
//
// 断言场景（每条防的事故见 brief）：
//   §1 默认关闭：默认 cfg 经真实注册表装配后 Router 不注册壁纸页、侧栏无条目。
//   §2 打开：dispatchModuleToggle on → 注册 + 侧栏条目当帧出现 + 壁纸页空态渲染。
//   §3 空态截图：--screenshot <路径>（captureIfRequested，退出码随失败转非零）。
//   §3b 设置页：设置→模块 Tab 壁纸格（插拔卡第 5 格）/44×24 开关存在且默认关；--screenshot-seeded
//      <路径>（captureSeedIfRequested）落设置页截图。
//   §4 关闭：off → 注销 + 当前页回 Home + 侧栏条目消失 + 关闭 toast。
// 本阶段不 spawn mpv、不建 WorkerW（P3 才做），probe 只验模块骨架与页面。
//
// app:: 层链接面：app::compose / app::assembly 用真实现（CMake 特例名单额外编译
// app_sidebar.cpp + app_assembly.cpp + module_toggle_bridge.cpp）；main.cpp 不在
// 链接面，故 dslAppConfig() 由本 TU 提供桩，requestShow/requestUpdate 的内联定义
// 也在本 TU 落地（GLFW 初始化后调用安全）。
#include "probes/probe_harness.h"
#include <eui/detail/dsl_app_impl.h>  // app::requestShow/requestUpdate 内联实现

#include "app_assembly.h"   // assembleAndStart / dispatchModuleToggle
#include "app_registry.h"   // kModuleRegistry / create_settings_module
#include "app_sidebar.h"    // 真 app::compose + appNoticeVisible
#include "settings_config.h"
#include "test_support.h"   // tempConfigFile / ScopedConfigPath（临时配置库）

#include "core/module_interface.h"
#include "core/router.h"
#include "core/theme.h"
#include "module_wallpaper/core/presentation.h"
#include "module_wallpaper/wallpaper_service.h"  // WallpaperSnapshot 完整定义（wallpaper_ui.h 只有前置声明）
#include "module_wallpaper/wallpaper_ui.h"
#include "platform/wallpaper_capability.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

// app:: 层链接桩（dsl_app_impl.h 引用 dslAppConfig）
namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config =
        DslAppConfig{}.title("devdesk_wallpaper_probe");
    return config;
}
}  // namespace app

namespace {

// 桩页面模块：只占据门控清单里的非目标页。生命周期/内容由无头用例覆盖，
// 本 probe 只钉「开关 → 下一帧侧栏 + 页面 DOM」这一层。
class StubPageModule final : public ::core::Module {
public:
    StubPageModule(::core::PageId page, std::string name)
        : page_(page), name_(std::move(name)) {}
    ::core::PageId pageId() const override { return page_; }
    std::string displayName() const override { return name_; }
    std::string iconGlyph() const override { return eui::utf8(0xF015); }
    void compose(eui::Ui& ui, const eui::Screen&) override {
        ui.text("stub." + name_).text(name_).size(120.0f, 20.0f).build();
    }

private:
    ::core::PageId page_;
    std::string name_;
};

// §3c 构造快照用的页面模块：真 composeWallpaperUi + 假快照/空动作。截图只验
// 真实渲染管线下的页面几何与观感，不启动 mpv、不依赖服务线程。
class WallpaperUiStubModule final : public ::core::Module {
public:
    module::wallpaper::WallpaperSnapshot snapshot;
    module::wallpaper::UiActions actions;
    module::wallpaper::UiState state;

    ::core::PageId pageId() const override {
        return ::core::PageId::Wallpaper;
    }
    std::string displayName() const override { return "壁纸"; }
    std::string iconGlyph() const override { return eui::utf8(0xF03E); }
    void compose(eui::Ui& ui, const eui::Screen& screen) override {
        module::wallpaper::composeWallpaperUi(ui, screen, snapshot, actions,
                                              state);
    }
};

// 与 WallpaperService::snapshot() 同源拼装文案（presentation 是唯一文案源）。
module::wallpaper::WallpaperSnapshot makeProbeSnapshot(
    module::wallpaper::WallpaperKind kind,
    module::wallpaper::Availability availability,
    module::wallpaper::PlayMode mode) {
    namespace wp = module::wallpaper;
    wp::WallpaperSnapshot snapshot;
    snapshot.kind = kind;
    snapshot.availability = availability;
    snapshot.folderPath =
        availability == wp::Availability::FolderUnconfigured
            ? std::string()
            : (kind == wp::WallpaperKind::Image
                   ? std::string("D:\\Pictures\\Wallpapers")
                   : std::string("D:\\Wallpapers\\动态壁纸"));
    snapshot.videoCount = 12;
    snapshot.imageTotal = 24;
    snapshot.imagePassing = 18;
    snapshot.imageFiltered = 6;
    snapshot.mode = mode;
    snapshot.rotationIntervalSec = 600;
    snapshot.rate = 1.0f;
    snapshot.volume = 0.82f;
    snapshot.muted = false;
    snapshot.pauseOnBattery = false;
    snapshot.imageMinPixels = 3686400;
    snapshot.imageFit = wp::ImageFit::Fill;
    snapshot.secondsUntilRotation = 272;
    snapshot.rotationProgress = 0.45f;

    wp::presentation::StatusInput input;
    input.kind = kind;
    input.availability = availability;
    input.mode = mode;
    input.paused = false;
    input.manualPaused = false;
    input.systemHolds = 0;
    input.itemCount = kind == wp::WallpaperKind::Video ? snapshot.videoCount
                                                       : snapshot.imagePassing;
    input.rotationIntervalSec = snapshot.rotationIntervalSec;
    snapshot.modeLabel = wp::presentation::playModeLabel(mode, kind);
    snapshot.statusTitle = wp::presentation::statusTitle(input);
    snapshot.statusSubtitle = wp::presentation::statusSubtitle(input);
    snapshot.countdownLabel = wp::presentation::countdownLabel(
        kind, mode, false, snapshot.secondsUntilRotation);
    return snapshot;
}

// ModuleLookup 是裸函数指针（无捕获约束）→ 经文件级静态中转
::core::Module* g_wallpaper = nullptr;
::core::Module* lookupForProbe(::core::PageId page) {
    return page == ::core::PageId::Wallpaper ? g_wallpaper : nullptr;
}

constexpr std::size_t kRegistrySize =
    sizeof(::app::registry::kModuleRegistry) /
    sizeof(::app::registry::kModuleRegistry[0]);

}  // namespace

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    // --shots-dir <目录>：构造快照渲染 5 个页面场景并落图（不影响既有断言）。
    std::string shotsDir;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--shots-dir") {
            shotsDir = argv[i + 1];
        }
    }
    // 平台能力前置：不支持的平台（macOS 等）必须诚实跳过（ctest SKIP_RETURN_CODE
    // 77），不能因为建窗成功就把「平台不支持壁纸宿主」报成红色失败——那是把
    // 门禁判据变成平台相关性误报，本地 ctest -L probe 会稳定红。
    if (!::platform::wallpaper::supported()) {
        std::cerr << "wallpaper_probe: 当前平台不支持壁纸宿主"
                     "（platform::wallpaper::supported() == false）"
                     "——本 probe 需要 Windows 真壁纸能力，跳过（exit "
                  << probe::kSkipExitCode << "）\n";
        return probe::kSkipExitCode;
    }
    if (!probe::initializeWindowBackend()) return 1;
    auto window = probe::makeWindow("wallpaper_probe", args.windowW,
                                    args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }

    // 配置库隔离：settings onStart 经 core::appDatabase() 读 app_config；
    // 空库 = 默认配置（enabledModules 不含 wallpaper）。
    const auto cfgFile = test_support::tempConfigFile("wp_", "cfg");
    test_support::ScopedConfigPath scopedCfg(cfgFile);

    StubPageModule home(::core::PageId::Home, "首页");
    StubPageModule markdown(::core::PageId::Markdown, "笔记");
    StubPageModule todo(::core::PageId::Todo, "待办");
    StubPageModule claude(::core::PageId::ClaudeProxy, "Claude");
    StubPageModule about(::core::PageId::About, "关于");
    std::unique_ptr<::core::Module> settings(create_settings_module());
    std::unique_ptr<::core::Module> wallpaper;  // dispatch on 时才建实例
    // UI 截图桩与生产 WallpaperModule 同生命周期（main 末尾才析构）：compose
    // 闭包按引用捕获其 state，runtime 上一帧元素树在下一帧 recompose 前仍持有
    // [&state]——桩若先于 runtime 析构，析构窗口内任何排队输入都会打在已析构
    // state 上（UAF，审计 F4）。
    std::unique_ptr<WallpaperUiStubModule> uiStub;

    ::core::Router router;
    core::dsl::Runtime runtime;

    bool passed = true;
    auto check = [&](bool cond, const std::string& what) {
        if (!cond) {
            std::cerr << "wallpaper_probe: FAIL " << what << "\n";
            passed = false;
        }
    };

    // ── §0 DPI 适配：按实测 framebuffer 反推可用逻辑尺寸 ──
    // GLFW 窗口尺寸按物理像素生效，而 compose 走逻辑尺寸。125%~135% 缩放下按
    // 1200×1000 建窗，framebuffer 仍只有 1200×1000 物理（逻辑约 886×738），内容
    // 放大 1.25~1.35 倍后右侧/底部被裁。先请求 物理 = 逻辑×scale；但系统可能按
    // 屏幕工作区上限钳小窗口（本机 135% 下高被钳到 1065），所以**不能假定请求
    // 尺寸成立**：轮询等尺寸落定后，再用实测 framebuffer / 实测 scale 反推
    // effW/effH，compose 与全部断言都以 eff 为准。
    auto* glfwWindow = static_cast<GLFWwindow*>(window);
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    glfwGetWindowContentScale(glfwWindow, &scaleX, &scaleY);
    check(scaleX > 0.0f && scaleY > 0.0f,
          "§0 DPI：contentScale 应为正数（实测 " + std::to_string(scaleX) +
              "x" + std::to_string(scaleY) + "）");
    if (!(scaleX > 0.0f)) scaleX = 1.0f;
    if (!(scaleY > 0.0f)) scaleY = 1.0f;
    glfwSetWindowSize(glfwWindow,
                      static_cast<int>(std::lround(
                          static_cast<double>(args.windowW) * scaleX)),
                      static_cast<int>(std::lround(
                          static_cast<double>(args.windowH) * scaleY)));
    // 轮询到 framebuffer 尺寸连续 5 帧稳定（窗口管理器钳尺寸需要若干帧）。
    int fbW = 0;
    int fbH = 0;
    int stableFrames = 0;
    int lastW = -1;
    int lastH = -1;
    for (int attempt = 0; attempt < 200; ++attempt) {
        glfwPollEvents();
        glfwWaitEventsTimeout(0.01);
        glfwGetFramebufferSize(glfwWindow, &fbW, &fbH);
        if (fbW == lastW && fbH == lastH) {
            if (++stableFrames >= 5) break;
        } else {
            lastW = fbW;
            lastH = fbH;
            stableFrames = 0;
        }
    }
    // 重新读 scale（窗口尺寸变化/跨屏都可能改变它），再反推有效逻辑尺寸。
    glfwGetWindowContentScale(glfwWindow, &scaleX, &scaleY);
    if (!(scaleX > 0.0f)) scaleX = 1.0f;
    if (!(scaleY > 0.0f)) scaleY = 1.0f;
    const int effW = static_cast<int>(
        std::lround(static_cast<double>(fbW) / static_cast<double>(scaleX)));
    const int effH = static_cast<int>(
        std::lround(static_cast<double>(fbH) / static_cast<double>(scaleY)));
    std::cout << "wallpaper_probe: contentScale=" << scaleX << "x" << scaleY
              << " framebuffer=" << fbW << "x" << fbH
              << " effLogical=" << effW << "x" << effH << "\n";
    check(fbW > 0 && fbH > 0 && effW > 0 && effH > 0,
          "§0 DPI：framebuffer 与有效逻辑尺寸应为正（实测 framebuffer " +
              std::to_string(fbW) + "x" + std::to_string(fbH) + "，eff " +
              std::to_string(effW) + "x" + std::to_string(effH) + "）");
    check(std::abs(fbW - static_cast<int>(std::lround(
                            static_cast<double>(effW) * scaleX))) <= 2 &&
              std::abs(fbH - static_cast<int>(std::lround(
                            static_cast<double>(effH) * scaleY))) <= 2,
          "§0 DPI：framebuffer " + std::to_string(fbW) + "x" +
              std::to_string(fbH) + " 应等于实测 eff " + std::to_string(effW) +
              "x" + std::to_string(effH) + " × scale（±2）");

    // 截图前复核：framebuffer 必须仍等于 eff×当前 scale（窗口漂移/缩放变化即红）
    auto assertFramebufferMatchesLogical = [&](const std::string& when) {
        int nowW = 0;
        int nowH = 0;
        float nowScaleX = 1.0f;
        float nowScaleY = 1.0f;
        glfwGetFramebufferSize(glfwWindow, &nowW, &nowH);
        glfwGetWindowContentScale(glfwWindow, &nowScaleX, &nowScaleY);
        const int wantW =
            nowScaleX > 0.0f
                ? static_cast<int>(std::lround(static_cast<double>(effW) *
                                               nowScaleX))
                : effW;
        const int wantH =
            nowScaleY > 0.0f
                ? static_cast<int>(std::lround(static_cast<double>(effH) *
                                               nowScaleY))
                : effH;
        check(std::abs(nowW - wantW) <= 2 && std::abs(nowH - wantH) <= 2,
              when + "：framebuffer " + std::to_string(nowW) + "x" +
                  std::to_string(nowH) + " 应等于 eff " +
                  std::to_string(effW) + "x" + std::to_string(effH) +
                  " × scale " + std::to_string(nowScaleX) + "x" +
                  std::to_string(nowScaleY) + "（±2）");
    };

    // 截图证据链：文件必须存在且非空（0 字节视为落图失败，不能只查存在）
    auto assertScreenshotFile = [&](const std::string& path,
                                    const std::string& what) {
        std::error_code ec;
        const bool exists = std::filesystem::exists(path, ec);
        const auto size =
            exists && !ec ? std::filesystem::file_size(path, ec) : 0u;
        check(exists && !ec && size > 0,
              what + "应落盘且非空: " + path);
    };

    auto cleanup = [&]() {
        // stopAll 先收模块后台线程（settings 的 join 在 onStop），再断进程级
        // appRouter 引用——否则 atexit 钩子会在栈对象析构后再碰 router。
        router.stopAll();
        ::core::registerAppRouter(nullptr);
        g_wallpaper = nullptr;
        wallpaper.reset();
        uiStub.reset();
        runtime.shutdown(false);
        ::core::releaseInputQueue(window);
        ::core::window::destroyWindow(window);
        probe::terminateWindowBackend();
    };

    // ── §1 默认关闭：默认 cfg + 真实注册表 + 真装配 ──
    const module::settings::SettingsConfig defaultCfg;
    auto entries = app::assembly::buildGatedEntries(
        defaultCfg, ::app::registry::kModuleRegistry, kRegistrySize);
    bool defaultGatingHasWallpaper = false;
    for (auto& entry : entries) {
        switch (entry.page) {
            case ::core::PageId::Home: entry.module = &home; break;
            case ::core::PageId::Markdown: entry.module = &markdown; break;
            case ::core::PageId::Todo: entry.module = &todo; break;
            case ::core::PageId::ClaudeProxy: entry.module = &claude; break;
            case ::core::PageId::Settings: entry.module = settings.get(); break;
            case ::core::PageId::About: entry.module = &about; break;
            case ::core::PageId::Wallpaper:
                defaultGatingHasWallpaper = true;
                break;
            case ::core::PageId::Agent:
                break;  // 默认关闭，门控清单本就不含
            case ::core::PageId::Memory:
                break;  // 默认开启但不在本探针注册集（壁纸探针只关心壁纸页）
            case ::core::PageId::Mcp:
                break;  // 默认关闭，门控清单本就不含
        }
    }
    check(!defaultGatingHasWallpaper,
          "§1 默认 cfg：真实注册表的门控清单不应含壁纸页");
    check(::platform::wallpaper::supported(),
          "§1 平台能力：当前平台应支持壁纸宿主（Windows true）");
    app::assembly::assembleAndStart(router, defaultCfg, entries, {});
    check(!router.isRegistered(::core::PageId::Wallpaper),
          "§1 默认关闭：Router 不应注册壁纸页");

    runtime.initialize(window);
    // 内联定义落地本 TU（见文件头说明）：GLFW 初始化后调用安全
    app::requestUpdate();
    app::requestShow();

    // 逻辑视口 = 实测 framebuffer / 实测 scale（不假定等于 --size 请求值：
    // 135% 本机上高会被屏幕工作区钳到约 786，按 1000 布局底部必被裁）。
    const float w = static_cast<float>(effW);
    const float h = static_cast<float>(effH);
    core::dsl::Ui* ui = nullptr;
    auto composeFrame = [&]() {
        probe::composeModuleFrame(runtime, window, "app", w, h,
                                  [&](eui::Ui& u, const eui::Screen& screen) {
                                      ui = &u;
                                      app::compose(u, screen);
                                  });
    };
    // 真实命中分发（同 module_toggle_sidebar_probe）：press + release 排队 →
    // 下一 composeFrame 的 update 消费。
    auto clickElement = [&](const core::dsl::Element& element) {
        const float cx = element.frame.x + element.frame.width * 0.5f;
        const float cy = element.frame.y + element.frame.height * 0.5f;
        core::queuePointerButton(window, cx, cy, core::PointerButton::Left,
                                 core::PointerAction::Press, {});
        core::queuePointerButton(window, cx, cy, core::PointerButton::Left,
                                 core::PointerAction::Release, {});
    };

    composeFrame();
    if (ui == nullptr) {
        std::cerr << "wallpaper_probe: ui 为空（compose 未执行）\n";
        cleanup();
        return 3;
    }
    check(ui->find("app.sidebar.item.Home.hit") != nullptr,
          "§1 装配基线：Home 条目应存在");
    check(ui->find("app.sidebar.item.Wallpaper.hit") == nullptr,
          "§1 默认关闭：侧栏不应出现壁纸条目");

    // ── §2 打开：注册 + 侧栏出现 + 空态渲染 ──
    wallpaper.reset(create_wallpaper_module());
    check(wallpaper != nullptr,
          "§2 平台能力：create_wallpaper_module 应返回实例");
    if (wallpaper == nullptr) {
        cleanup();
        return 4;
    }
    g_wallpaper = wallpaper.get();

    app::assembly::dispatchModuleToggle(module::settings::kModuleIdWallpaper,
                                        true, router, &lookupForProbe);
    composeFrame();
    check(router.isRegistered(::core::PageId::Wallpaper),
          "§2 开启：Router 应注册壁纸页");
    check(ui->find("app.sidebar.item.Wallpaper.hit") != nullptr,
          "§2 开启：侧栏应出现壁纸条目");
    check(ui->find("app.sidebar.item.Wallpaper.label") != nullptr,
          "§2 开启：展开态壁纸 label 应存在");

    router.navigateTo(::core::PageId::Wallpaper);
    composeFrame();
    check(router.currentPage() == ::core::PageId::Wallpaper,
          "§2 开启：导航后当前页应为壁纸");
    const auto* wallpaperTitle = ui->find("wallpaper.title");
    const auto* wallpaperTabs = ui->find("wallpaper.tabs");
    check(wallpaperTitle != nullptr && wallpaperTitle->text == "壁纸",
          "§2 空态：页面标题「壁纸」应渲染");
    check(wallpaperTabs != nullptr, "§2 空态：播放/片库 Tab 应渲染");
    check(ui->find("wallpaper.card.empty") != nullptr,
          "§2 空态：未配置目录时应渲染空态卡");

    // ── §3 空页面截图 ──
    assertFramebufferMatchesLogical("§3 截图前");
    probe::captureIfRequested(args, window, runtime, &passed);
    if (!args.screenshotPath.empty()) {
        assertScreenshotFile(args.screenshotPath, "§3 空页面截图");
    }

    // ── §3b 设置 → 模块 Tab：壁纸卡存在且默认关 ──
    router.navigateTo(::core::PageId::Settings);
    composeFrame();
    const auto* tabModules = ui->find("settings.tabs.1.hit");
    check(tabModules != nullptr, "§3b 设置页：模块 tab 应存在");
    if (tabModules != nullptr) {
        clickElement(*tabModules);
        composeFrame();
    }
    // 2026-10-27 用户真机拍板：独立全宽卡废除 → 壁纸 = 模块插拔卡第 5 格
    //（格无独立根元素，存在性锚 = 格 name）。
    const auto* wallpaperCell = ui->find("settings.card.wallpaper.name");
    check(wallpaperCell != nullptr, "§3b 设置页：壁纸格（插拔卡内第 5 格）应存在");
    const auto* cardsScroll = ui->find("settings.cards.scroll");
    check(cardsScroll != nullptr, "§3b 设置页：卡片滚动视口应存在");
    // 滚动视口内断言：元素 frame 必须整体落在视口可见区内——DOM 存在但被
    // scrollView 裁掉（越出视口底/顶）在截图上等同于缺失，必须判红。
    auto assertInsideScrollViewport = [&](const core::dsl::Element& element,
                                          const std::string& what) {
        if (cardsScroll == nullptr) {
            return;
        }
        const float relTop = element.frame.y - cardsScroll->frame.y;
        const float relBottom = relTop + element.frame.height;
        check(relTop >= -0.5f &&
                  relBottom <= cardsScroll->frame.height + 0.5f,
              what + " 应落在滚动视口内（相对 y=" +
                  std::to_string(relTop) + "，相对底=" +
                  std::to_string(relBottom) + "，视口高=" +
                  std::to_string(cardsScroll->frame.height) +
                  "；要求 y≥0 且 y+height≤视口高）");
    };
    if (wallpaperCell != nullptr) {
        // 格在宿主卡内（末行首列），不再有全宽口径——右缘契约由插拔卡自身承担。
        assertInsideScrollViewport(*wallpaperCell, "§3b 壁纸格");
    }
    const auto* wallpaperTrack =
        ui->find("settings.card.wallpaper.sw.track");
    check(wallpaperTrack != nullptr, "§3b 设置页：壁纸开关应存在（44×24，格内右贴）");
    if (wallpaperTrack != nullptr) {
        check(std::fabs(wallpaperTrack->frame.width - 44.0f) < 0.01f &&
                  std::fabs(wallpaperTrack->frame.height - 24.0f) < 0.01f,
              "§3b 设置页：壁纸开关几何应为 44×24");
        assertInsideScrollViewport(*wallpaperTrack, "§3b 壁纸开关");
    }
    const auto* wallpaperThumb =
        ui->find("settings.card.wallpaper.sw.thumb");
    check(wallpaperThumb != nullptr &&
              std::fabs(wallpaperThumb->x - 2.0f) < 0.01f,
          "§3b 设置页：壁纸开关默认应为关（滑块贴左 2px）");

    // ── §3b 设置页截图前复核 ──
    assertFramebufferMatchesLogical("§3b 截图前");
    probe::captureSeedIfRequested(args, window, runtime, &passed);
    if (!args.screenshotSeedPath.empty()) {
        assertScreenshotFile(args.screenshotSeedPath, "§3b 设置页截图");
    }

    // ── §3c 五张设计截图（--shots-dir）：构造快照 + 真 composeWallpaperUi ──
    if (!shotsDir.empty()) {
        std::error_code dirEc;
        std::filesystem::create_directories(shotsDir, dirEc);
        check(!dirEc, "§3c 截图目录应可创建: " + shotsDir);

        // 换页面实例为 UI 桩（真元素树、假快照）；服务线程已随上一轮 off 收束。
        // 桩本体在 main 作用域（见声明处不变量），此处只赋值。
        uiStub = std::make_unique<WallpaperUiStubModule>();
        app::assembly::dispatchModuleToggle(module::settings::kModuleIdWallpaper,
                                            false, router, &lookupForProbe);
        g_wallpaper = uiStub.get();
        app::assembly::dispatchModuleToggle(module::settings::kModuleIdWallpaper,
                                            true, router, &lookupForProbe);
        router.navigateTo(::core::PageId::Wallpaper);

        // 逻辑尺寸 → 物理尺寸（DPI 适配），等窗口尺寸稳定后按实测反推 eff。
        auto resizeToLogical = [&](int logicalW, int logicalH) {
            float sx = 1.0f;
            float sy = 1.0f;
            glfwGetWindowContentScale(glfwWindow, &sx, &sy);
            if (!(sx > 0.0f)) sx = 1.0f;
            if (!(sy > 0.0f)) sy = 1.0f;
            glfwSetWindowSize(
                glfwWindow,
                static_cast<int>(std::lround(logicalW * sx)),
                static_cast<int>(std::lround(logicalH * sy)));
            int fbW = 0;
            int fbH = 0;
            int lastW = -1;
            int lastH = -1;
            int stable = 0;
            for (int attempt = 0; attempt < 240; ++attempt) {
                glfwPollEvents();
                glfwWaitEventsTimeout(0.01);
                glfwGetFramebufferSize(glfwWindow, &fbW, &fbH);
                if (fbW == lastW && fbH == lastH) {
                    if (++stable >= 5) break;
                } else {
                    lastW = fbW;
                    lastH = fbH;
                    stable = 0;
                }
            }
            glfwGetWindowContentScale(glfwWindow, &sx, &sy);
            if (!(sx > 0.0f)) sx = 1.0f;
            if (!(sy > 0.0f)) sy = 1.0f;
            return std::pair<int, int>{
                static_cast<int>(std::lround(fbW / sx)),
                static_cast<int>(std::lround(fbH / sy))};
        };

        struct Shot {
            const char* file;
            int logicalW;
            int logicalH;
            bool dark;
            module::wallpaper::WallpaperKind kind;
            module::wallpaper::Availability availability;
            module::wallpaper::PlayMode mode;
            int tab;
        };
        namespace wp = module::wallpaper;
        const Shot shots[] = {
            {"play-video-1600x1000-light.png", 1600, 1000, false,
             wp::WallpaperKind::Video, wp::Availability::Playing,
             wp::PlayMode::LoopList, 0},
            {"play-video-1000x640-light.png", 1000, 640, false,
             wp::WallpaperKind::Video, wp::Availability::Playing,
             wp::PlayMode::LoopList, 0},
            {"play-image-1600x1000-dark.png", 1600, 1000, true,
             wp::WallpaperKind::Image, wp::Availability::Playing,
             wp::PlayMode::LoopList, 0},
            {"library-video-1600x1000-light.png", 1600, 1000, false,
             wp::WallpaperKind::Video, wp::Availability::Playing,
             wp::PlayMode::LoopList, 1},
            {"library-empty-1000x640-light.png", 1000, 640, false,
             wp::WallpaperKind::Video, wp::Availability::FolderMissing,
             wp::PlayMode::LoopSingle, 1},
        };

        for (const Shot& shot : shots) {
            if (shot.dark) {
                ::core::theme::setDark();
            } else {
                ::core::theme::setLight();
            }
            uiStub->snapshot =
                makeProbeSnapshot(shot.kind, shot.availability, shot.mode);
            uiStub->state = wp::UiState{};
            uiStub->state.tab = shot.tab;

            const auto [effW, effH] =
                resizeToLogical(shot.logicalW, shot.logicalH);
            check(effW > 0 && effH > 0,
                  std::string("§3c ") + shot.file + "：有效逻辑尺寸应为正");

            // 连续两帧：第 1 帧布局/条件元素落定，第 2 帧拍稳定树。
            for (int frame = 0; frame < 2; ++frame) {
                ui = nullptr;
                probe::composeModuleFrame(
                    runtime, window, "app", static_cast<float>(effW),
                    static_cast<float>(effH),
                    [&](eui::Ui& frameUi, const eui::Screen& frameScreen) {
                        ui = &frameUi;
                        app::compose(frameUi, frameScreen);
                    });
            }
            check(ui != nullptr, std::string("§3c ") + shot.file +
                                     "：compose 未执行");
            if (ui == nullptr) continue;

            const auto* page = ui->find("wallpaper.page");
            const auto* scroll = ui->find("wallpaper.scroll");
            check(page != nullptr,
                  std::string("§3c ") + shot.file + "：缺页面根");
            check(scroll != nullptr,
                  std::string("§3c ") + shot.file + "：缺内容滚动视口");
            check(ui->find("wallpaper.scroll.scroll") == nullptr,
                  std::string("§3c ") + shot.file +
                      "：首屏不应出现纵向滚动条");
            if (page != nullptr) {
                check(std::fabs(page->frame.width -
                                static_cast<float>(effW - 208)) <= 0.6f,
                      std::string("§3c ") + shot.file + "：页面宽应为 eff − 208（实测 " +
                          std::to_string(page->frame.width) + "）");
            }
            // 卡片不越界：x/右缘落在页面内、上下落在滚动视口内。
            std::function<void(const core::dsl::Element&)> visit =
                [&](const core::dsl::Element& element) {
                    const int dots = static_cast<int>(
                        std::count(element.id.begin(), element.id.end(), '.'));
                    const bool isCard =
                        element.id.rfind("wallpaper.card.", 0) == 0 &&
                        dots == 2;
                    if (isCard && page != nullptr && scroll != nullptr) {
                        const float pageRight =
                            page->frame.x + page->frame.width;
                        check(element.frame.x >= page->frame.x - 0.5f &&
                                  element.frame.x + element.frame.width <=
                                      pageRight + 0.5f,
                              std::string("§3c ") + shot.file + "：" +
                                  element.id + " 横向越界");
                        check(element.frame.y >= scroll->frame.y - 0.5f &&
                                  element.frame.y + element.frame.height <=
                                      scroll->frame.y + scroll->frame.height +
                                          0.5f,
                              std::string("§3c ") + shot.file + "：" +
                                  element.id + " 纵向越界");
                    }
                    for (const auto& child : element.children) {
                        visit(*child);
                    }
                };
            for (const auto& root : ui->roots()) {
                visit(*root);
            }

            // 截图尺寸必须等于当前 framebuffer（完整帧，沿用 DPI 适配）。
            int fbW = 0;
            int fbH = 0;
            glfwGetFramebufferSize(glfwWindow, &fbW, &fbH);
            probe::FrameCapture capture(window);
            check(capture.ok(),
                  std::string("§3c ") + shot.file + "：帧捕获初始化失败: " +
                      capture.error());
            if (!capture.ok()) continue;
            check(capture.pixelWidth() == fbW && capture.pixelHeight() == fbH,
                  std::string("§3c ") + shot.file + "：截图尺寸 " +
                      std::to_string(capture.pixelWidth()) + "x" +
                      std::to_string(capture.pixelHeight()) +
                      " 应等于 framebuffer " + std::to_string(fbW) + "x" +
                      std::to_string(fbH));
            const std::string path =
                (std::filesystem::path(shotsDir) / shot.file).string();
            check(capture.capturePng(runtime, path),
                  std::string("§3c ") + shot.file + "：落图失败");
            assertScreenshotFile(path, std::string("§3c ") + shot.file);
        }

        // 截图桩只活在本段：先切回真模块实例，避免 §4 的 lookup 拿到已析构对象。
        app::assembly::dispatchModuleToggle(module::settings::kModuleIdWallpaper,
                                            false, router, &lookupForProbe);
        g_wallpaper = wallpaper.get();
        app::assembly::dispatchModuleToggle(module::settings::kModuleIdWallpaper,
                                            true, router, &lookupForProbe);
        router.navigateTo(::core::PageId::Wallpaper);
        ::core::theme::setDark();  // 恢复到测试默认（后续断言与主题无关）
    }

    // ── §4 关闭：注销 + 当前页回 Home + 侧栏条目消失 ──
    router.navigateTo(::core::PageId::Wallpaper);
    composeFrame();
    check(router.currentPage() == ::core::PageId::Wallpaper,
          "§4 前置：当前页应回到壁纸");
    app::assembly::dispatchModuleToggle(module::settings::kModuleIdWallpaper,
                                        false, router, &lookupForProbe);
    composeFrame();
    check(!router.isRegistered(::core::PageId::Wallpaper),
          "§4 关闭：Router 应注销壁纸页");
    check(router.currentPage() == ::core::PageId::Home,
          "§4 关闭：当前页应回 Home");
    check(ui->find("app.sidebar.item.Wallpaper.hit") == nullptr,
          "§4 关闭：侧栏条目应消失");
    check(ui->find("stub.首页") != nullptr,
          "§4 关闭：首页内容应渲染");
    check(app::appNoticeVisible(), "§4 关闭：关闭 toast 应可见");

    cleanup();
    if (!passed) {
        std::cerr << "FAIL\n";
    } else {
        std::cout << "wallpaper_probe: PASS（请求 " << args.windowW << "x"
                  << args.windowH << "，实际逻辑 " << effW << "x" << effH
                  << "，framebuffer " << fbW << "x" << fbH << "）\n";
    }
    return passed ? 0 : 1;
}
