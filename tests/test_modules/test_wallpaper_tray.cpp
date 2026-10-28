// tests/test_modules/test_wallpaper_tray.cpp
// 托盘壁纸组（P5 尾）：spec 形状（关=无组/开=组+项序+文案口径）、打开页映射、
// 壁纸托盘桥的注入/路由/复位、让路态下播放暂停的真实语义。
// 每条用例描述含 ASCII "wallpaper"（--filter 只认 ASCII）。
// 跨文件前提：
//   - spec 播放/暂停文案口径 = manualPaused（与壁纸页暂停钮同源），不用 paused
//     （held）驱动——让路态下 held=true 而桥命令只切手动位，见 tray_menu_spec 契约。
//   - 桥双槽是进程级全局：用例内 RAII 复位（同 test_module_toggle_bridge 的纪律，
//     不能写在用例尾部——断言 fail-fast 早退会跳过尾部语句）。
//   - 服务语义用例注入假玩家/假扫描器/临时设置路径，不建桌面宿主、不 spawn mpv、
//     不碰真实会话/电源。
#include "doctest.h"
#include "test_support.h"

#include "app_assembly.h"
#include "core/tray_menu_spec.h"
#include "wallpaper_tray_bridge.h"

#include "module_wallpaper/core/model.h"
#include "module_wallpaper/platform/player_backend.h"
#include "module_wallpaper/wallpaper_service.h"

#include <string>
#include <vector>

namespace {

namespace wp = module::wallpaper;

using Action = core::tray::TrayMenuAction;

// ---- 桥 recording 桩（函数指针缝 → 桩与记录只能走文件级静态） ----
struct TrayRecording {
    int stateCalls = 0;
    int commandCalls = 0;
    std::vector<app::tray::WallpaperTrayCommand> commands;
    app::tray::WallpaperTrayState state{};
};
TrayRecording& trayRec() {
    static TrayRecording rec;
    return rec;
}
app::tray::WallpaperTrayState recordState() {
    ++trayRec().stateCalls;
    return trayRec().state;
}
void recordCommand(app::tray::WallpaperTrayCommand command) {
    auto& rec = trayRec();
    ++rec.commandCalls;
    rec.commands.push_back(command);
}
// 复位守卫：任何退出路径都把双槽回厂（同 test_module_toggle_bridge 的理由）
struct TrayBridgeGuard {
    TrayBridgeGuard() { trayRec() = TrayRecording{}; }
    ~TrayBridgeGuard() { app::tray::setWallpaperTrayBridge(nullptr, nullptr); }
    TrayBridgeGuard(const TrayBridgeGuard&) = delete;
    TrayBridgeGuard& operator=(const TrayBridgeGuard&) = delete;
};

} // namespace

// 防：壁纸组漏进缺省形状（原生降级菜单/既有用例基线靠它保持 2 项）。
REGISTER_TEST(wallpaper_tray_spec_off_equals_default,
              "wallpaper tray: 组关 = 缺省 spec 逐项全等（2 项基线不变）", {
    const auto baseline = core::tray::trayMenuSpec();
    const auto off = core::tray::trayMenuSpec(false, true);
    DOCTEST_REQUIRE_EQ(off.size(), baseline.size());
    for (std::size_t i = 0; i < baseline.size(); ++i) {
        DOCTEST_CHECK_EQ(off[i].text, baseline[i].text);
        DOCTEST_CHECK_EQ(off[i].iconCodepoint, baseline[i].iconCodepoint);
        DOCTEST_CHECK(off[i].action == baseline[i].action);
    }
    DOCTEST_CHECK_EQ(off.size(), static_cast<std::size_t>(2));
    return true;
});

