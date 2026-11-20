// tests/test_modules/test_wallpaper_delete.cpp
// need356 B3 删除链路：outcome→toast 文案纯映射、服务「先切后删 / 单项进空态 / 失败回滚
// 不切下一个」、确认弹窗桥开态与取消零副作用。
//
// 跨文件前提：
//   - 删除动作全走注入假 trash mover（不碰真实废纸篓；平台真实现归 B1 probe）。
//   - 服务测试注入假扫描器/假玩家/临时设置路径，不建桌面宿主、不 spawn mpv。
//   - 桥是进程级 static：用例内 RAII 复位（fail-fast 早退也回厂）。
//   - REGISTER_TEST 描述串必须含 ASCII "wallpaper"（--filter 只认 ASCII）。
#include "doctest.h"
#include "test_support.h"

#include "module_wallpaper/platform/player_backend.h"
#include "module_wallpaper/wallpaper_service.h"
#include "platform/file_trash.h"
#include "wallpaper_delete_bridge.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace wp = module::wallpaper;
namespace fs = std::filesystem;
namespace trash = platform::file_trash;

void writeFile(const fs::path& path, std::size_t bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << std::string(bytes, 'x');
}

wp::WallpaperSnapshot waitForPlaying(wp::WallpaperService& service, int timeoutMs = 4000) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        wp::WallpaperSnapshot snapshot = service.snapshot();
        if (!snapshot.scanning && snapshot.availability == wp::Availability::Playing) {
            return snapshot;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return service.snapshot();
}

std::string lastLoadedPath(const wp::NullPlayerBackend& player) {
    std::string last;
    for (const wp::NullPlayerBackend::Call& call : player.calls()) {
        if (call.op == "load") last = call.path;
    }
    return last;
}

// 记录 show/hide 次数的假宿主：只验「池==1 删除失败回滚有没有把宿主重新显出来」。
// 真宿主是平台窗口（Windows WorkerW / macOS 桌面层），单测不碰。
class RecordingHost final : public wp::WallpaperHost {
public:
    bool start(std::string*) override {
        ++starts_;
        return true;
    }
    void stop() override {}
    bool show(std::string*) override {
        ++shows_;
        return true;
    }
    void hide() override { ++hides_; }
    wp::HostHandle handle() const override { return hostHandle_; }
    void rebuild(std::string*) override {}

    int showCount() const { return shows_; }
    int hideCount() const { return hides_; }

private:
    int starts_ = 0;
    int shows_ = 0;
    int hides_ = 0;
    // 非空 videoChild 才能在 ensurePlayerStartedLocked 里拉起播放器（与真宿主同判据）
    wp::HostHandle hostHandle_{reinterpret_cast<void*>(0x1), nullptr, 1920, 1080};
};

// 桥 static 状态复位守卫（与 test_wallpaper_tray 的 TrayBridgeGuard 同纪律）。
struct DeleteBridgeGuard {
    DeleteBridgeGuard() { app::wallpaper_delete::resetForTest(); }
    ~DeleteBridgeGuard() { app::wallpaper_delete::resetForTest(); }
    DeleteBridgeGuard(const DeleteBridgeGuard&) = delete;
    DeleteBridgeGuard& operator=(const DeleteBridgeGuard&) = delete;
};

// 弹窗桥的 recording 桩。
struct DialogRecording {
    int targetCalls = 0;
    int confirmCalls = 0;
    int showCalls = 0;
    app::wallpaper_delete::DeleteTarget target{};
};
DialogRecording& dialogRec() {
    static DialogRecording rec;
    return rec;
}
app::wallpaper_delete::DeleteTarget recordTarget() {
    ++dialogRec().targetCalls;
    return dialogRec().target;
}
void recordConfirm() { ++dialogRec().confirmCalls; }
void recordShow() { ++dialogRec().showCalls; }

}  // namespace

