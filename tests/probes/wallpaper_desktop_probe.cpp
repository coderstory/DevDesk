// tests/probes/wallpaper_desktop_probe.cpp
// 真桌面探针（Batch 3a）：WorkerW 宿主 + mpv 视频嵌入。
//
// 断言（需求 §6.1/§6.2/§9）：
//   §1 host.start 成功、videoChild 非空、宿主尺寸 == 主屏 rcMonitor 物理像素。
//   §2 播放器以 videoChild 为 --wid 启动并 load clip-a.mp4，1.5s 内 position > 0.2s。
//   §3 pause(true) 后 0.8s 位置增量 < 0.15s；pause(false) 后继续增长。
//   §4 seek 到 1.0s，position 落在 1.0±0.5。
//   §5 stop 后进程退出（≤3s，按本次 pid 判残留）；宿主窗口销毁；无错误残留。
// 退出码：0 通过 / 77 非 Windows 跳过 / 非零失败（stderr 中文原因）。
// 探针会短暂把测试视频显示在桌面图标后面，所有退出路径都清理（stop player + stop host）。
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "core/paths.h"
#include "module_wallpaper/platform/mpv_controller.h"
#include "module_wallpaper/platform/mpv_player.h"
#include "module_wallpaper/platform/wallpaper_host.h"
#endif

namespace {

constexpr int kSkipExitCode = 77;

#if defined(_WIN32)

constexpr const char* kDefaultClip =
    "D:/Data/Downloads/dynamic-wallpaper-master/fixtures/clip-a.mp4";

using Clock = std::chrono::steady_clock;

bool waitUntil(const std::function<bool()>& predicate, std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return predicate();
}

bool processStillRunning(std::uint32_t pid) {
    if (pid == 0) return false;
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) return false;
    const bool running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return running;
}

bool primaryMonitorSize(int* width, int* height) {
    POINT origin{0, 0};
    HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    if (monitor == nullptr) return false;
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return false;
    *width = info.rcMonitor.right - info.rcMonitor.left;
    *height = info.rcMonitor.bottom - info.rcMonitor.top;
    return true;
}

std::string envValue(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
}

#endif  // _WIN32

}  // namespace