// 防：组内项数/顺序漂移（styled 菜单 path 下标映射依赖它）与分隔线/action 完整性。
REGISTER_TEST(wallpaper_tray_spec_on_shape,
              "wallpaper tray: 组开 = 分隔线+播放暂停/下一张/停止/打开页 固定顺序", {
    const auto spec = core::tray::trayMenuSpec(true, false);
    DOCTEST_REQUIRE_EQ(spec.size(), static_cast<std::size_t>(7));
    DOCTEST_CHECK_EQ(spec[0].action, Action::ShowMainWindow);
    DOCTEST_CHECK_EQ(spec[1].action, Action::Exit);
    DOCTEST_CHECK_EQ(spec[2].text, std::string("-"));
    DOCTEST_CHECK(spec[2].action == Action::None);
    DOCTEST_CHECK_EQ(spec[3].text, std::string("暂停壁纸"));
    DOCTEST_CHECK_EQ(spec[3].iconCodepoint, 0xF04C);
    DOCTEST_CHECK(spec[3].action == Action::WallpaperTogglePause);
    DOCTEST_CHECK_EQ(spec[4].text, std::string("下一张"));
    DOCTEST_CHECK_EQ(spec[4].iconCodepoint, 0xF051);
    DOCTEST_CHECK(spec[4].action == Action::WallpaperNext);
    DOCTEST_CHECK_EQ(spec[5].text, std::string("停止壁纸"));
    DOCTEST_CHECK_EQ(spec[5].iconCodepoint, 0xF04D);
    DOCTEST_CHECK(spec[5].action == Action::WallpaperStop);
    DOCTEST_CHECK_EQ(spec[6].text, std::string("打开壁纸页"));
    DOCTEST_CHECK_EQ(spec[6].iconCodepoint, 0xF03E);
    DOCTEST_CHECK(spec[6].action == Action::NavigateWallpaper);
    return true;
});

// 防：播放/暂停文案误用 paused（held）驱动——让路态（手动位未开）必须仍显示
// 「暂停壁纸」，否则「继续」字样会配出落手动暂停位的反向效果。
REGISTER_TEST(wallpaper_tray_spec_pause_text_flip,
              "wallpaper tray: 文案口径 = manualPaused（让路态不翻成「继续」）", {
    const auto manual = core::tray::trayMenuSpec(true, true);
    DOCTEST_REQUIRE_EQ(manual.size(), static_cast<std::size_t>(7));
    DOCTEST_CHECK_EQ(manual[3].text, std::string("继续壁纸"));
    DOCTEST_CHECK_EQ(manual[3].iconCodepoint, 0xF04B);
    DOCTEST_CHECK(manual[3].action == Action::WallpaperTogglePause);
    // 其余壁纸项不受暂停位影响
    DOCTEST_CHECK_EQ(manual[4].text, std::string("下一张"));
    DOCTEST_CHECK_EQ(manual[5].text, std::string("停止壁纸"));
    DOCTEST_CHECK_EQ(manual[6].text, std::string("打开壁纸页"));
    // held 态（手动位未开）不得驱动文案
    const auto heldOnly = core::tray::trayMenuSpec(true, false);
    DOCTEST_CHECK_EQ(heldOnly[3].text, std::string("暂停壁纸"));
    return true;
});

// 防：打开壁纸页丢映射 / 壁纸命令被误当导航。
REGISTER_TEST(wallpaper_tray_action_page_mapping,
              "wallpaper tray: NavigateWallpaper→壁纸页；三个壁纸命令非导航", {
    const auto page =
        app::assembly::pageForTrayAction(Action::NavigateWallpaper);
    DOCTEST_REQUIRE(page.has_value());
    DOCTEST_CHECK(*page == ::core::PageId::Wallpaper);
    DOCTEST_CHECK(!app::assembly::pageForTrayAction(Action::WallpaperTogglePause)
                       .has_value());
    DOCTEST_CHECK(
        !app::assembly::pageForTrayAction(Action::WallpaperNext).has_value());
    DOCTEST_CHECK(
        !app::assembly::pageForTrayAction(Action::WallpaperStop).has_value());
    return true;
});