// 防：六档 outcome 的 toast 文案错位（失败被说成成功 / 档位串味）。
REGISTER_TEST(wallpaper_delete_outcome_maps_to_toast_text,
              "wallpaper delete: outcome 六档 → toast 文案纯映射", {
    // 成功：标题「已移到废纸篓」+ 文件名
    const auto ok = app::wallpaper_delete::toastTextForResult(true, "a.mp4", 0);
    DOCTEST_CHECK_EQ(ok.title, std::string("已移到废纸篓"));
    DOCTEST_CHECK_EQ(ok.message, std::string("a.mp4"));

    const auto notFound = app::wallpaper_delete::toastTextForResult(
        false, "a.mp4", static_cast<int>(trash::TrashFailure::NotFound));
    DOCTEST_CHECK_EQ(notFound.title, std::string("删除失败"));
    DOCTEST_CHECK_EQ(notFound.message,
                     std::string("文件已被移动或占用，文件仍在文件夹里"));

    const auto denied = app::wallpaper_delete::toastTextForResult(
        false, "a.mp4", static_cast<int>(trash::TrashFailure::PermissionDenied));
    DOCTEST_CHECK_EQ(denied.title, std::string("删除失败"));
    DOCTEST_CHECK_EQ(denied.message,
                     std::string("没有权限删除这个文件，文件仍在文件夹里"));

    const auto unavailable = app::wallpaper_delete::toastTextForResult(
        false, "a.mp4", static_cast<int>(trash::TrashFailure::TrashUnavailable));
    DOCTEST_CHECK_EQ(unavailable.title, std::string("删除失败"));
    DOCTEST_CHECK_EQ(unavailable.message,
                     std::string("系统废纸篓当前不可用，文件仍在文件夹里"));

    const auto busy = app::wallpaper_delete::toastTextForResult(
        false, "a.mp4", static_cast<int>(trash::TrashFailure::Busy));
    DOCTEST_CHECK_EQ(busy.title, std::string("删除失败"));
    DOCTEST_CHECK_EQ(busy.message, std::string("文件正被占用，文件仍在文件夹里"));
    return true;
});

// 防：删除后结果没进桥队列（worker 直投 toast 违反线程契约 → 队列是唯一通道）。
REGISTER_TEST(wallpaper_delete_result_queue_drains_once,
              "wallpaper delete: postResult → takeResults 单次取空", {
    DeleteBridgeGuard guard;
    DOCTEST_CHECK(app::wallpaper_delete::takeResults().empty());
    app::wallpaper_delete::postResult(true, "a.mp4", 0);
    app::wallpaper_delete::postResult(
        false, "b.mp4", static_cast<int>(trash::TrashFailure::Busy));
    const auto drained = app::wallpaper_delete::takeResults();
    DOCTEST_REQUIRE_EQ(drained.size(), static_cast<std::size_t>(2));
    DOCTEST_CHECK_EQ(drained[0].first, std::string("已移到废纸篓"));
    DOCTEST_CHECK_EQ(drained[0].second, std::string("a.mp4"));
    DOCTEST_CHECK_EQ(drained[1].first, std::string("删除失败"));
    DOCTEST_CHECK_EQ(drained[1].second, std::string("文件正被占用，文件仍在文件夹里"));
    // 取走即清
    DOCTEST_CHECK(app::wallpaper_delete::takeResults().empty());
    return true;
});

