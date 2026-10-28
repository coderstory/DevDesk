// tests/test_modules/test_wallpaper_core.cpp
// 壁纸模块 P2 纯逻辑 + P4 服务骨架契约（需求 §5/§8/§9）。
//
// 每条用例注明防的事故；REGISTER_TEST 描述串必须含 ASCII "wallpaper"（--filter 只认 ASCII）。
// 跨文件前提：
//   - 本文件只测纯逻辑与服务骨架，不测 Win32/mpv/WorkerW（平台实现由探针覆盖）。
//   - 图片像素档过滤已落地：过滤逻辑经 ImageSizeProbe 注入假探针做跨平台确定性
//     测试；平台探针本体（WIC/stub）的 unknown 语义由探针与实现保证。
//   - 服务测试注入假扫描器/假玩家/假 watcher/临时设置路径，不读写用户真实数据根。
#include "doctest.h"
#include "test_support.h"

#include "module_wallpaper/core/arbiter.h"
#include "module_wallpaper/core/availability.h"
#include "module_wallpaper/core/folder_policy.h"
#include "module_wallpaper/core/image_library.h"
#include "module_wallpaper/core/presentation.h"
#include "module_wallpaper/core/rotation.h"
#include "module_wallpaper/core/settings.h"
#include "module_wallpaper/core/video_library.h"
#include "module_wallpaper/platform/player_backend.h"
#include "module_wallpaper/platform/yield_watchers.h"
#include "module_wallpaper/wallpaper_service.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace wp = module::wallpaper;
namespace fs = std::filesystem;

void writeTextFile(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::set<std::string> fileNames(const std::vector<std::string>& paths) {
    std::set<std::string> names;
    for (const std::string& path : paths) names.insert(fs::path(path).filename().string());
    return names;
}

// 轮询等待快照满足条件；超时返回最后一次快照，由调用方断言（helper 内不用 DOCTEST 宏，
// 宏会 return false 到 helper 而不是用例体）。
wp::WallpaperSnapshot waitForSnapshot(
    const wp::WallpaperService& service,
    const std::function<bool(const wp::WallpaperSnapshot&)>& predicate,
    int timeoutMs = 4000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        wp::WallpaperSnapshot snapshot = service.snapshot();
        if (predicate(snapshot)) return snapshot;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return service.snapshot();
}

bool hasPlayerCall(const wp::NullPlayerBackend& player, const std::string& op) {
    for (const wp::NullPlayerBackend::Call& call : player.calls()) {
        if (call.op == op) return true;
    }
    return false;
}

int playerCallCount(const wp::NullPlayerBackend& player, const std::string& op) {
    int count = 0;
    for (const wp::NullPlayerBackend::Call& call : player.calls()) {
        if (call.op == op) ++count;
    }
    return count;
}

bool hasPauseCall(const wp::NullPlayerBackend& player, bool paused) {
    for (const wp::NullPlayerBackend::Call& call : player.calls()) {
        if (call.op == "pause" && call.flag == paused) return true;
    }
    return false;
}

bool hasSeekCall(const wp::NullPlayerBackend& player, float seconds) {
    for (const wp::NullPlayerBackend::Call& call : player.calls()) {
        if (call.op == "seek" && std::fabs(call.number - static_cast<double>(seconds)) < 0.01) {
            return true;
        }
    }
    return false;
}

std::string loadedPath(const wp::NullPlayerBackend& player) {
    for (const wp::NullPlayerBackend::Call& call : player.calls()) {
        if (call.op == "load") return call.path;
    }
    return {};
}

// 假让路 watcher：事件通道手动点火（模拟 WTS 回调），轮询通道读原子开关
//（模拟 GetSystemPowerStatus / EnumWindows 的瞬时状态）。
class FakeYieldWatchers final : public wp::YieldWatchers {
public:
    void setHoldCallback(HoldCallback callback) override { hold_ = std::move(callback); }
    void setDisplayChangedCallback(DisplayChangedCallback callback) override {
        display_ = std::move(callback);
    }
    bool start(std::string* error) override {
        if (error != nullptr) *error = "fake";
        started_ = true;
        return true;
    }
    void stop() override { stopped_ = true; }
    bool pollOnBattery() override { return onBattery_.load(); }
    bool pollFullscreenOtherProcess() override { return fullscreen_.load(); }

    void fireHold(wp::HoldReason reason, bool active) {
        if (hold_) hold_(reason, active);
    }
    void fireDisplayChanged() {
        if (display_) display_();
    }
    void setBattery(bool on) { onBattery_.store(on); }
    void setFullscreen(bool on) { fullscreen_.store(on); }
    bool started() const { return started_; }
    bool stopped() const { return stopped_; }

private:
    HoldCallback hold_;
    DisplayChangedCallback display_;
    std::atomic<bool> onBattery_{false};
    std::atomic<bool> fullscreen_{false};
    bool started_ = false;
    bool stopped_ = false;
};

} // namespace

// 防：默认值与 §8 脱钩 / 原子写往返丢字段 / 损坏 JSON 崩溃 / 未知枚举把模块带进非法状态 /
// 数值越界直传播放器 / 空白路径被当成「已配置」。
REGISTER_TEST(wallpaper_core_settings, "wallpaper core: settings 默认值/往返/损坏/越界/空目录", {
    test_support::TempDir dir("wallpaper_settings");

    const wp::WallpaperSettings defaults = wp::defaultWallpaperSettings();
    DOCTEST_CHECK(defaults.kind == wp::WallpaperKind::Video);
    DOCTEST_CHECK_EQ(defaults.sourceFolder, std::string());
    DOCTEST_CHECK_EQ(defaults.imageFolderPath, std::string());
    DOCTEST_CHECK_EQ(defaults.rate, 1.0f);
    DOCTEST_CHECK_EQ(defaults.volume, 1.0f);
    DOCTEST_CHECK_FALSE(defaults.muted);
    DOCTEST_CHECK(defaults.mode == wp::PlayMode::LoopSingle);
    DOCTEST_CHECK_EQ(defaults.rotationIntervalSec, 300);
    DOCTEST_CHECK_FALSE(defaults.pauseOnBattery);
    DOCTEST_CHECK_EQ(defaults.imageMinPixels, 3686400);
    DOCTEST_CHECK(defaults.imageFit == wp::ImageFit::Fill);

    const fs::path settingsPath = dir.path / "nested" / "deep" / "settings.json";
    wp::WallpaperSettings settings = defaults;
    settings.kind = wp::WallpaperKind::Image;
    settings.sourceFolder = (dir.path / "videos").string();
    settings.imageFolderPath = (dir.path / "images").string();
    settings.rate = 1.25f;
    settings.volume = 0.4f;
    settings.muted = true;
    settings.mode = wp::PlayMode::Shuffle;
    settings.rotationIntervalSec = 900;
    settings.pauseOnBattery = true;
    settings.imageMinPixels = 8294400;
    settings.imageFit = wp::ImageFit::Tile;
    settings.lastPlayedPath = "D:/wallpapers/a.mp4";
    settings.lastPlayedPosition = 12.5;
    settings.lastImagePath = "D:/wallpapers/a.png";

    const core::Result<void> saved = wp::saveWallpaperSettings(settingsPath, settings);
    DOCTEST_CHECK(saved.isOk());
    DOCTEST_CHECK(fs::exists(settingsPath));

    const wp::WallpaperSettings loaded = wp::loadWallpaperSettings(settingsPath);
    DOCTEST_CHECK(loaded.kind == wp::WallpaperKind::Image);
    DOCTEST_CHECK_EQ(wp::folder_policy::normalizeFolderPath(loaded.sourceFolder),
                     wp::folder_policy::normalizeFolderPath(settings.sourceFolder));
    DOCTEST_CHECK_EQ(wp::folder_policy::normalizeFolderPath(loaded.imageFolderPath),
                     wp::folder_policy::normalizeFolderPath(settings.imageFolderPath));
    DOCTEST_CHECK_EQ(loaded.rate, 1.25f);
    DOCTEST_CHECK_EQ(loaded.volume, 0.4f);
    DOCTEST_CHECK(loaded.muted);
    DOCTEST_CHECK(loaded.mode == wp::PlayMode::Shuffle);
    DOCTEST_CHECK_EQ(loaded.rotationIntervalSec, 900);
    DOCTEST_CHECK(loaded.pauseOnBattery);
    DOCTEST_CHECK_EQ(loaded.imageMinPixels, 8294400);
    DOCTEST_CHECK(loaded.imageFit == wp::ImageFit::Tile);
    DOCTEST_CHECK_EQ(loaded.lastPlayedPath, settings.lastPlayedPath);
    DOCTEST_CHECK_EQ(loaded.lastPlayedPosition, 12.5);
    DOCTEST_CHECK_EQ(loaded.lastImagePath, settings.lastImagePath);

    // 损坏 JSON 回默认（不崩、不抛）。
    const fs::path corruptPath = dir.path / "corrupt.json";
    writeTextFile(corruptPath, "{not json");
    const wp::WallpaperSettings corrupt = wp::loadWallpaperSettings(corruptPath);
    DOCTEST_CHECK(corrupt.kind == wp::WallpaperKind::Video);
    DOCTEST_CHECK_EQ(corrupt.rotationIntervalSec, 300);

    // 未知枚举收敛 + 数值 clamp + 空白路径 = 未配置。
    const fs::path weirdPath = dir.path / "weird.json";
    writeTextFile(weirdPath,
                  "{\"wallpaperKind\":\"poster\",\"playMode\":\"nonsense\",\"imageFit\":\"wide\","
                  "\"rate\":9.0,\"volume\":-1.0,\"rotationInterval\":7,\"imageMinPixels\":0,"
                  "\"sourceFolder\":\"   \"}");
    const wp::WallpaperSettings weird = wp::loadWallpaperSettings(weirdPath);
    DOCTEST_CHECK(weird.kind == wp::WallpaperKind::Video);
    DOCTEST_CHECK(weird.mode == wp::PlayMode::LoopSingle);
    DOCTEST_CHECK(weird.imageFit == wp::ImageFit::Fill);
    DOCTEST_CHECK_EQ(weird.rate, 2.0f);
    DOCTEST_CHECK_EQ(weird.volume, 0.0f);
    DOCTEST_CHECK_EQ(weird.rotationIntervalSec, 300);
    DOCTEST_CHECK_EQ(weird.imageMinPixels, 3686400);
    DOCTEST_CHECK_FALSE(wp::folder_policy::isFolderConfigured(weird.sourceFolder));
    return true;
})