int main(int argc, char* argv[]) {
#if !defined(_WIN32)
    (void)argc;
    (void)argv;
    std::cout << "wallpaper_desktop_probe: 非 Windows 平台，跳过（WorkerW/mpv 仅 Windows）"
              << " exit=" << kSkipExitCode << "\n";
    return kSkipExitCode;
#else
    using module::wallpaper::HostHandle;
    using module::wallpaper::MpvPlayer;
    using module::wallpaper::WallpaperHost;

    // 探针独立进程：先对齐 DevDesk 的 PerMonitorV2（模块自身不重复设置 DPI，
    // 见需求 §6.5）。不设的话 DPI 虚拟化会让 rcMonitor 变成逻辑像素。
    (void)SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const std::string clipPath =
        argc > 1 ? std::string(argv[1]) : std::string(kDefaultClip);

    bool passed = true;
    auto check = [&passed](bool condition, const std::string& what) {
        if (condition) {
            std::cout << "[PASS] " << what << "\n";
        } else {
            std::cerr << "[FAIL] " << what << "\n";
            passed = false;
        }
    };

    std::unique_ptr<WallpaperHost> host = module::wallpaper::createWallpaperHost();
    MpvPlayer player;
    auto cleanup = [&]() {
        player.stop();
        if (host != nullptr) host->stop();
    };

    if (!std::filesystem::is_regular_file(clipPath)) {
        std::cerr << "wallpaper_desktop_probe: 测试视频不存在: " << clipPath << "\n";
        cleanup();
        return 3;
    }

    std::string error;
    if (!host->start(&error)) {
        std::cerr << "wallpaper_desktop_probe: 宿主启动失败: " << error << "\n";
        cleanup();
        return 4;
    }

    // ── §1 宿主 ──
    const HostHandle handle = host->handle();
    int monitorWidth = 0;
    int monitorHeight = 0;
    const bool monitorOk = primaryMonitorSize(&monitorWidth, &monitorHeight);
    check(handle.videoChild != nullptr, "§1 宿主：videoChild 句柄非空");
    check(monitorOk && handle.width == monitorWidth && handle.height == monitorHeight,
          "§1 宿主：尺寸 " + std::to_string(handle.width) + "x" +
              std::to_string(handle.height) + " == 主屏物理 " +
              std::to_string(monitorWidth) + "x" + std::to_string(monitorHeight));
    if (handle.videoChild == nullptr || !monitorOk) {
        cleanup();
        return 5;
    }

    if (!host->show(&error)) {
        std::cerr << "wallpaper_desktop_probe: 宿主显示失败: " << error << "\n";
        cleanup();
        return 6;
    }

    // ── mpv 路径解析（生产同款优先级；探针允许开发兜底） ──
    bool usedDevFallback = false;
    const std::string mpvPath = module::wallpaper::resolveMpvExecutable(
        envValue(module::wallpaper::kMpvPathEnvVar),
        core::paths::executableDir().string(), module::wallpaper::kMpvDevFallbackPath, true,
        &usedDevFallback);
    if (mpvPath.empty()) {
        std::cerr << "wallpaper_desktop_probe: 找不到 mpv.exe（DEVDESK_MPV_PATH / "
                     "<exeDir>/assets/mpv/mpv.exe / 开发兜底）\n";
        cleanup();
        return 7;
    }
    if (usedDevFallback) {
        std::cout << "[INFO] 使用开发兜底 mpv: " << mpvPath << "\n";
    }

    // ── §2 启动 + 装载 + 位置增长 ──
    player.setMute(true);
    player.pause(false);
    player.start(reinterpret_cast<std::uintptr_t>(handle.videoChild));
    player.load(clipPath);

    const auto playStarted = Clock::now();
    const bool grew = waitUntil(
        [&player]() { return player.positionSec() > 0.2f; }, std::chrono::milliseconds(1500));
    const float positionAfterStart = player.positionSec();
    check(grew, "§2 播放：1.5s 内 position > 0.2s（实测 " +
                    std::to_string(positionAfterStart) + "s）");
    if (!grew) {
        std::string playerError;
        if (player.takeError(&playerError)) {
            std::cerr << "wallpaper_desktop_probe: mpv 错误: " << playerError << "\n";
        }
        cleanup();
        return 8;
    }
    std::cout << "[INFO] 启动到 position>0.2 用时 " +
                     std::to_string(std::chrono::duration<double>(Clock::now() - playStarted)
                                        .count()) +
                     "s\n";

    // ── §3 暂停冻结 ──
    player.pause(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const float frozenBegin = player.positionSec();
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    const float frozenEnd = player.positionSec();
    const float frozenDelta = frozenEnd - frozenBegin;
    check(std::fabs(frozenDelta) < 0.15f,
          "§3 暂停：0.8s 内位置增量 " + std::to_string(frozenDelta) +
              "s（起点 " + std::to_string(frozenBegin) + "s，终点 " +
              std::to_string(frozenEnd) + "s，要求 |增量| < 0.15s）");

    // ── §4 seek（暂停态下做，避开 2s 短片循环回绕） ──
    player.seek(1.0f);
    const bool seekOk = waitUntil(
        [&player]() { return std::fabs(player.positionSec() - 1.0f) <= 0.5f; },
        std::chrono::milliseconds(1500));
    const float seekPosition = player.positionSec();
    check(seekOk, "§4 seek：position 落在 1.0±0.5（实测 " +
                      std::to_string(seekPosition) + "s）");

    // ── §3（续）解除暂停继续增长 ──
    player.pause(false);
    const float resumeBase = player.positionSec();
    const bool grewAfterResume = waitUntil(
        [&player, resumeBase]() { return player.positionSec() > resumeBase + 0.1f; },
        std::chrono::milliseconds(1000));
    const float positionAfterResume = player.positionSec();
    check(grewAfterResume, "§3 恢复：pause(false) 后继续增长（起点 " +
                               std::to_string(resumeBase) + "s → 实测 " +
                               std::to_string(positionAfterResume) + "s）");

    // ── §5 stop：进程退出 + 宿主销毁 ──
    const std::uint32_t mpvPid = player.processId();
    const auto stopStarted = Clock::now();
    player.stop();
    const double stopElapsed =
        std::chrono::duration<double>(Clock::now() - stopStarted).count();
    const bool exited = waitUntil([&player]() { return !player.processAlive(); },
                                  std::chrono::milliseconds(3000));
    const bool orphan = processStillRunning(mpvPid);
    check(mpvPid != 0 && exited && !orphan,
          "§5 停止：mpv pid " + std::to_string(mpvPid) + " 已退出且无残留（stop 耗时 " +
              std::to_string(stopElapsed) + "s）");

    const auto videoWindow = reinterpret_cast<HWND>(handle.videoChild);
    host->stop();
    check(IsWindow(videoWindow) == FALSE && host->handle().videoChild == nullptr,
          "§5 停止：宿主窗口已销毁，handle().videoChild 为空");

    std::string hostError;
    if (!passed) hostError = "（探针失败，见上方 [FAIL]）";
    cleanup();
    if (!passed) {
        std::cerr << "wallpaper_desktop_probe: FAIL" << hostError << "\n";
        return 1;
    }
    std::cout << "wallpaper_desktop_probe: PASS\n";
    return 0;
#endif
}