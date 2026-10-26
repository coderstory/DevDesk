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
//   §3b 设置页：设置→模块 Tab 壁纸卡/44×24 开关存在且默认关；--screenshot-seeded
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
#include "platform/wallpaper_capability.h"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

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

    ::core::Router router;
    core::dsl::Runtime runtime;

    bool passed = true;
    auto check = [&](bool cond, const std::string& what) {
        if (!cond) {
            std::cerr << "wallpaper_probe: FAIL " << what << "\n";
            passed = false;
        }
    };

    auto cleanup = [&]() {
        // stopAll 先收模块后台线程（settings 的 join 在 onStop），再断进程级
        // appRouter 引用——否则 atexit 钩子会在栈对象析构后再碰 router。
        router.stopAll();
        ::core::registerAppRouter(nullptr);
        g_wallpaper = nullptr;
        wallpaper.reset();
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

    const float w = static_cast<float>(args.windowW);
    const float h = static_cast<float>(args.windowH);
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
    const auto* wallpaperHint = ui->find("wallpaper.hint");
    check(wallpaperTitle != nullptr && wallpaperTitle->text == "壁纸",
          "§2 空态：页面标题「壁纸」应渲染");
    check(wallpaperHint != nullptr && !wallpaperHint->text.empty(),
          "§2 空态：说明文案应渲染");

    // ── §3 空页面截图 ──
    probe::captureIfRequested(args, window, runtime, &passed);
    if (!args.screenshotPath.empty()) {
        check(std::filesystem::exists(args.screenshotPath),
              "§3 空页面截图应落盘: " + args.screenshotPath);
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
    check(ui->find("settings.card.wallpaper") != nullptr,
          "§3b 设置页：壁纸卡应存在");
    const auto* wallpaperTrack =
        ui->find("settings.card.wallpaper.sw.track");
    check(wallpaperTrack != nullptr, "§3b 设置页：壁纸开关应存在");
    if (wallpaperTrack != nullptr) {
        check(std::fabs(wallpaperTrack->frame.width - 44.0f) < 0.01f &&
                  std::fabs(wallpaperTrack->frame.height - 24.0f) < 0.01f,
              "§3b 设置页：壁纸开关几何应为 44×24");
    }
    const auto* wallpaperThumb =
        ui->find("settings.card.wallpaper.sw.thumb");
    check(wallpaperThumb != nullptr &&
              std::fabs(wallpaperThumb->x - 2.0f) < 0.01f,
          "§3b 设置页：壁纸开关默认应为关（滑块贴左 2px）");

    probe::captureSeedIfRequested(args, window, runtime, &passed);
    if (!args.screenshotSeedPath.empty()) {
        check(std::filesystem::exists(args.screenshotSeedPath),
              "§3b 设置页截图应落盘: " + args.screenshotSeedPath);
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
        std::cout << "wallpaper_probe: PASS（" << args.windowW << "x"
                  << args.windowH << "）\n";
    }
    return passed ? 0 : 1;
}