// 防：三模式推进错乱 / 洗牌一轮重复 / 暂停没冻结剩余 / refresh 把当前片打断 /
// 间隔允许值被绕过进到播放器。
REGISTER_TEST(wallpaper_core_rotation, "wallpaper core: rotation 三模式/洗牌/暂停冻结/refresh/吸附", {
    using Clock = wp::RotationClock;
    const Clock::time_point base = Clock::time_point(std::chrono::seconds(10000));

    // 列表循环：a→b→c→a。
    wp::RotationController list(7);
    list.setItems({"a", "b", "c"});
    list.setMode(wp::PlayMode::LoopList);
    DOCTEST_CHECK_EQ(list.start(base).value(), std::string("a"));
    DOCTEST_CHECK_EQ(list.advanceNow(base).value(), std::string("b"));
    DOCTEST_CHECK_EQ(list.advanceNow(base).value(), std::string("c"));
    DOCTEST_CHECK_EQ(list.advanceNow(base).value(), std::string("a"));

    // 单循环：到点不重新装载；用户「下一个」才换片。
    wp::RotationController single(1);
    single.setItems({"a", "b", "c"});
    single.setMode(wp::PlayMode::LoopSingle);
    DOCTEST_CHECK_EQ(single.start(base).value(), std::string("a"));
    DOCTEST_CHECK_FALSE(single.tick(base + std::chrono::seconds(300)).has_value());
    DOCTEST_CHECK_EQ(single.currentPath().value(), std::string("a"));
    DOCTEST_CHECK_EQ(single.advanceNow(base).value(), std::string("b"));
    DOCTEST_CHECK_EQ(single.secondsUntilNextRotation(base), -1);

    // 洗牌：一轮内不重复（3 条 = start + 2 次 advance 恰好覆盖全集）。
    wp::RotationController shuffle(42);
    shuffle.setItems({"a", "b", "c"});
    shuffle.setMode(wp::PlayMode::Shuffle);
    std::set<std::string> drawn;
    drawn.insert(shuffle.start(base).value());
    for (int index = 0; index < 2; ++index) {
        const auto next = shuffle.advanceNow(base);
        DOCTEST_REQUIRE(next.has_value());
        drawn.insert(*next);
    }
    DOCTEST_CHECK_EQ(drawn.size(), static_cast<std::size_t>(3));

    // 暂停冻结剩余、恢复续走。
    wp::RotationController paused(3);
    paused.setItems({"a", "b"});
    paused.setMode(wp::PlayMode::LoopList);
    DOCTEST_CHECK_EQ(paused.start(base).value(), std::string("a"));
    paused.pause(base + std::chrono::seconds(120));
    DOCTEST_CHECK_EQ(paused.secondsUntilNextRotation(base + std::chrono::seconds(1000)), 180);
    DOCTEST_CHECK(std::fabs(paused.progress(base + std::chrono::seconds(1000)) -
                            (static_cast<float>(120) / 300.0f)) < 0.001f);
    paused.resume(base + std::chrono::seconds(1000));
    DOCTEST_CHECK_EQ(paused.secondsUntilNextRotation(base + std::chrono::seconds(1000)), 180);
    DOCTEST_CHECK_FALSE(
        paused.tick(base + std::chrono::seconds(1179)).has_value());
    DOCTEST_CHECK_EQ(paused.tick(base + std::chrono::seconds(1180)).value(),
                     std::string("b"));

    // refresh 保留当前与剩余计时。
    wp::RotationController refresh(5);
    refresh.setItems({"a", "b", "c"});
    refresh.setMode(wp::PlayMode::LoopList);
    refresh.start(base);
    refresh.advanceNow(base);
    const int beforeRefresh = refresh.secondsUntilNextRotation(base + std::chrono::seconds(10));
    DOCTEST_CHECK(refresh.refreshItems({"x", "b", "y"}));
    DOCTEST_CHECK_EQ(refresh.currentPath().value(), std::string("b"));
    DOCTEST_CHECK_EQ(refresh.secondsUntilNextRotation(base + std::chrono::seconds(10)),
                     beforeRefresh);
    DOCTEST_CHECK_FALSE(refresh.refreshItems({"x", "y"}));
    DOCTEST_CHECK_FALSE(refresh.isRunning());

    // 间隔吸附：非法回默认，非档位就近吸附。
    DOCTEST_CHECK_EQ(wp::snapRotationIntervalSec(299), 300);
    DOCTEST_CHECK_EQ(wp::snapRotationIntervalSec(450), 300);
    DOCTEST_CHECK_EQ(wp::snapRotationIntervalSec(480), 600);
    DOCTEST_CHECK_EQ(wp::snapRotationIntervalSec(3300), 3600);
    DOCTEST_CHECK_EQ(wp::snapRotationIntervalSec(0), 300);
    DOCTEST_CHECK_EQ(wp::snapRotationIntervalSec(-5), 300);
    DOCTEST_CHECK_EQ(wp::snapRotationIntervalSec(7200), 7200);
    return true;
})

// 防：图片进混合池后破坏既有选择模型——列表循环跨类型按字典序轮转 / LoopSingle 到点
// 必须停在图片项（tick 不产生装载）、手动 next 才跨过图片 / 洗牌一轮不重复 /
// 空视频纯图片清单与普通清单同语义。
REGISTER_TEST(wallpaper_core_rotation_mixed_pool,
              "wallpaper core: rotation 混合池（视频+图片）三模式与 LoopSingle 图片行为", {
    using Clock = wp::RotationClock;
    const Clock::time_point base = Clock::time_point(std::chrono::seconds(20000));

    // 列表循环跨类型平等参与：a.mp4 → b.png → c.mp4 → a.mp4。
    wp::RotationController list(7);
    list.setItems({"a.mp4", "b.png", "c.mp4"});
    list.setMode(wp::PlayMode::LoopList);
    DOCTEST_CHECK_EQ(list.start(base).value(), std::string("a.mp4"));
    DOCTEST_CHECK_EQ(list.advanceNow(base).value(), std::string("b.png"));
    DOCTEST_CHECK_EQ(list.advanceNow(base).value(), std::string("c.mp4"));
    DOCTEST_CHECK_EQ(list.advanceNow(base).value(), std::string("a.mp4"));

    // LoopSingle 图片行为：首项是图片 → 到点不推进不装载；手动「下一个」才跨回视频。
    wp::RotationController single(1);
    single.setItems({"a.png", "b.mp4"});
    single.setMode(wp::PlayMode::LoopSingle);
    DOCTEST_CHECK_EQ(single.start(base).value(), std::string("a.png"));
    DOCTEST_CHECK_FALSE(single.tick(base + std::chrono::seconds(300)).has_value());
    DOCTEST_CHECK_EQ(single.currentPath().value(), std::string("a.png"));
    DOCTEST_CHECK_EQ(single.secondsUntilNextRotation(base + std::chrono::seconds(300)), -1);
    DOCTEST_CHECK_EQ(single.advanceNow(base + std::chrono::seconds(300)).value(),
                     std::string("b.mp4"));

    // 洗牌：混合池一轮内不重复。
    wp::RotationController shuffle(42);
    shuffle.setItems({"a.mp4", "b.png", "c.jpg", "d.mp4"});
    shuffle.setMode(wp::PlayMode::Shuffle);
    std::set<std::string> drawn;
    drawn.insert(shuffle.start(base).value());
    for (int index = 0; index < 3; ++index) {
        const auto next = shuffle.advanceNow(base);
        DOCTEST_REQUIRE(next.has_value());
        drawn.insert(*next);
    }
    DOCTEST_CHECK_EQ(drawn.size(), static_cast<std::size_t>(4));

    // 空视频纯图片清单：列表循环到点换图，与普通清单同语义。
    wp::RotationController imagesOnly(3);
    imagesOnly.setItems({"i1.jpg", "i2.png"});
    imagesOnly.setMode(wp::PlayMode::LoopList);
    DOCTEST_CHECK_EQ(imagesOnly.start(base).value(), std::string("i1.jpg"));
    DOCTEST_CHECK_EQ(imagesOnly.tick(base + std::chrono::seconds(300)).value(),
                     std::string("i2.png"));
    DOCTEST_CHECK_EQ(imagesOnly.secondsUntilNextRotation(base + std::chrono::seconds(300)), 300);
    return true;
})