// 防：桥未装配时查询/命令必须安全缺省；装配后命令按序 1:1 转发、复位即回厂。
REGISTER_TEST(wallpaper_tray_bridge_roundtrip,
              "wallpaper tray: 桥注入→查询/命令路由；复位→absent/no-op", {
    TrayBridgeGuard guard;
    // 未装配缺省：查询 absent、命令 no-op 不崩
    const auto absent = app::tray::wallpaperTrayState();
    DOCTEST_CHECK(!absent.present);
    DOCTEST_CHECK(!absent.manualPaused);
    app::tray::dispatchWallpaperTrayCommand(
        app::tray::WallpaperTrayCommand::TogglePause);
    DOCTEST_CHECK_EQ(trayRec().commandCalls, 0);

    // 注入 recording 桩：命令按序 1:1 路由
    app::tray::setWallpaperTrayBridge(&recordState, &recordCommand);
    trayRec().state = app::tray::WallpaperTrayState{true, true};
    const auto wired = app::tray::wallpaperTrayState();
    DOCTEST_CHECK(wired.present);
    DOCTEST_CHECK(wired.manualPaused);
    app::tray::dispatchWallpaperTrayCommand(
        app::tray::WallpaperTrayCommand::TogglePause);
    app::tray::dispatchWallpaperTrayCommand(
        app::tray::WallpaperTrayCommand::Next);
    app::tray::dispatchWallpaperTrayCommand(
        app::tray::WallpaperTrayCommand::Stop);
    DOCTEST_REQUIRE_EQ(trayRec().commands.size(), static_cast<std::size_t>(3));
    DOCTEST_CHECK(trayRec().commands[0] ==
                  app::tray::WallpaperTrayCommand::TogglePause);
    DOCTEST_CHECK(trayRec().commands[1] ==
                  app::tray::WallpaperTrayCommand::Next);
    DOCTEST_CHECK(trayRec().commands[2] ==
                  app::tray::WallpaperTrayCommand::Stop);

    // 复位（守卫析构同款路径）：查询回 absent、命令丢弃
    app::tray::setWallpaperTrayBridge(nullptr, nullptr);
    const auto reset = app::tray::wallpaperTrayState();
    DOCTEST_CHECK(!reset.present);
    app::tray::dispatchWallpaperTrayCommand(
        app::tray::WallpaperTrayCommand::Next);
    DOCTEST_CHECK_EQ(trayRec().commands.size(), static_cast<std::size_t>(3));
    return true;
});

// 防：托盘点「继续」越过系统让路（语义决策：采用 service 现有 toggleManualPause
// 语义——只切手动位、不解除系统让路；让路解除且手动位已清才自动续播）。
REGISTER_TEST(wallpaper_tray_yield_pause_semantics,
              "wallpaper tray: 让路态下暂停/继续只切手动位，不解除系统让路", {
    test_support::TempDir dir("wallpaper_tray");
    wp::WallpaperService service;
    service.setSettingsPathForTest(
        (dir.path / "wallpaper" / "settings.json").string());
    service.setVideoScannerForTest([](const std::string&) {
        return wp::VideoScanResult{};
    });
    service.setImageScannerForTest([](const std::string&, int) {
        return wp::ImageScanResult{};
    });
    service.setPlayerForTest(std::make_unique<wp::NullPlayerBackend>());

    service.start();
    // 系统让路（等价 watcher 事件入口）：paused=true、manualPaused=false
    service.setSystemHoldForTest(wp::HoldReason::Fullscreen, true);
    const auto held = service.snapshot();
    DOCTEST_CHECK(held.paused);
    DOCTEST_CHECK_FALSE(held.manualPaused);

    // 让路态点「暂停壁纸」（文案口径仍由手动位驱动 → 此时字样是「暂停壁纸」）：
    // 落手动位，播放保持暂停
    service.toggleManualPause();
    auto snap = service.snapshot();
    DOCTEST_CHECK(snap.manualPaused);
    DOCTEST_CHECK(snap.paused);

    // 让路态点「继续壁纸」：只清手动位，播放仍被系统让路按住（决定性断言）
    service.toggleManualPause();
    snap = service.snapshot();
    DOCTEST_CHECK_FALSE(snap.manualPaused);
    DOCTEST_CHECK(snap.paused);

    // 让路解除：手动位已清 → 自动续播（paused 回 false）
    service.setSystemHoldForTest(wp::HoldReason::Fullscreen, false);
    snap = service.snapshot();
    DOCTEST_CHECK_FALSE(snap.paused);
    DOCTEST_CHECK_FALSE(snap.manualPaused);

    service.stop();
    return true;
});