// 防：弹窗在目标不可删时仍打开 / 取消钮产生副作用 / 未装配时崩溃。
REGISTER_TEST(wallpaper_delete_dialog_open_cancel_zero_side_effect,
              "wallpaper delete: 弹窗 开态判据 + 取消零副作用", {
    DeleteBridgeGuard guard;
    dialogRec() = DialogRecording{};
    // 未装配：openConfirm 安全 no-op
    app::wallpaper_delete::openConfirm();
    DOCTEST_CHECK_FALSE(app::wallpaper_delete::dialogOpenForTest());

    app::wallpaper_delete::setShowHandler(&recordShow);
    app::wallpaper_delete::setHandlers(&recordTarget, &recordConfirm);

    // 目标不可用 → 不打开、不唤起
    dialogRec().target = {};
    app::wallpaper_delete::openConfirm();
    DOCTEST_CHECK_FALSE(app::wallpaper_delete::dialogOpenForTest());
    DOCTEST_CHECK_EQ(dialogRec().showCalls, 0);

    // 目标可用 → 先唤起主窗再置开态
    dialogRec().target = {true, "a.mp4", false};
    app::wallpaper_delete::openConfirm();
    DOCTEST_CHECK(app::wallpaper_delete::dialogOpenForTest());
    DOCTEST_CHECK_EQ(dialogRec().showCalls, 1);
    DOCTEST_CHECK_EQ(dialogRec().confirmCalls, 0);

    // 取消路径 = 只关弹窗，不触发确认、不入结果队列
    app::wallpaper_delete::resetForTest();
    DOCTEST_CHECK_FALSE(app::wallpaper_delete::dialogOpenForTest());
    DOCTEST_CHECK(app::wallpaper_delete::takeResults().empty());
    DOCTEST_CHECK_EQ(dialogRec().confirmCalls, 0);
    return true;
});

// 池 ≥2：先切到下一项、再移废纸篓；成功写墓碑 + 重扫。
REGISTER_TEST(wallpaper_delete_service_switches_then_trashes,
              "wallpaper delete: 池≥2 先切走再移废纸篓（成功）", {
    test_support::TempDir dir("wallpaper_delete_two");
    const fs::path root = dir.path / "videos";
    fs::create_directories(root);
    const std::string a = (root / "a.mp4").make_preferred().string();
    const std::string b = (root / "b.mp4").make_preferred().string();
    writeFile(a, 128);
    writeFile(b, 256);

    auto player = std::make_unique<wp::NullPlayerBackend>();
    wp::NullPlayerBackend* playerRaw = player.get();

    // 扫描器：a 永远可枚举（删除由假 mover 模拟，目录里文件不真删——重扫仍见两项，
    // 便于断言「切走的是 a」。真实计数刷新归 F-05 真机）。
    wp::WallpaperService service;
    service.setSettingsPathForTest((dir.path / "wallpaper" / "settings.json").string());
    service.setVideoScannerForTest([a, b](const std::string&) {
        wp::VideoScanResult result;
        result.status = wp::LibraryScanStatus::Ok;
        result.files = {a, b};
        return result;
    });
    service.setImageScannerForTest([](const std::string&, int) {
        return wp::ImageScanResult{};
    });
    service.setPlayerForTest(std::move(player));

    std::atomic<int> trashCalls{0};
    std::string trashedPath;
    service.setTrashMoverForTest([&](const std::string& path) {
        ++trashCalls;
        trashedPath = path;
        return trash::TrashOutcome{};
    });

    service.start();
    std::string error;
    DOCTEST_REQUIRE_MESSAGE(service.selectFolder(root.string(), &error),
                            "selectFolder 失败: " + error);
    wp::WallpaperSnapshot snapshot = waitForPlaying(service);
    DOCTEST_REQUIRE(snapshot.availability == wp::Availability::Playing);
    DOCTEST_CHECK_EQ(snapshot.poolCount, 2);
    DOCTEST_CHECK_EQ(snapshot.currentItemFileName, std::string("a.mp4"));

    playerRaw->clearCalls();
    const auto outcome = service.deleteCurrentItem();
    DOCTEST_CHECK(outcome == wp::WallpaperService::DeleteCurrentOutcome::Deleted);
    // 先切走：播放器装了 b
    DOCTEST_CHECK_EQ(fs::path(lastLoadedPath(*playerRaw)).filename().string(),
                     std::string("b.mp4"));
    // 再移废纸篓：mover 收到的是原当前项路径
    DOCTEST_CHECK_EQ(trashCalls.load(), 1);
    DOCTEST_CHECK_EQ(fs::path(trashedPath).filename().string(), std::string("a.mp4"));
    // 成功后仍可播（池未空）
    DOCTEST_CHECK(service.snapshot().availability == wp::Availability::Playing);
    service.stop();
    return true;
});