// 防：多原因互相覆盖 / 锚点在暂停期间被重写 / 幂等 set 重复 seek /
// 手动暂停被算进系统原因（UI 会错误显示原因 chip）/ 电池开关关闭仍让路。
REGISTER_TEST(wallpaper_core_arbiter, "wallpaper core: arbiter 并集/锚点/幂等/手动暂停独立/电池门控", {
    wp::HoldArbiter arbiter;
    DOCTEST_CHECK(arbiter.shouldPlay());

    DOCTEST_CHECK(arbiter.setSystemReason(wp::HoldReason::Fullscreen, true, 12.0f));
    DOCTEST_CHECK(arbiter.setSystemReason(wp::HoldReason::ScreenLocked, true, 99.0f));
    const wp::HoldReasonSet both = wp::holdReasonBit(wp::HoldReason::Fullscreen) |
                                   wp::holdReasonBit(wp::HoldReason::ScreenLocked);
    DOCTEST_CHECK_EQ(arbiter.systemHolds(), both);
    DOCTEST_CHECK_FALSE(arbiter.shouldPlay());

    // 幂等：重复置位不重写锚点。
    DOCTEST_CHECK_FALSE(arbiter.setSystemReason(wp::HoldReason::Fullscreen, true, 42.0f));
    DOCTEST_CHECK_EQ(arbiter.anchorSec(), 12.0f);

    // 解除一个原因仍被另一个原因拦住，不产生 seek。
    DOCTEST_CHECK(arbiter.setSystemReason(wp::HoldReason::Fullscreen, false, 0.0f));
    float seekSeconds = -1.0f;
    DOCTEST_CHECK_FALSE(arbiter.takeResumeSeek(&seekSeconds));
    DOCTEST_CHECK_FALSE(arbiter.shouldPlay());

    // 全部解除才 seek 回锚点，且只消费一次。
    DOCTEST_CHECK(arbiter.setSystemReason(wp::HoldReason::ScreenLocked, false, 0.0f));
    DOCTEST_CHECK(arbiter.takeResumeSeek(&seekSeconds));
    DOCTEST_CHECK_EQ(seekSeconds, 12.0f);
    DOCTEST_CHECK_FALSE(arbiter.takeResumeSeek(&seekSeconds));

    // 手动暂停：参与暂停判定，但不产生系统原因。
    wp::HoldArbiter manual;
    DOCTEST_CHECK(manual.setManualPause(true, 5.0f));
    DOCTEST_CHECK(manual.held());
    DOCTEST_CHECK(manual.manuallyPaused());
    DOCTEST_CHECK_EQ(manual.systemHolds(), static_cast<wp::HoldReasonSet>(0));
    DOCTEST_CHECK_FALSE(manual.setManualPause(true, 8.0f));
    DOCTEST_CHECK_EQ(manual.anchorSec(), 5.0f);
    DOCTEST_CHECK(manual.setManualPause(false, 0.0f));
    DOCTEST_CHECK_FALSE(manual.held());
    DOCTEST_CHECK(manual.takeResumeSeek(&seekSeconds));
    DOCTEST_CHECK_EQ(seekSeconds, 5.0f);

    // 电池门控。
    wp::HoldArbiter battery;
    DOCTEST_CHECK_FALSE(battery.setSystemReason(wp::HoldReason::Battery, true, 3.0f));
    DOCTEST_CHECK_EQ(battery.systemHolds(), static_cast<wp::HoldReasonSet>(0));
    battery.setPauseOnBattery(true);
    DOCTEST_CHECK(battery.setSystemReason(wp::HoldReason::Battery, true, 3.0f));
    DOCTEST_CHECK(battery.held());
    battery.setPauseOnBattery(false);  // 关闭瞬间移除已存在的电池位
    DOCTEST_CHECK_FALSE(battery.held());
    DOCTEST_CHECK(battery.takeResumeSeek(&seekSeconds));
    DOCTEST_CHECK_EQ(seekSeconds, 3.0f);
    return true;
})

// 防：watcher 全接线后四路原因叠加/部分解除/解除顺序错乱——UI 原因集合与恢复
// seek 时机漂移（锚点必须只在第一路写入、seek 只在全解除时产生一次、电池门控
// 与其它位共存时关门只摘电池位）。
REGISTER_TEST(wallpaper_core_arbiter_matrix,
              "wallpaper core: arbiter 四路组合矩阵（叠加/部分解除/单次 seek/电池门控共存）", {
    float seek = -1.0f;

    // 四路同时压上：锚点只在第一路写入，重复事件不覆盖。
    wp::HoldArbiter four;
    four.setPauseOnBattery(true);
    DOCTEST_CHECK(four.setSystemReason(wp::HoldReason::Fullscreen, true, 10.0f));
    DOCTEST_CHECK(four.setSystemReason(wp::HoldReason::Battery, true, 20.0f));
    DOCTEST_CHECK(four.setSystemReason(wp::HoldReason::ScreenLocked, true, 30.0f));
    DOCTEST_CHECK(four.setSystemReason(wp::HoldReason::DisplayAsleep, true, 40.0f));
    const wp::HoldReasonSet allFour =
        wp::holdReasonBit(wp::HoldReason::Fullscreen) |
        wp::holdReasonBit(wp::HoldReason::ScreenLocked) |
        wp::holdReasonBit(wp::HoldReason::DisplayAsleep) |
        wp::holdReasonBit(wp::HoldReason::Battery);
    DOCTEST_CHECK_EQ(four.systemHolds(), allFour);
    DOCTEST_CHECK_FALSE(four.shouldPlay());
    DOCTEST_CHECK_EQ(four.anchorSec(), 10.0f);

    // 子集解除仍被其余原因拦住，不产生 seek。
    DOCTEST_CHECK(four.setSystemReason(wp::HoldReason::Fullscreen, false, 0.0f));
    DOCTEST_CHECK(four.setSystemReason(wp::HoldReason::Battery, false, 0.0f));
    DOCTEST_CHECK_FALSE(four.takeResumeSeek(&seek));
    DOCTEST_CHECK_FALSE(four.shouldPlay());
    DOCTEST_CHECK_EQ(four.systemHolds(),
                     wp::holdReasonBit(wp::HoldReason::ScreenLocked) |
                         wp::holdReasonBit(wp::HoldReason::DisplayAsleep));

    // 全解除才产生一次 seek，目标 = 首锚点。
    DOCTEST_CHECK(four.setSystemReason(wp::HoldReason::ScreenLocked, false, 0.0f));
    DOCTEST_CHECK_FALSE(four.shouldPlay());
    DOCTEST_CHECK(four.setSystemReason(wp::HoldReason::DisplayAsleep, false, 0.0f));
    DOCTEST_CHECK(four.shouldPlay());
    DOCTEST_CHECK(four.takeResumeSeek(&seek));
    DOCTEST_CHECK_EQ(seek, 10.0f);
    DOCTEST_CHECK_FALSE(four.takeResumeSeek(&seek));

    // 解除顺序不影响语义：先解除的不是锚点来源位时，锚点保持首次值。
    wp::HoldArbiter order;
    order.setPauseOnBattery(true);
    DOCTEST_CHECK(order.setSystemReason(wp::HoldReason::Battery, true, 5.0f));
    DOCTEST_CHECK(order.setSystemReason(wp::HoldReason::Fullscreen, true, 9.0f));
    DOCTEST_CHECK(order.setSystemReason(wp::HoldReason::Battery, false, 0.0f));
    DOCTEST_CHECK_FALSE(order.shouldPlay());  // 全屏仍拦
    DOCTEST_CHECK_EQ(order.anchorSec(), 5.0f);
    DOCTEST_CHECK(order.setSystemReason(wp::HoldReason::Fullscreen, false, 0.0f));
    DOCTEST_CHECK(order.takeResumeSeek(&seek));
    DOCTEST_CHECK_EQ(seek, 5.0f);

    // 电池门控与其它位共存：关门只摘电池位，不解除整体暂停、不产生 seek。
    wp::HoldArbiter coexist;
    coexist.setPauseOnBattery(true);
    DOCTEST_CHECK(coexist.setSystemReason(wp::HoldReason::Battery, true, 7.0f));
    DOCTEST_CHECK(coexist.setSystemReason(wp::HoldReason::ScreenLocked, true, 8.0f));
    coexist.setPauseOnBattery(false);
    DOCTEST_CHECK_EQ(coexist.systemHolds(),
                     wp::holdReasonBit(wp::HoldReason::ScreenLocked));
    DOCTEST_CHECK_FALSE(coexist.shouldPlay());
    DOCTEST_CHECK_FALSE(coexist.takeResumeSeek(&seek));
    DOCTEST_CHECK(coexist.setSystemReason(wp::HoldReason::ScreenLocked, false, 0.0f));
    DOCTEST_CHECK(coexist.takeResumeSeek(&seek));
    DOCTEST_CHECK_EQ(seek, 7.0f);
    return true;
})

// 防：四态判定把「未配置」和「目录不在」混淆 / 0 条被当成可播。
REGISTER_TEST(wallpaper_core_availability, "wallpaper core: availability 空路径/不存在/0 可播/可用", {
    using wp::Availability;
    using wp::LibraryScanStatus;
    DOCTEST_CHECK(wp::evaluateAvailability(false, LibraryScanStatus::Ok, 5) ==
                  Availability::FolderUnconfigured);
    DOCTEST_CHECK(wp::evaluateAvailability(false, LibraryScanStatus::Missing, 0) ==
                  Availability::FolderUnconfigured);
    DOCTEST_CHECK(wp::evaluateAvailability(true, LibraryScanStatus::Missing, 0) ==
                  Availability::FolderMissing);
    DOCTEST_CHECK(wp::evaluateAvailability(true, LibraryScanStatus::Unreadable, 0) ==
                  Availability::FolderMissing);
    DOCTEST_CHECK(wp::evaluateAvailability(true, LibraryScanStatus::Ok, 0) ==
                  Availability::NoPlayableVideos);
    DOCTEST_CHECK(wp::evaluateAvailability(true, LibraryScanStatus::Ok, 2) ==
                  Availability::Playing);
    DOCTEST_CHECK(wp::shouldShowWallpaper(Availability::Playing));
    DOCTEST_CHECK_FALSE(wp::shouldShowWallpaper(Availability::NoPlayableVideos));
    return true;
})

// 防：扩展名白名单漏大小写 / 递归漏子目录 / 隐藏文件混入 / 符号链接把目录带出根 /
// 入口上限失效导致超大目录卡死 UI。
REGISTER_TEST(wallpaper_core_video_library, "wallpaper core: video_library 扩展名/递归/隐藏/符号链接/上限", {
    test_support::TempDir dir("wallpaper_video_library");
    const fs::path root = dir.path / "library";
    fs::create_directories(root / "sub");
    fs::create_directories(root / ".hidden_dir");
    writeTextFile(root / "a.mp4", "x");
    writeTextFile(root / "b.MOV", "x");
    writeTextFile(root / "c.m4v", "x");
    writeTextFile(root / "d.mkv", "x");
    writeTextFile(root / "sub" / "e.mp4", "x");
    writeTextFile(root / ".hidden.mp4", "x");
    writeTextFile(root / ".hidden_dir" / "f.mp4", "x");

    std::error_code linkError;
    fs::create_symlink(root / "a.mp4", root / "link.mp4", linkError);
    const bool symlinkCreated = !linkError;

    const wp::VideoScanResult result = wp::scanVideoLibrary(root);
    DOCTEST_CHECK(result.status == wp::LibraryScanStatus::Ok);
    const std::set<std::string> names = fileNames(result.files);
    DOCTEST_CHECK(names.count("a.mp4") == 1);
    DOCTEST_CHECK(names.count("b.MOV") == 1);
    DOCTEST_CHECK(names.count("c.m4v") == 1);
    DOCTEST_CHECK(names.count("e.mp4") == 1);
    DOCTEST_CHECK(names.count("d.mkv") == 0);
    DOCTEST_CHECK(names.count(".hidden.mp4") == 0);
    DOCTEST_CHECK(names.count("f.mp4") == 0);
    DOCTEST_CHECK_EQ(result.files.size(), static_cast<std::size_t>(4));
    DOCTEST_CHECK_EQ(result.extensionRejected, static_cast<std::size_t>(1));
    if (symlinkCreated) {
        DOCTEST_CHECK(names.count("link.mp4") == 0);
        DOCTEST_CHECK(result.skippedSymlinks >= static_cast<std::size_t>(1));
    } else {
        std::cout << "[SKIP] wallpaper symlink case (create_symlink denied)\n";
    }

    // 入口上限：cap=3 时最多处理 3 个目录条目。
    const fs::path capped = dir.path / "capped";
    fs::create_directories(capped);
    for (int index = 0; index < 5; ++index) {
        writeTextFile(capped / ("v" + std::to_string(index) + ".mp4"), "x");
    }
    const wp::VideoScanResult cappedResult = wp::scanVideoLibrary(capped, 3);
    DOCTEST_CHECK(cappedResult.hitEntryCap);
    DOCTEST_CHECK_EQ(cappedResult.files.size(), static_cast<std::size_t>(3));

    DOCTEST_CHECK(wp::scanVideoLibrary(dir.path / "does-not-exist").status ==
                  wp::LibraryScanStatus::Missing);
    DOCTEST_CHECK(wp::scanVideoLibrary(root / "a.mp4").status ==
                  wp::LibraryScanStatus::Unreadable);
    return true;
})

// 防：图片扩展名白名单错 / 统计三格口径漂移 / 无探针注入时误过滤（core 直调
// 默认不过滤，生产路径才由 service 注入平台探针）。
REGISTER_TEST(wallpaper_core_image_library, "wallpaper core: image_library 扩展名/递归/统计/隐藏", {
    test_support::TempDir dir("wallpaper_image_library");
    const fs::path root = dir.path / "library";
    fs::create_directories(root / "sub");
    fs::create_directories(root / ".hidden_dir");
    writeTextFile(root / "a.jpg", "x");
    writeTextFile(root / "b.JPEG", "x");
    writeTextFile(root / "c.png", "x");
    writeTextFile(root / "d.webp", "x");
    writeTextFile(root / "e.heic", "x");
    writeTextFile(root / "f.gif", "x");
    writeTextFile(root / "sub" / "g.jpg", "x");
    writeTextFile(root / ".secret.png", "x");
    writeTextFile(root / ".hidden_dir" / "h.png", "x");

    const wp::ImageScanResult result = wp::scanImageLibrary(root, 8294400);
    DOCTEST_CHECK(result.status == wp::LibraryScanStatus::Ok);
    const std::set<std::string> names = fileNames(result.files);
    DOCTEST_CHECK(names.count("a.jpg") == 1);
    DOCTEST_CHECK(names.count("b.JPEG") == 1);
    DOCTEST_CHECK(names.count("c.png") == 1);
    DOCTEST_CHECK(names.count("d.webp") == 1);
    DOCTEST_CHECK(names.count("e.heic") == 1);
    DOCTEST_CHECK(names.count("g.jpg") == 1);
    DOCTEST_CHECK(names.count("f.gif") == 0);
    DOCTEST_CHECK(names.count(".secret.png") == 0);
    DOCTEST_CHECK(names.count("h.png") == 0);
    DOCTEST_CHECK_EQ(result.total, 6);
    DOCTEST_CHECK_EQ(result.passing, 6);  // 无探针注入：passing 恒等于 total（不过滤）
    DOCTEST_CHECK_EQ(result.filtered, 0);
    DOCTEST_CHECK_EQ(result.extensionRejected, static_cast<std::size_t>(1));
    return true;
})

// 防：像素档过滤逻辑漂移——unknown 语义（解码失败不过滤，宁滥勿缺）/ 档位线
// 恰好过线按 ≥ / 过滤只影响 passing 不动 total / files 只含 passing /
// minPixels<=0 关闭过滤。探针用夹具自带的 IHDR 解析（跨平台确定），等价于
// 平台层 WIC 的成功/失败两态。
REGISTER_TEST(wallpaper_core_image_filter,
              "wallpaper core: image_library 像素档过滤（假探针 unknown 不过滤/档位分流/计数三格）", {
    test_support::TempDir dir("wallpaper_image_filter");
    const fs::path root = dir.path / "library";
    fs::create_directories(root / "sub");
    test_support::writeMinimalPng(root / "small.png", 64, 64);       // 4,096 px
    test_support::writeMinimalPng(root / "big.png", 2560, 1440);     // 3,686,400 px（恰 2K 档）
    test_support::writeMinimalPng(root / "sub" / "tiny.jpg", 10, 10);
    test_support::writeMinimalPng(root / "opaque.webp", 1920, 1080); // 2,073,600 px（恰 1080P 档）
    test_support::writeTextFile(root / "broken.png", "not a png at all");

    const auto headerProbe = [](const std::string& path) -> wp::ProbedImageSize {
        std::ifstream in(path, std::ios::binary);
        if (!in) return {};
        unsigned char head[24] = {};
        in.read(reinterpret_cast<char*>(head), static_cast<std::streamsize>(sizeof(head)));
        if (in.gcount() < 24) return {};
        static const unsigned char kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        for (int i = 0; i < 8; ++i) {
            if (head[i] != kSig[i]) return {};  // 签名不符 = unknown（WIC 拒开同语义）
        }
        const auto be32 = [&head](int offset) {
            return static_cast<int>((static_cast<unsigned>(head[offset]) << 24) |
                                    (static_cast<unsigned>(head[offset + 1]) << 16) |
                                    (static_cast<unsigned>(head[offset + 2]) << 8) |
                                    static_cast<unsigned>(head[offset + 3]));
        };
        wp::ProbedImageSize size;
        size.known = true;
        size.width = be32(16);
        size.height = be32(20);
        return size;
    };

    // 2K 档：big 恰好过线（≥）；small/tiny/opaque 被滤；broken unknown 不过滤。
    const wp::ImageScanResult at2K =
        wp::scanImageLibrary(root, 3686400, 1000, {}, headerProbe);
    DOCTEST_CHECK(at2K.status == wp::LibraryScanStatus::Ok);
    const std::set<std::string> names2K = fileNames(at2K.files);
    DOCTEST_CHECK(names2K.count("big.png") == 1);
    DOCTEST_CHECK(names2K.count("broken.png") == 1);
    DOCTEST_CHECK(names2K.count("small.png") == 0);
    DOCTEST_CHECK(names2K.count("tiny.jpg") == 0);
    DOCTEST_CHECK(names2K.count("opaque.webp") == 0);
    DOCTEST_CHECK_EQ(at2K.total, 5);
    DOCTEST_CHECK_EQ(at2K.passing, 2);
    DOCTEST_CHECK_EQ(at2K.filtered, 3);

    // 1080P 档：opaque 恰好过线（≥），big 仍在池。
    const wp::ImageScanResult at1080 =
        wp::scanImageLibrary(root, 2073600, 1000, {}, headerProbe);
    DOCTEST_CHECK_EQ(at1080.passing, 3);
    DOCTEST_CHECK_EQ(at1080.filtered, 2);
    const std::set<std::string> names1080 = fileNames(at1080.files);
    DOCTEST_CHECK(names1080.count("opaque.webp") == 1);
    DOCTEST_CHECK(names1080.count("big.png") == 1);

    // minPixels <= 0：过滤关闭，探针不调用（passing == total）。
    const wp::ImageScanResult unfiltered =
        wp::scanImageLibrary(root, 0, 1000, {}, headerProbe);
    DOCTEST_CHECK_EQ(unfiltered.total, 5);
    DOCTEST_CHECK_EQ(unfiltered.passing, 5);
    DOCTEST_CHECK_EQ(unfiltered.filtered, 0);
    DOCTEST_CHECK_EQ(unfiltered.files.size(), static_cast<std::size_t>(5));
    return true;
})