// 池 ==1：删除前先进空态（停播 + NoPlayableVideos），不残留单条清单。
REGISTER_TEST(wallpaper_delete_service_single_item_enters_empty,
              "wallpaper delete: 池==1 删除前先进空态", {
    test_support::TempDir dir("wallpaper_delete_one");
    const fs::path root = dir.path / "videos";
    fs::create_directories(root);
    const std::string a = (root / "only.mp4").make_preferred().string();
    writeFile(a, 64);

    auto player = std::make_unique<wp::NullPlayerBackend>();
    wp::NullPlayerBackend* playerRaw = player.get();

    wp::WallpaperService service;
    service.setSettingsPathForTest((dir.path / "wallpaper" / "settings.json").string());
    service.setVideoScannerForTest([a](const std::string&) {
        wp::VideoScanResult result;
        result.status = wp::LibraryScanStatus::Ok;
        result.files = {a};
        return result;
    });
    service.setImageScannerForTest([](const std::string&, int) {
        return wp::ImageScanResult{};
    });
    service.setPlayerForTest(std::move(player));
    service.setTrashMoverForTest([](const std::string&) { return trash::TrashOutcome{}; });

    service.start();
    std::string error;
    DOCTEST_REQUIRE_MESSAGE(service.selectFolder(root.string(), &error),
                            "selectFolder 失败: " + error);
    wp::WallpaperSnapshot snapshot = waitForPlaying(service);
    DOCTEST_REQUIRE(snapshot.availability == wp::Availability::Playing);
    DOCTEST_CHECK_EQ(snapshot.poolCount, 1);

    playerRaw->clearCalls();
    const auto outcome = service.deleteCurrentItem();
    DOCTEST_CHECK(outcome == wp::WallpaperService::DeleteCurrentOutcome::Deleted);
    snapshot = service.snapshot();
    DOCTEST_CHECK(snapshot.availability == wp::Availability::NoPlayableVideos);
    DOCTEST_CHECK_EQ(snapshot.poolCount, 0);
    // 停播已发出
    bool stopped = false;
    for (const auto& call : playerRaw->calls()) {
        if (call.op == "stop") stopped = true;
    }
    DOCTEST_CHECK(stopped);
    service.stop();
    return true;
});

// 池==1 且移废纸篓失败（F-04 的极端面）：P2 已停播并藏宿主，回滚必须把**宿主重新
// show** 出来——否则状态卡写「正在播放」而桌面一片空（用户可见的假回滚）。
// 注入假宿主后断言 show 被调用过一次以上。
REGISTER_TEST(wallpaper_delete_single_item_failure_reshows_host,
              "wallpaper delete: 池==1 删除失败回滚要把宿主重新显出来", {
    test_support::TempDir dir("wallpaper_delete_one_fail");
    const fs::path root = dir.path / "videos";
    fs::create_directories(root);
    const std::string a = (root / "only.mp4").make_preferred().string();
    writeFile(a, 64);

    auto player = std::make_unique<wp::NullPlayerBackend>();
    auto host = std::make_unique<RecordingHost>();
    RecordingHost* hostRaw = host.get();

    wp::WallpaperService service;
    service.setSettingsPathForTest((dir.path / "wallpaper" / "settings.json").string());
    service.setVideoScannerForTest([a](const std::string&) {
        wp::VideoScanResult result;
        result.status = wp::LibraryScanStatus::Ok;
        result.files = {a};
        return result;
    });
    service.setImageScannerForTest([](const std::string&, int) {
        return wp::ImageScanResult{};
    });
    service.setPlayerForTest(std::move(player));
    service.setHostForTest(std::move(host));
    service.setTrashMoverForTest([](const std::string&) {
        return trash::TrashOutcome{trash::TrashFailure::PermissionDenied};
    });

    service.start();
    std::string error;
    DOCTEST_REQUIRE_MESSAGE(service.selectFolder(root.string(), &error),
                            "selectFolder 失败: " + error);
    DOCTEST_REQUIRE(waitForPlaying(service).availability == wp::Availability::Playing);

    const auto outcome = service.deleteCurrentItem();
    DOCTEST_CHECK(outcome == wp::WallpaperService::DeleteCurrentOutcome::PermissionDenied);
    // 回滚后仍在播、池仍有 1 项
    const wp::WallpaperSnapshot snapshot = service.snapshot();
    DOCTEST_CHECK(snapshot.availability == wp::Availability::Playing);
    DOCTEST_CHECK_EQ(snapshot.poolCount, 1);
    // 决定性断言：hide 之后必须再有 show，否则桌面空着而状态卡说在播
    DOCTEST_CHECK(hostRaw->hideCount() >= 1);
    DOCTEST_CHECK(hostRaw->showCount() >= 1);
    service.stop();
    return true;
});