// 防：UI 文案两套来源不一致 / 间隔 60 分钟显示成「60 分钟」/ 暂停仍显示倒计时 /
// 手动暂停错误显示原因 / 文案里漏进文件名。
REGISTER_TEST(wallpaper_core_presentation, "wallpaper core: presentation 标签/格式/倒计时/空态/无文件名", {
    using namespace module::wallpaper::presentation;

    DOCTEST_CHECK_EQ(playModeLabel(wp::PlayMode::LoopSingle, wp::WallpaperKind::Video),
                     std::string("单循环"));
    DOCTEST_CHECK_EQ(playModeLabel(wp::PlayMode::LoopList, wp::WallpaperKind::Video),
                     std::string("列表循环"));
    DOCTEST_CHECK_EQ(playModeLabel(wp::PlayMode::Shuffle, wp::WallpaperKind::Video),
                     std::string("随机"));
    DOCTEST_CHECK_EQ(playModeLabel(wp::PlayMode::LoopSingle, wp::WallpaperKind::Image),
                     std::string("单张不变"));
    DOCTEST_CHECK_EQ(playModeLabel(wp::PlayMode::LoopList, wp::WallpaperKind::Image),
                     std::string("顺序轮播"));
    DOCTEST_CHECK_EQ(playModeLabel(wp::PlayMode::Shuffle, wp::WallpaperKind::Image),
                     std::string("随机轮播"));

    DOCTEST_CHECK_EQ(rotationIntervalLabel(300), std::string("5 分钟"));
    DOCTEST_CHECK_EQ(rotationIntervalLabel(600), std::string("10 分钟"));
    DOCTEST_CHECK_EQ(rotationIntervalLabel(3600), std::string("1 小时"));
    DOCTEST_CHECK_EQ(rotationIntervalLabel(7200), std::string("2 小时"));
    DOCTEST_CHECK_EQ(rotationIntervalLabel(299), std::string("5 分钟"));

    DOCTEST_CHECK_EQ(rateLabel(1.0f), std::string("1.00×"));
    DOCTEST_CHECK_EQ(rateLabel(0.1f), std::string("0.50×"));
    DOCTEST_CHECK_EQ(rateLabel(3.0f), std::string("2.00×"));
    DOCTEST_CHECK_EQ(volumeLabel(0.42f), std::string("42%"));
    DOCTEST_CHECK_EQ(volumeLabel(1.2f), std::string("100%"));

    DOCTEST_CHECK_EQ(countdownLabel(wp::WallpaperKind::Video, wp::PlayMode::LoopList, false, 272),
                     std::string("距下次换片 04:32"));
    DOCTEST_CHECK_EQ(countdownLabel(wp::WallpaperKind::Image, wp::PlayMode::Shuffle, false, 8),
                     std::string("距下次换图 00:08"));
    DOCTEST_CHECK_EQ(countdownLabel(wp::WallpaperKind::Video, wp::PlayMode::LoopSingle, false, -1),
                     std::string("不轮换"));
    DOCTEST_CHECK_EQ(countdownLabel(wp::WallpaperKind::Video, wp::PlayMode::LoopList, true, 180),
                     std::string("等待恢复"));

    // 空态三变体关键句。
    const auto unconfigured = emptyStateCopy(wp::Availability::FolderUnconfigured,
                                             wp::WallpaperKind::Video);
    DOCTEST_REQUIRE(unconfigured.has_value());
    DOCTEST_CHECK_EQ(unconfigured->title, std::string("还没有设置壁纸文件夹"));
    DOCTEST_CHECK_EQ(unconfigured->primaryAction, std::string("选择文件夹…"));
    DOCTEST_CHECK_EQ(unconfigured->secondaryAction, std::string("重新扫描"));
    const auto missing = emptyStateCopy(wp::Availability::FolderMissing, wp::WallpaperKind::Image);
    DOCTEST_REQUIRE(missing.has_value());
    DOCTEST_CHECK_EQ(missing->title, std::string("壁纸文件夹找不到了"));
    DOCTEST_CHECK_EQ(missing->primaryAction, std::string("重新选择…"));
    DOCTEST_CHECK_EQ(missing->secondaryAction, std::string("重新扫描"));
    const auto emptyVideo = emptyStateCopy(wp::Availability::NoPlayableVideos,
                                           wp::WallpaperKind::Video);
    DOCTEST_REQUIRE(emptyVideo.has_value());
    DOCTEST_CHECK_EQ(emptyVideo->title, std::string("这个文件夹里没有能播的视频"));
    DOCTEST_CHECK_EQ(emptyVideo->primaryAction, std::string("更换文件夹"));
    DOCTEST_CHECK_EQ(emptyVideo->secondaryAction, std::string("重新扫描"));
    DOCTEST_CHECK(emptyVideo->reason.find("转码") == std::string::npos);
    const auto emptyImage = emptyStateCopy(wp::Availability::NoPlayableVideos,
                                           wp::WallpaperKind::Image);
    DOCTEST_REQUIRE(emptyImage.has_value());
    DOCTEST_CHECK_EQ(emptyImage->title, std::string("没有符合条件的图片"));
    DOCTEST_CHECK_EQ(emptyImage->primaryAction, std::string("更换文件夹"));
    // 档位过滤已落地：空态如实说明档位口径（F1 反转——旧「后续版本」口径回潮即红）。
    DOCTEST_CHECK(emptyImage->reason.find("低于分辨率档位") != std::string::npos);
    DOCTEST_CHECK(emptyImage->reason.find("后续版本") == std::string::npos);
    // 未配置的图片变体标题必须按来源分派（不能复用视频文案）。
    const auto unconfiguredImage = emptyStateCopy(wp::Availability::FolderUnconfigured,
                                                  wp::WallpaperKind::Image);
    DOCTEST_REQUIRE(unconfiguredImage.has_value());
    DOCTEST_CHECK_EQ(unconfiguredImage->title, std::string("还没有设置图片文件夹"));
    DOCTEST_CHECK_FALSE(emptyStateCopy(wp::Availability::Playing, wp::WallpaperKind::Video)
                            .has_value());

    // 状态标题/副题与原因拼接。
    wp::presentation::StatusInput playing;
    playing.kind = wp::WallpaperKind::Video;
    playing.availability = wp::Availability::Playing;
    playing.mode = wp::PlayMode::LoopList;
    playing.itemCount = 12;
    playing.rotationIntervalSec = 600;
    DOCTEST_CHECK_EQ(statusTitle(playing), std::string("正在播放 · 列表循环"));
    DOCTEST_CHECK_EQ(statusSubtitle(playing), std::string("12 个视频 · 每 10 分钟换一个"));
    playing.kind = wp::WallpaperKind::Image;
    playing.mode = wp::PlayMode::LoopSingle;
    playing.itemCount = 3;
    DOCTEST_CHECK_EQ(statusTitle(playing), std::string("正在显示 · 单张不变"));
    DOCTEST_CHECK_EQ(statusSubtitle(playing), std::string("3 张图片 · 单张不变"));

    wp::presentation::StatusInput manualPaused = playing;
    manualPaused.paused = true;
    manualPaused.manualPaused = true;
    DOCTEST_CHECK_EQ(statusTitle(manualPaused), std::string("已暂停"));
    DOCTEST_CHECK_EQ(statusSubtitle(manualPaused), std::string("点『继续』恢复播放。"));

    wp::presentation::StatusInput held = playing;
    held.paused = true;
    held.systemHolds = wp::holdReasonBit(wp::HoldReason::Fullscreen) |
                       wp::holdReasonBit(wp::HoldReason::ScreenLocked);
    DOCTEST_CHECK_EQ(statusTitle(held),
                     std::string("已暂停 · 检测到全屏/最大化窗口、屏幕已锁定"));
    DOCTEST_CHECK_EQ(statusSubtitle(held),
                     std::string("条件解除后自动续播，不会从头开始。"));

    // 手动暂停位混进 systemHolds 时不产生原因文案（防 UI 错误 chip）。
    DOCTEST_CHECK_EQ(holdReasonsLabel(wp::holdReasonBit(wp::HoldReason::ManualPause)),
                     std::string());

    DOCTEST_CHECK_EQ(libraryCountLabel(wp::WallpaperKind::Video, 2), std::string("2 个视频"));
    DOCTEST_CHECK_EQ(libraryCountLabel(wp::WallpaperKind::Image, 3), std::string("3 张可用图片"));

    // 文案不含文件名（本模块所有文案函数都不接收路径；这里锁住产出形状）。
    const std::string all = statusTitle(playing) + statusSubtitle(playing) +
                            countdownLabel(wp::WallpaperKind::Image, wp::PlayMode::Shuffle,
                                           false, 60) +
                            unconfigured->reason;
    DOCTEST_CHECK(all.find(".mp4") == std::string::npos);
    DOCTEST_CHECK(all.find(".png") == std::string::npos);
    return true;
})

// 防：服务 start 后扫描结果没进快照 / 选目录不持久化 / rescan 丢设置 /
// stop 不收线程 / 手动暂停与系统让路在快照里混为一谈 / 来源切换沿用旧清单。
REGISTER_TEST(wallpaper_core_service, "wallpaper core: service start/扫描/选目录持久化/暂停/stop", {
    test_support::TempDir dir("wallpaper_service");
    const fs::path settingsPath = dir.path / "wallpaper" / "settings.json";
    const fs::path videoRoot = dir.path / "videos";
    const fs::path imageRoot = dir.path / "images";
    fs::create_directories(videoRoot);
    fs::create_directories(imageRoot);

    auto player = std::make_unique<wp::NullPlayerBackend>();
    wp::NullPlayerBackend* playerRaw = player.get();

    wp::WallpaperService service;
    service.setSettingsPathForTest(settingsPath.string());
    service.setVideoScannerForTest([](const std::string& folder) {
        wp::VideoScanResult result;
        result.status = wp::LibraryScanStatus::Ok;
        result.files = {(fs::path(folder) / "a.mp4").make_preferred().string(),
                        (fs::path(folder) / "b.mp4").make_preferred().string()};
        return result;
    });
    // 图片段只喂图片来源：Video 来源现在是混合池（视频+图片），本用例的视频段
    // 断言针对纯视频目录，混合池语义由 wallpaper_core_service_mixed_pool 覆盖。
    service.setImageScannerForTest([&imageRoot](const std::string& folder, int minPixels) {
        (void)minPixels;
        wp::ImageScanResult result;
        if (wp::folder_policy::normalizeFolderPath(folder) !=
            wp::folder_policy::normalizeFolderPath(imageRoot.string())) {
            return result;  // 视频目录无图片：混合池只含视频
        }
        result.status = wp::LibraryScanStatus::Ok;
        result.files = {(fs::path(folder) / "a.jpg").make_preferred().string(),
                        (fs::path(folder) / "b.jpg").make_preferred().string(),
                        (fs::path(folder) / "c.png").make_preferred().string()};
        result.total = 3;
        result.passing = 3;
        result.filtered = 0;
        return result;
    });
    service.setPlayerForTest(std::move(player));

    service.start();
    std::string error;
    DOCTEST_REQUIRE_MESSAGE(service.selectFolder(videoRoot.string(), &error),
                            "selectFolder 失败: " + error);

    wp::WallpaperSnapshot snapshot = waitForSnapshot(service, [](const wp::WallpaperSnapshot& s) {
        return !s.scanning && s.availability == wp::Availability::Playing;
    });
    DOCTEST_CHECK(snapshot.availability == wp::Availability::Playing);
    DOCTEST_CHECK_EQ(snapshot.videoCount, 2);
    DOCTEST_CHECK_EQ(snapshot.kind, wp::WallpaperKind::Video);
    DOCTEST_CHECK_EQ(wp::folder_policy::normalizeFolderPath(snapshot.folderPath),
                     wp::folder_policy::normalizeFolderPath(videoRoot.string()));
    DOCTEST_CHECK_EQ(snapshot.modeLabel, std::string("单循环"));
    DOCTEST_CHECK_EQ(snapshot.statusTitle, std::string("正在播放 · 单循环"));
    DOCTEST_CHECK_EQ(snapshot.statusSubtitle, std::string("2 个视频 · 单循环播放"));
    DOCTEST_CHECK_FALSE(snapshot.paused);
    DOCTEST_CHECK(hasPlayerCall(*playerRaw, "load"));
    DOCTEST_CHECK_EQ(fs::path(loadedPath(*playerRaw)).filename().string(), std::string("a.mp4"));

    // 选目录已持久化；默认其余设置不变。
    wp::WallpaperSettings stored = wp::loadWallpaperSettings(settingsPath);
    DOCTEST_CHECK_EQ(wp::folder_policy::normalizeFolderPath(wp::folderForKind(
                         stored, wp::WallpaperKind::Video)),
                     wp::folder_policy::normalizeFolderPath(videoRoot.string()));
    DOCTEST_CHECK(stored.mode == wp::PlayMode::LoopSingle);

    // rescan 不丢设置，且扫描完成状态回到 playing。
    service.setMode(wp::PlayMode::LoopList);
    service.setRate(1.25f);
    service.rescan();
    DOCTEST_CHECK(service.snapshot().scanning);
    snapshot = waitForSnapshot(service, [](const wp::WallpaperSnapshot& s) {
        return !s.scanning && s.availability == wp::Availability::Playing;
    });
    DOCTEST_CHECK_EQ(snapshot.videoCount, 2);
    DOCTEST_CHECK_EQ(snapshot.modeLabel, std::string("列表循环"));
    DOCTEST_CHECK_EQ(snapshot.statusSubtitle,
                     std::string("2 个视频 · 每 5 分钟换一个"));
    stored = wp::loadWallpaperSettings(settingsPath);
    DOCTEST_CHECK(stored.mode == wp::PlayMode::LoopList);
    DOCTEST_CHECK_EQ(stored.rate, 1.25f);

    // 系统让路与手动暂停分离：系统原因进 systemHolds，手动暂停不进。
    service.setSystemHoldForTest(wp::HoldReason::Fullscreen, true);
    snapshot = service.snapshot();
    DOCTEST_CHECK(snapshot.paused);
    DOCTEST_CHECK_FALSE(snapshot.manualPaused);
    DOCTEST_CHECK_EQ(snapshot.systemHolds, wp::holdReasonBit(wp::HoldReason::Fullscreen));
    DOCTEST_CHECK(snapshot.statusTitle.find("已暂停 · 检测到全屏") != std::string::npos);
    DOCTEST_CHECK_EQ(snapshot.countdownLabel, std::string("等待恢复"));
    service.setSystemHoldForTest(wp::HoldReason::Fullscreen, false);
    DOCTEST_CHECK_FALSE(service.snapshot().paused);

    service.toggleManualPause();
    snapshot = service.snapshot();
    DOCTEST_CHECK(snapshot.paused);
    DOCTEST_CHECK(snapshot.manualPaused);
    DOCTEST_CHECK_EQ(snapshot.systemHolds, static_cast<wp::HoldReasonSet>(0));
    DOCTEST_CHECK_EQ(snapshot.statusTitle, std::string("已暂停"));
    service.toggleManualPause();
    snapshot = service.snapshot();
    DOCTEST_CHECK_FALSE(snapshot.paused);
    DOCTEST_CHECK_FALSE(snapshot.manualPaused);

    // 来源切换 = 新一轮：图片统计进入快照，视频计数保留上一次结果。
    service.setMode(wp::PlayMode::LoopSingle);  // 单张不变便于断言图片模式文案
    service.setKind(wp::WallpaperKind::Image);
    DOCTEST_CHECK(service.snapshot().kind == wp::WallpaperKind::Image);
    DOCTEST_REQUIRE_MESSAGE(service.selectFolder(imageRoot.string(), &error),
                            "图片 selectFolder 失败: " + error);
    snapshot = waitForSnapshot(service, [](const wp::WallpaperSnapshot& s) {
        return !s.scanning && s.availability == wp::Availability::Playing;
    });
    DOCTEST_CHECK(snapshot.availability == wp::Availability::Playing);
    DOCTEST_CHECK_EQ(snapshot.imageTotal, 3);
    DOCTEST_CHECK_EQ(snapshot.imagePassing, 3);
    DOCTEST_CHECK_EQ(snapshot.imageFiltered, 0);
    DOCTEST_CHECK_EQ(snapshot.modeLabel, std::string("单张不变"));
    DOCTEST_CHECK_EQ(snapshot.statusTitle, std::string("正在显示 · 单张不变"));
    DOCTEST_CHECK_EQ(snapshot.statusSubtitle, std::string("3 张图片 · 单张不变"));
    stored = wp::loadWallpaperSettings(settingsPath);
    DOCTEST_CHECK(stored.kind == wp::WallpaperKind::Image);
    DOCTEST_CHECK_EQ(wp::folder_policy::normalizeFolderPath(stored.imageFolderPath),
                     wp::folder_policy::normalizeFolderPath(imageRoot.string()));

    // stop 有界返回 + 幂等。
    const auto stopStarted = std::chrono::steady_clock::now();
    service.stop();
    const auto stopElapsed = std::chrono::steady_clock::now() - stopStarted;
    DOCTEST_CHECK(stopElapsed < std::chrono::seconds(3));
    // 防（审计 F-7）：停止链必须真的到达播放器——删掉 stopPlaybackLocked 里的
    // player_->stop() 时本断言红。
    DOCTEST_CHECK(hasPlayerCall(*playerRaw, "stop"));
    service.stop();
    return true;
})