// 失败回滚（F-04）：不切下一个——播放器重新装回原项、池读数不变。
REGISTER_TEST(wallpaper_delete_service_failure_rolls_back,
              "wallpaper delete: 移废纸篓失败回滚原项且不切下一个", {    test_support::TempDir dir("wallpaper_delete_fail");
    const fs::path root = dir.path / "videos";
    fs::create_directories(root);
    const std::string a = (root / "a.mp4").make_preferred().string();
    const std::string b = (root / "b.mp4").make_preferred().string();
    writeFile(a, 128);
    writeFile(b, 256);

    auto player = std::make_unique<wp::NullPlayerBackend>();
    wp::NullPlayerBackend* playerRaw = player.get();

    wp::WallpaperService service;
    service.setSettingsPathForTest((dir.path / "wallpaper" / "settings.json").string());
    service.setVideoScannerForTest([a, b](const std::string&) {
        wp::VideoScanResult result;
        result.status = wp::LibraryScanStatus::Ok;
        result.files = {a, b};
        return result;
    });
    service.setImageScannerForTest([](const std::string&, int) {
        return wp::ImageScanResult{};
    });
    service.setPlayerForTest(std::move(player));
    service.setTrashMoverForTest([](const std::string&) {
        return trash::TrashOutcome{trash::TrashFailure::Busy};
    });

    service.start();
    std::string error;
    DOCTEST_REQUIRE_MESSAGE(service.selectFolder(root.string(), &error),
                            "selectFolder 失败: " + error);
    wp::WallpaperSnapshot snapshot = waitForPlaying(service);
    DOCTEST_REQUIRE(snapshot.availability == wp::Availability::Playing);
    DOCTEST_CHECK_EQ(snapshot.currentItemFileName, std::string("a.mp4"));

    playerRaw->clearCalls();
    const auto outcome = service.deleteCurrentItem();
    DOCTEST_CHECK(outcome == wp::WallpaperService::DeleteCurrentOutcome::Busy);
    // 回滚：最后装载的仍是 a（不是 b）
    DOCTEST_CHECK_EQ(fs::path(lastLoadedPath(*playerRaw)).filename().string(),
                     std::string("a.mp4"));
    // 池未变、仍在播
    snapshot = service.snapshot();
    DOCTEST_CHECK(snapshot.availability == wp::Availability::Playing);
    DOCTEST_CHECK_EQ(snapshot.poolCount, 2);
    DOCTEST_CHECK_EQ(snapshot.currentItemFileName, std::string("a.mp4"));
    service.stop();
    return true;
});