// 防（审计 F-2/F-7）：空态/目录缺失后旧轮换清单仍被 next() 装载——P3 接 mpv 后会播放已删除
// 或上一来源的文件；播放态下 next 必须仍能正常换片。
REGISTER_TEST(wallpaper_core_service_next_guard,
              "wallpaper core: service next 守卫（播放推进 / 空扫描与缺目录不产生 load）", {
    test_support::TempDir dir("wallpaper_service_next");
    const fs::path settingsPath = dir.path / "wallpaper" / "settings.json";
    const fs::path videoRoot = dir.path / "videos";
    fs::create_directories(videoRoot);

    // 0 = 两个文件；1 = 空目录；2 = 目录缺失。扫描器只读原子量（不在 start 后替换 scanner）。
    std::atomic<int> scanMode{0};

    auto player = std::make_unique<wp::NullPlayerBackend>();
    wp::NullPlayerBackend* playerRaw = player.get();

    wp::WallpaperService service;
    service.setSettingsPathForTest(settingsPath.string());
    service.setVideoScannerForTest([&scanMode](const std::string& folder) {
        wp::VideoScanResult result;
        const int mode = scanMode.load();
        if (mode == 2) {
            result.status = wp::LibraryScanStatus::Missing;
            return result;
        }
        result.status = wp::LibraryScanStatus::Ok;
        if (mode == 0) {
            result.files = {(fs::path(folder) / "a.mp4").string(),
                            (fs::path(folder) / "b.mp4").string()};
        }
        return result;
    });
    service.setPlayerForTest(std::move(player));

    service.start();
    std::string error;
    DOCTEST_REQUIRE_MESSAGE(service.selectFolder(videoRoot.string(), &error),
                            "selectFolder 失败: " + error);
    wp::WallpaperSnapshot snapshot = waitForSnapshot(
        service, [](const wp::WallpaperSnapshot& s) {
            return !s.scanning && s.availability == wp::Availability::Playing;
        });
    DOCTEST_CHECK_EQ(snapshot.availability, wp::Availability::Playing);

    // 播放态：next 正常推进并产生一次 load。
    playerRaw->clearCalls();
    service.next();
    DOCTEST_CHECK_EQ(playerCallCount(*playerRaw, "load"), 1);
    DOCTEST_CHECK_EQ(fs::path(loadedPath(*playerRaw)).filename().string(), std::string("b.mp4"));

    // 空扫描：清单必须清空，next 不能再产生 load。
    scanMode.store(1);
    service.rescan();
    snapshot = waitForSnapshot(service, [](const wp::WallpaperSnapshot& s) {
        return !s.scanning && s.availability == wp::Availability::NoPlayableVideos;
    });
    DOCTEST_CHECK(snapshot.availability == wp::Availability::NoPlayableVideos);
    DOCTEST_CHECK_EQ(snapshot.videoCount, 0);
    playerRaw->clearCalls();
    service.next();
    DOCTEST_CHECK_EQ(playerCallCount(*playerRaw, "load"), 0);

    // 目录缺失：同样不得装载旧条目。
    scanMode.store(2);
    service.rescan();
    snapshot = waitForSnapshot(service, [](const wp::WallpaperSnapshot& s) {
        return !s.scanning && s.availability == wp::Availability::FolderMissing;
    });
    DOCTEST_CHECK(snapshot.availability == wp::Availability::FolderMissing);
    playerRaw->clearCalls();
    service.next();
    DOCTEST_CHECK_EQ(playerCallCount(*playerRaw, "load"), 0);

    service.stop();
    return true;
})

// 防：图片不进 Video 轮换池 / 混合池首条与副题按池构成错乱 / 空视频纯图片目录被判
// 0 可播 / 图片项仍走视频 position/seek 语义（锚点会对静图 seek）。定时到点自动换片
// 的端到端实证在 wallpaper_image_rotation_probe（本用例只锁纯逻辑与服务接线）。
REGISTER_TEST(wallpaper_core_service_mixed_pool,
              "wallpaper core: service 混合池/纯图片目录可轮换/图片态 position=0 与 seek no-op", {
    test_support::TempDir dir("wallpaper_service_mixed");
    const fs::path settingsPath = dir.path / "wallpaper" / "settings.json";
    const fs::path videoRoot = dir.path / "videos";
    fs::create_directories(videoRoot);

    // 0 = 视频+图片混合（2 视频 + 1 图）；1 = 纯图片（视频扫描空 + 2 图）。
    std::atomic<int> mixMode{0};

    auto player = std::make_unique<wp::NullPlayerBackend>();
    wp::NullPlayerBackend* playerRaw = player.get();

    wp::WallpaperService service;
    service.setSettingsPathForTest(settingsPath.string());
    service.setVideoScannerForTest([&mixMode](const std::string& folder) {
        wp::VideoScanResult result;
        result.status = wp::LibraryScanStatus::Ok;
        if (mixMode.load() == 0) {
            result.files = {(fs::path(folder) / "a.mp4").string(),
                            (fs::path(folder) / "b.mp4").string()};
        }
        return result;
    });
    service.setImageScannerForTest([&mixMode](const std::string& folder, int minPixels) {
        (void)minPixels;
        wp::ImageScanResult result;
        result.status = wp::LibraryScanStatus::Ok;
        result.filtered = 0;
        if (mixMode.load() == 0) {
            result.files = {(fs::path(folder) / "c.png").string()};
            result.total = 1;
            result.passing = 1;
        } else {
            result.files = {(fs::path(folder) / "i1.jpg").string(),
                            (fs::path(folder) / "i2.png").string()};
            result.total = 2;
            result.passing = 2;
        }
        return result;
    });
    service.setPlayerForTest(std::move(player));

    service.start();
    std::string error;
    DOCTEST_REQUIRE_MESSAGE(service.selectFolder(videoRoot.string(), &error),
                            "selectFolder 失败: " + error);

    // ── 混合池：视频与图片平等入池，副题按池构成分派 ──
    wp::WallpaperSnapshot snapshot = waitForSnapshot(service, [](const wp::WallpaperSnapshot& s) {
        return !s.scanning && s.availability == wp::Availability::Playing;
    });
    DOCTEST_CHECK_EQ(snapshot.videoCount, 2);
    DOCTEST_CHECK_EQ(snapshot.poolCount, 3);
    DOCTEST_CHECK_EQ(snapshot.poolImageCount, 1);
    DOCTEST_CHECK_EQ(snapshot.statusTitle, std::string("正在播放 · 单循环"));
    DOCTEST_CHECK_EQ(snapshot.statusSubtitle,
                     std::string("2 个视频 · 1 张图片 · 单循环播放"));
    DOCTEST_CHECK_FALSE(snapshot.currentItemImage);
    DOCTEST_CHECK_EQ(fs::path(loadedPath(*playerRaw)).filename().string(), std::string("a.mp4"));

    // 列表循环手动推进跨类型：b.mp4 → c.png；图片项置位 + 标题点名静图。
    service.setMode(wp::PlayMode::LoopList);
    playerRaw->clearCalls();
    service.next();
    DOCTEST_CHECK_EQ(fs::path(loadedPath(*playerRaw)).filename().string(), std::string("b.mp4"));
    service.next();
    snapshot = service.snapshot();
    std::string lastLoad;
    for (const wp::NullPlayerBackend::Call& call : playerRaw->calls()) {
        if (call.op == "load") lastLoad = call.path;  // loadedPath 取首条，这里要最新一条
    }
    DOCTEST_CHECK_EQ(fs::path(lastLoad).filename().string(), std::string("c.png"));
    DOCTEST_CHECK(snapshot.currentItemImage);
    DOCTEST_CHECK_EQ(snapshot.statusTitle, std::string("正在显示 · 静态图片"));
    DOCTEST_CHECK_EQ(snapshot.statusSubtitle,
                     std::string("2 个视频 · 1 张图片 · 每 5 分钟换一个"));

    // 图片态 position 恒 0：手动暂停写不下视频锚点，解禁不产生 seek（静图安全 no-op）。
    playerRaw->setPositionForTest(37.5f);
    service.toggleManualPause();
    DOCTEST_CHECK(service.snapshot().paused);
    service.toggleManualPause();
    DOCTEST_CHECK_FALSE(service.snapshot().paused);
    DOCTEST_CHECK_EQ(playerCallCount(*playerRaw, "seek"), 0);
    DOCTEST_CHECK(hasPauseCall(*playerRaw, true));
    DOCTEST_CHECK(hasPauseCall(*playerRaw, false));

    // ── 空视频纯图片目录：仍 Playing 且可轮换 ──
    mixMode.store(1);
    service.rescan();
    snapshot = waitForSnapshot(service, [](const wp::WallpaperSnapshot& s) {
        return !s.scanning && s.availability == wp::Availability::Playing;
    });
    DOCTEST_CHECK_EQ(snapshot.availability, wp::Availability::Playing);
    DOCTEST_CHECK_EQ(snapshot.videoCount, 0);
    DOCTEST_CHECK_EQ(snapshot.poolCount, 2);
    DOCTEST_CHECK_EQ(snapshot.poolImageCount, 2);
    std::string pureImageLoad;
    for (const wp::NullPlayerBackend::Call& call : playerRaw->calls()) {
        if (call.op == "load") pureImageLoad = call.path;  // 重扫后最新一次装载
    }
    DOCTEST_CHECK_EQ(fs::path(pureImageLoad).filename().string(), std::string("i1.jpg"));
    DOCTEST_CHECK(snapshot.currentItemImage);
    DOCTEST_CHECK_EQ(snapshot.statusSubtitle,
                     std::string("2 张图片 · 每 5 分钟换一张"));

    service.stop();
    DOCTEST_CHECK(hasPlayerCall(*playerRaw, "stop"));
    return true;
})

// 防（审计 F-7/§9.3）：让路解除时没有 seek 回锚点（P3 会表现为解禁后从头播）。
REGISTER_TEST(wallpaper_core_service_hold_seek,
              "wallpaper core: service 让路释放 seek 回锚点并恢复播放", {
    test_support::TempDir dir("wallpaper_service_hold");
    const fs::path settingsPath = dir.path / "wallpaper" / "settings.json";
    const fs::path videoRoot = dir.path / "videos";
    fs::create_directories(videoRoot);

    auto player = std::make_unique<wp::NullPlayerBackend>();
    wp::NullPlayerBackend* playerRaw = player.get();

    wp::WallpaperService service;
    service.setSettingsPathForTest(settingsPath.string());
    service.setVideoScannerForTest([&videoRoot](const std::string&) {
        wp::VideoScanResult result;
        result.status = wp::LibraryScanStatus::Ok;
        result.files = {(videoRoot / "a.mp4").string(), (videoRoot / "b.mp4").string()};
        return result;
    });
    service.setPlayerForTest(std::move(player));

    service.start();
    std::string error;
    DOCTEST_REQUIRE_MESSAGE(service.selectFolder(videoRoot.string(), &error),
                            "selectFolder 失败: " + error);
    waitForSnapshot(service, [](const wp::WallpaperSnapshot& s) {
        return !s.scanning && s.availability == wp::Availability::Playing;
    });

    playerRaw->clearCalls();
    playerRaw->setPositionForTest(37.5f);
    service.setSystemHoldForTest(wp::HoldReason::Fullscreen, true);
    DOCTEST_CHECK(service.snapshot().paused);
    DOCTEST_CHECK(hasPauseCall(*playerRaw, true));

    service.setSystemHoldForTest(wp::HoldReason::Fullscreen, false);
    DOCTEST_CHECK_FALSE(service.snapshot().paused);
    DOCTEST_CHECK(hasSeekCall(*playerRaw, 37.5f));
    DOCTEST_CHECK(hasPauseCall(*playerRaw, false));
    DOCTEST_CHECK_EQ(playerCallCount(*playerRaw, "load"), 0);  // 释放不重新装载

    service.stop();
    return true;
})

// 防：watcher 全接线的服务侧裁决路径——事件/轮询两通道的状态变化必须落成
// pause/seek 命令序列与快照原因集合；电池豁免开关即时生效且不受持续上报影响；
// stop 必须真的停掉 watcher；测试装配下换屏回调安全 no-op（无宿主不报错）。
REGISTER_TEST(wallpaper_core_service_yield_watchers,
              "wallpaper core: service 让路 watcher 接线（锁屏事件/电池与全屏轮询/电池豁免/换屏 no-op）", {
    test_support::TempDir dir("wallpaper_service_yield");
    const fs::path settingsPath = dir.path / "wallpaper" / "settings.json";
    const fs::path videoRoot = dir.path / "videos";
    fs::create_directories(videoRoot);

    auto player = std::make_unique<wp::NullPlayerBackend>();
    wp::NullPlayerBackend* playerRaw = player.get();
    auto watchers = std::make_unique<FakeYieldWatchers>();
    FakeYieldWatchers* watchersRaw = watchers.get();

    wp::WallpaperService service;
    service.setSettingsPathForTest(settingsPath.string());
    service.setVideoScannerForTest([&videoRoot](const std::string&) {
        wp::VideoScanResult result;
        result.status = wp::LibraryScanStatus::Ok;
        result.files = {(videoRoot / "a.mp4").string()};
        return result;
    });
    service.setYieldWatchersForTest(std::move(watchers));
    service.setPlayerForTest(std::move(player));

    service.start();
    std::string error;
    DOCTEST_REQUIRE_MESSAGE(service.selectFolder(videoRoot.string(), &error),
                            "selectFolder 失败: " + error);
    waitForSnapshot(service, [](const wp::WallpaperSnapshot& s) {
        return !s.scanning && s.availability == wp::Availability::Playing;
    });
    DOCTEST_CHECK(watchersRaw->started());

    // ① 锁屏事件通道：fire → 暂停 + 原因集合；解锁 → 恢复 + seek 回锚点。
    playerRaw->clearCalls();
    playerRaw->setPositionForTest(11.0f);
    watchersRaw->fireHold(wp::HoldReason::ScreenLocked, true);
    wp::WallpaperSnapshot snapshot = waitForSnapshot(
        service, [](const wp::WallpaperSnapshot& s) { return s.paused; });
    DOCTEST_CHECK(snapshot.paused);
    DOCTEST_CHECK(snapshot.systemHolds == wp::holdReasonBit(wp::HoldReason::ScreenLocked));
    DOCTEST_CHECK(hasPauseCall(*playerRaw, true));
    watchersRaw->fireHold(wp::HoldReason::ScreenLocked, false);
    snapshot = waitForSnapshot(service,
                               [](const wp::WallpaperSnapshot& s) { return !s.paused; });
    DOCTEST_CHECK_FALSE(snapshot.paused);
    DOCTEST_CHECK_EQ(snapshot.systemHolds, static_cast<wp::HoldReasonSet>(0));
    DOCTEST_CHECK(hasSeekCall(*playerRaw, 11.0f));
    DOCTEST_CHECK(hasPauseCall(*playerRaw, false));

    // ② 全屏轮询通道：置位 → 下一拍（≤250ms+余量）暂停；复位 → 恢复。
    playerRaw->clearCalls();
    playerRaw->setPositionForTest(21.0f);
    watchersRaw->setFullscreen(true);
    snapshot = waitForSnapshot(service, [](const wp::WallpaperSnapshot& s) {
        return s.paused &&
               s.systemHolds == wp::holdReasonBit(wp::HoldReason::Fullscreen);
    });
    DOCTEST_CHECK(snapshot.paused);
    watchersRaw->setFullscreen(false);
    snapshot = waitForSnapshot(service,
                               [](const wp::WallpaperSnapshot& s) { return !s.paused; });
    DOCTEST_CHECK_FALSE(snapshot.paused);
    DOCTEST_CHECK(hasSeekCall(*playerRaw, 21.0f));

    // ③ 电池轮询通道 + 豁免开关：默认门控关闭时电池上报不让路；开门控后电池位
    // 生效 → 暂停；关开关 → 立即恢复（watcher 仍上报电池也不让路）；重开开关 →
    // 下一拍电池位回来（契约：开门不主动置位，等下一帧轮询）。
    watchersRaw->setBattery(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    DOCTEST_CHECK_FALSE(service.snapshot().paused);  // 门控关：持续上报也不让路
    service.setPauseOnBattery(true);
    wp::WallpaperSnapshot batterySnap = waitForSnapshot(service, [](const wp::WallpaperSnapshot& s) {
        return s.paused && s.systemHolds == wp::holdReasonBit(wp::HoldReason::Battery);
    });
    DOCTEST_CHECK(batterySnap.paused);
    DOCTEST_CHECK(batterySnap.pauseOnBattery);
    service.setPauseOnBattery(false);
    batterySnap = waitForSnapshot(
        service, [](const wp::WallpaperSnapshot& s) { return !s.paused; });
    DOCTEST_CHECK_FALSE(batterySnap.paused);
    DOCTEST_CHECK_EQ(batterySnap.systemHolds, static_cast<wp::HoldReasonSet>(0));
    service.setPauseOnBattery(true);
    batterySnap = waitForSnapshot(service, [](const wp::WallpaperSnapshot& s) {
        return s.paused && s.systemHolds == wp::holdReasonBit(wp::HoldReason::Battery);
    });
    DOCTEST_CHECK(batterySnap.paused);
    watchersRaw->setBattery(false);
    batterySnap = waitForSnapshot(
        service, [](const wp::WallpaperSnapshot& s) { return !s.paused; });
    DOCTEST_CHECK_FALSE(batterySnap.paused);

    // ④ 换屏回调：测试装配无宿主 → 安全 no-op（不报错、不影响播放态与暂停态）。
    const std::string errorBefore = service.snapshot().playbackError;
    watchersRaw->fireDisplayChanged();
    snapshot = service.snapshot();
    DOCTEST_CHECK_EQ(snapshot.playbackError, errorBefore);
    DOCTEST_CHECK_FALSE(snapshot.paused);

    // ⑤ stop 停 watcher：fake 记录 stopped，重复 stop 幂等。
    service.stop();
    service.stop();
    DOCTEST_CHECK(watchersRaw->stopped());
    return true;
})