// F-05：删除成功后立即重扫（不靠下一轮轮询）→ 池计数与副题文案随目录实况刷新。
REGISTER_TEST(wallpaper_delete_service_rescans_after_success,
              "wallpaper delete: 成功删除后重扫，池计数递减", {
    test_support::TempDir dir("wallpaper_delete_rescan");
    const fs::path root = dir.path / "videos";
    fs::create_directories(root);
    const std::string a = (root / "a.mp4").make_preferred().string();
    const std::string b = (root / "b.mp4").make_preferred().string();
    writeFile(a, 128);
    writeFile(b, 256);

    // 目录实况由本用例控制：删除成功后把 a 从扫描结果里摘掉（等价于文件真的走了）。
    std::atomic<bool> hideA{false};
    wp::WallpaperService service;
    service.setSettingsPathForTest((dir.path / "wallpaper" / "settings.json").string());
    service.setVideoScannerForTest([a, b, &hideA](const std::string&) {
        wp::VideoScanResult result;
        result.status = wp::LibraryScanStatus::Ok;
        result.files = {a, b};
        if (hideA.load()) result.files = {b};
        return result;
    });
    service.setImageScannerForTest([](const std::string&, int) {
        return wp::ImageScanResult{};
    });
    service.setPlayerForTest(std::make_unique<wp::NullPlayerBackend>());
    service.setTrashMoverForTest([&hideA](const std::string&) {
        hideA.store(true);  // mover 真删 = 目录里不再有该文件
        return trash::TrashOutcome{};
    });

    service.start();
    std::string error;
    DOCTEST_REQUIRE_MESSAGE(service.selectFolder(root.string(), &error),
                            "selectFolder 失败: " + error);
    wp::WallpaperSnapshot snapshot = waitForPlaying(service);
    DOCTEST_REQUIRE(snapshot.availability == wp::Availability::Playing);
    DOCTEST_CHECK_EQ(snapshot.videoCount, 2);
    DOCTEST_CHECK_EQ(snapshot.poolCount, 2);

    const auto outcome = service.deleteCurrentItem();
    DOCTEST_CHECK(outcome == wp::WallpaperService::DeleteCurrentOutcome::Deleted);
    // 重扫是异步的：等计数落到 1
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(4000);
    while (std::chrono::steady_clock::now() < deadline) {
        snapshot = service.snapshot();
        if (!snapshot.scanning && snapshot.poolCount == 1) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    DOCTEST_CHECK_EQ(snapshot.videoCount, 1);
    DOCTEST_CHECK_EQ(snapshot.poolCount, 1);
    service.stop();
    return true;
});

// 无当前项：静默 NoCurrent（不投失败 toast、不动播放）。
REGISTER_TEST(wallpaper_delete_service_no_current_is_silent,
              "wallpaper delete: 无当前项 = 静默 NoCurrent", {
    test_support::TempDir dir("wallpaper_delete_none");
    wp::WallpaperService service;
    service.setSettingsPathForTest((dir.path / "wallpaper" / "settings.json").string());
    service.setVideoScannerForTest([](const std::string&) {
        return wp::VideoScanResult{};
    });
    service.setImageScannerForTest([](const std::string&, int) {
        return wp::ImageScanResult{};
    });
    service.setPlayerForTest(std::make_unique<wp::NullPlayerBackend>());
    std::atomic<int> trashCalls{0};
    service.setTrashMoverForTest([&](const std::string&) {
        ++trashCalls;
        return trash::TrashOutcome{};
    });
    service.start();
    const auto outcome = service.deleteCurrentItem();
    DOCTEST_CHECK(outcome == wp::WallpaperService::DeleteCurrentOutcome::NoCurrent);
    DOCTEST_CHECK_EQ(trashCalls.load(), 0);
    service.stop();
    return true;
});

// 异步入口：requestDeleteCurrent 后台跑完把结果交结果出口（worker 不直接投 toast）。
REGISTER_TEST(wallpaper_delete_service_request_posts_result,
              "wallpaper delete: requestDeleteCurrent 异步跑完经结果出口回填", {
    test_support::TempDir dir("wallpaper_delete_async");
    const fs::path root = dir.path / "videos";
    fs::create_directories(root);
    const std::string a = (root / "only.mp4").make_preferred().string();
    writeFile(a, 32);

    wp::WallpaperService service;
    service.setSettingsPathForTest((dir.path / "wallpaper" / "settings.json").string());
    service.setVideoScannerForTest([a](const std::string&) {
        wp::VideoScanResult result;
        result.status = wp::LibraryScanStatus::Ok;
        result.files = {a};
        return result;
    });
    service.setImageScannerForTest([](const std::string&, int) {
        return wp::ImageScanResult{};
    });
    service.setPlayerForTest(std::make_unique<wp::NullPlayerBackend>());
    service.setTrashMoverForTest([](const std::string&) { return trash::TrashOutcome{}; });

    std::atomic<int> handlerCalls{0};
    std::atomic<bool> handlerOk{false};
    std::string handlerName;
    service.setDeleteResultHandler([&](bool ok, const std::string& name, int) {
        handlerOk.store(ok);
        handlerName = name;
        ++handlerCalls;
    });

    service.start();
    std::string error;
    DOCTEST_REQUIRE_MESSAGE(service.selectFolder(root.string(), &error),
                            "selectFolder 失败: " + error);
    DOCTEST_REQUIRE(waitForPlaying(service).availability == wp::Availability::Playing);

    service.requestDeleteCurrent();
    DOCTEST_REQUIRE(service.waitDeleteIdleForTest(4000));
    service.stop();  // join 掉删除线程
    DOCTEST_CHECK_EQ(handlerCalls.load(), 1);
    DOCTEST_CHECK(handlerOk.load());
    DOCTEST_CHECK_EQ(handlerName, std::string("only.mp4"));
    return true;
});

// P0（bug358）：服务已停后晚到的删除确认（托盘退出 → 确认弹窗迟来）不得造删除线程，
// 析构里 stop() 也不得留下 joinable thread 触发 std::terminate。
REGISTER_TEST(wallpaper_delete_request_after_stop_does_not_crash,
              "wallpaper delete: stop 后 requestDeleteCurrent 安全 no-op、析构不崩溃", {
    test_support::TempDir dir("wallpaper_delete_after_stop");
    const fs::path root = dir.path / "videos";
    fs::create_directories(root);
    const std::string a = (root / "only.mp4").make_preferred().string();
    writeFile(a, 32);

    wp::WallpaperService service;
    service.setSettingsPathForTest((dir.path / "wallpaper" / "settings.json").string());
    service.setVideoScannerForTest([a](const std::string&) {
        wp::VideoScanResult result;
        result.status = wp::LibraryScanStatus::Ok;
        result.files = {a};
        return result;
    });
    service.setImageScannerForTest([](const std::string&, int) {
        return wp::ImageScanResult{};
    });
    service.setPlayerForTest(std::make_unique<wp::NullPlayerBackend>());
    std::atomic<int> trashCalls{0};
    service.setTrashMoverForTest([&](const std::string&) {
        ++trashCalls;
        return trash::TrashOutcome{};
    });
    std::atomic<int> handlerCalls{0};
    service.setDeleteResultHandler([&](bool, const std::string&, int) { ++handlerCalls; });

    service.start();
    service.stop();
    service.requestDeleteCurrent();
    // 前置门生效：没有线程对象诞生（joinable 即会失败）；也不该碰 mover / 结果出口。
    DOCTEST_CHECK(service.waitDeleteIdleForTest(2000));
    DOCTEST_CHECK_EQ(trashCalls.load(), 0);
    DOCTEST_CHECK_EQ(handlerCalls.load(), 0);
    return true;  // service 析构再跑一次 stop()：不得 terminate
});

// bug358 治本分支：未 start 直接请求同样不建线程（旧实现会留下已结束但 joinable 的对象）。
REGISTER_TEST(wallpaper_delete_request_before_start_is_noop,
              "wallpaper delete: 未 start 时 requestDeleteCurrent 不建线程", {
    wp::WallpaperService service;
    std::atomic<int> handlerCalls{0};
    service.setDeleteResultHandler([&](bool, const std::string&, int) { ++handlerCalls; });

    service.requestDeleteCurrent();
    // timeout=0：joinable 时立即返回 false，正好用来证明线程对象未诞生。
    DOCTEST_CHECK(service.waitDeleteIdleForTest(0));
    DOCTEST_CHECK_EQ(handlerCalls.load(), 0);
    return true;
});
