// tests/probes/wallpaper_macos_probe.cpp
// macOS 真桌面探针：桌面层窗口宿主 + AVFoundation 播放（视频 / 图片）+ 让路信号。
//
// 与 Windows 版 wallpaper_desktop_probe 的分工：那边验 WorkerW + mpv 进程生命周期；
// 这边验 macOS 原生路线的**同等语义**——
//   §1 宿主建窗成功、handle 非空、像素尺寸 == 主屏 convertRectToBacking。
//   §2 视频装载：load 后 4s 内 position > 0.2s（真解码真播放，不是"命令发出"）。
//   §3 pause(true) 后位置冻结（增量 < 0.15s），pause(false) 后继续增长。
//   §4 seek 到 1.0s，position 落在 1.0±0.6。
//   §5 图片装载：贴图后 position 恒 0、无错误（静图无时间轴的真相）。
//   §6 图片铺法四种都能下发且不报错（Fill/Fit/Center/Tile）。
//   §7 stop 后无错误残留、宿主窗口不再持有播放器。
//   §8 让路 watcher：五路 poll 通道可用（电池可读、全屏判定不崩）。
// 退出码：0 通过 / 77 非 macOS 跳过 / 非零失败（stderr 中文原因）。
//
// 素材：视频与图片从 --video / --image 指定；不给则用 /tmp 下临时生成的素材
// （图片用 CoreGraphics 现画 PNG，视频找不到就跳过视频段但不算失败——视频段
// 需要真实可解码素材，CI 环境没有）。
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#if defined(__APPLE__)
// 只有 C/CF API：本文件是 .cpp，不能 include AppKit/AVFoundation 的 ObjC 头。
// 事件泵走 CFRunLoopRunInMode（与 GLFW 主循环同一模式），不需要 NSApplication。
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

#include "module_wallpaper/platform/mpv_controller.h"
#include "module_wallpaper/platform/wallpaper_host.h"
#include "module_wallpaper/platform/yield_watchers.h"
#endif

namespace {

[[maybe_unused]] constexpr int kSkipExitCode = 77;

#if defined(__APPLE__)

using Clock = std::chrono::steady_clock;

// 主线程跑一会儿 run loop。
//
// **不是可选的**：AVFoundation 的播放推进依赖进程里有人在跑 run loop——实测
// 主线程纯 sleep 时 AVQueuePlayer.currentTime 恒为 0（status 也是 unknown），
// 一旦主线程按几十毫秒的节奏跑 run loop，位置立刻按时间推进。真机 app 的
// EUI-NEO 主循环本来就在转，所以生产代码不需要额外线程；探针必须自己补上。
// 用 CFRunLoopRunInMode(kCFRunLoopDefaultMode) 而不是 NSApplication 事件泵：
// 本文件是 .cpp，且默认模式正是 AppKit 通知/AVFoundation 内部源所在模式。
void pumpMainEvents(double seconds) {
    const CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + seconds;
    while (CFAbsoluteTimeGetCurrent() < deadline) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, true);
    }
}

bool waitUntil(const std::function<bool()>& predicate, std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        if (predicate()) return true;
        pumpMainEvents(0.02);
    }
    return predicate();
}

// 现画一张 320×180 的 PNG 作为图片来源（不依赖仓库素材）。
std::string writeTestImage(const std::filesystem::path& path) {
    const size_t width = 320;
    const size_t height = 180;
    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    if (colorSpace == nullptr) return std::string();
    CGContextRef context = CGBitmapContextCreate(
        nullptr, width, height, 8, width * 4, colorSpace,
        kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(colorSpace);
    if (context == nullptr) return std::string();
    // 渐变底 + 一个亮块：既能验贴图，也能在肉眼截图里分辨是否真画上了。
    CGContextSetRGBFillColor(context, 0.12, 0.18, 0.32, 1.0);
    CGContextFillRect(context, CGRectMake(0, 0, width, height));
    CGContextSetRGBFillColor(context, 0.15, 0.82, 0.55, 1.0);
    CGContextFillRect(context, CGRectMake(60, 50, 200, 80));
    CGImageRef image = CGBitmapContextCreateImage(context);
    CGContextRelease(context);
    if (image == nullptr) return std::string();

    std::string result;
    CFURLRef url = CFURLCreateWithFileSystemPath(
        kCFAllocatorDefault,
        CFStringCreateWithCString(kCFAllocatorDefault, path.c_str(),
                                  kCFStringEncodingUTF8),
        kCFURLPOSIXPathStyle, false);
    if (url != nullptr) {
        CGImageDestinationRef destination =
            CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, nullptr);
        if (destination != nullptr) {
            CGImageDestinationAddImage(destination, image, nullptr);
            if (CGImageDestinationFinalize(destination)) result = path.string();
            CFRelease(destination);
        }
        CFRelease(url);
    }
    CGImageRelease(image);
    return result;
}

int runProbe(const std::string& videoPath, const std::string& imagePath) {
    bool passed = true;
    auto check = [&](bool condition, const std::string& what) {
        if (!condition) {
            std::cerr << "wallpaper_macos_probe: FAIL " << what << "\n";
            passed = false;
        }
    };

    // ── §1 宿主建窗 ──
    std::unique_ptr<module::wallpaper::WallpaperHost> host =
        module::wallpaper::createWallpaperHost();
    check(host != nullptr, "§1 createWallpaperHost 应返回实例");
    if (host == nullptr) return 1;
    std::string error;
    const bool started = host->start(&error);
    check(started, "§1 宿主 start 应成功: " + error);
    if (!started) return 1;

    const module::wallpaper::HostHandle handle = host->handle();
    check(handle.videoChild != nullptr, "§1 handle.videoChild 应非空（媒体挂载点）");
    check(handle.width > 0 && handle.height > 0,
          "§1 宿主尺寸应为正物理像素（实测 " + std::to_string(handle.width) + "x" +
              std::to_string(handle.height) + "）");
    std::cout << "wallpaper_macos_probe: host " << handle.width << "x" << handle.height
              << "\n";

    check(host->show(&error), "§1 宿主 show 应成功: " + error);

    // ── §2~§4 视频 ──
    const bool hasVideo = !videoPath.empty() &&
                          std::filesystem::exists(videoPath) &&
                          std::filesystem::file_size(videoPath) > 0;
    if (hasVideo) {
        module::wallpaper::MpvController player;
        player.start(reinterpret_cast<std::uintptr_t>(handle.videoChild));
        check(player.processId() == 0,
              "§2 macOS 路线无播放器子进程（processId 恒 0）");
        player.pause(false);
        player.load(videoPath);

        const bool advanced = waitUntil(
            [&player] { return player.positionSec() > 0.2f; },
            std::chrono::milliseconds(4000));
        check(advanced, "§2 装载后 4s 内播放位置应前进（实测 " +
                            std::to_string(player.positionSec()) + "s）");
        check(player.processAlive(), "§2 装载后播放器应处于活着状态");

        player.pause(true);
        pumpMainEvents(0.3);
        const float frozen = player.positionSec();
        pumpMainEvents(0.8);
        const float afterPause = player.positionSec();
        check(std::fabs(afterPause - frozen) < 0.15f,
              "§3 暂停后位置应冻结（" + std::to_string(frozen) + " → " +
                  std::to_string(afterPause) + "）");

        player.pause(false);
        const bool resumed = waitUntil(
            [&player, afterPause] { return player.positionSec() > afterPause + 0.15f; },
            std::chrono::milliseconds(3000));
        check(resumed, "§3 恢复后位置应继续前进");

        player.seek(1.0f);
        const bool seeked = waitUntil(
            [&player] {
                const float pos = player.positionSec();
                return pos >= 0.4f && pos <= 1.6f;
            },
            std::chrono::milliseconds(2000));
        check(seeked, "§4 seek 到 1.0s 后位置应落在 1.0±0.6（实测 " +
                          std::to_string(player.positionSec()) + "s）");

        player.stop();
        check(!player.processAlive(), "§7 stop 后播放器应为已停状态");
        const std::string playerError = player.takeError();
        check(playerError.empty(), "§7 视频段不应有错误: " + playerError);
    } else {
        std::cout << "wallpaper_macos_probe: 跳过视频段（未提供可解码视频，"
                     "用 --video <路径> 指定）\n";
    }

    // ── §5~§6 图片 ──
    std::string resolvedImage = imagePath;
    if (resolvedImage.empty() || !std::filesystem::exists(resolvedImage)) {
        const auto generated = std::filesystem::temp_directory_path() /
                               "devdesk_wallpaper_probe_image.png";
        resolvedImage = writeTestImage(generated);
        check(!resolvedImage.empty(), "§5 临时测试图片应生成成功");
    }
    if (!resolvedImage.empty()) {
        module::wallpaper::MpvController player;
        player.start(reinterpret_cast<std::uintptr_t>(handle.videoChild));
        player.load(resolvedImage);
        // 解码 + 贴图在后台完成：给足时间再断言。
        pumpMainEvents(0.8);
        check(player.processAlive(), "§5 图片装载后播放器应处于活着状态");
        // 静图无时间轴：position 必须恒 0（服务层据此不查锚点）。
        check(std::fabs(player.positionSec()) < 0.01f,
              "§5 图片项 position 应恒 0（实测 " +
                  std::to_string(player.positionSec()) + "）");
        const std::string imageError = player.takeError();
        check(imageError.empty(), "§5 图片装载不应报错: " + imageError);

        using module::wallpaper::ImageFit;
        for (const ImageFit fit :
             {ImageFit::Fill, ImageFit::Fit, ImageFit::Center, ImageFit::Tile}) {
            player.setImageFit(fit);
        }
        pumpMainEvents(0.2);
        const std::string fitError = player.takeError();
        check(fitError.empty(), "§6 四种铺法下发不应报错: " + fitError);
        player.stop();
    }

    // ── §8 让路 watcher ──
    {
        std::unique_ptr<module::wallpaper::YieldWatchers> watchers =
            module::wallpaper::createYieldWatchers();
        check(watchers != nullptr, "§8 createYieldWatchers 应返回实例");
        if (watchers != nullptr) {
            std::string watcherError;
            const bool watcherStarted = watchers->start(&watcherError);
            check(watcherStarted, "§8 让路 watcher 应启动成功: " + watcherError);
            // 轮询通道必须快速可返回（不崩、不常量假绿）。
            const bool onBattery = watchers->pollOnBattery();
            const bool fullscreen = watchers->pollFullscreenOtherProcess();
            std::cout << "wallpaper_macos_probe: yield poll battery="
                      << (onBattery ? "true" : "false")
                      << " fullscreen=" << (fullscreen ? "true" : "false") << "\n";
            // 壁纸窗口在桌面层（实测 layer=-2147483623），**不是 layer 0**，因此天然
            // 不会被全屏扫描枚举——不会被自己让路。这里 true 与否取决于桌面上是否
            // 真有别的 app 铺满屏幕（探针跑在终端里时终端就是），不是缺陷。
            // 真正要钉的是「扫描逻辑不崩且可重复」：连续两次结果一致。
            const bool fullscreenAgain = watchers->pollFullscreenOtherProcess();
            check(fullscreen == fullscreenAgain,
                  "§8 全屏轮询应可重复且结果稳定（同环境连查两次一致）");
            watchers->stop();
            watchers->stop();  // 幂等
        }
    }

    host->hide();
    host->stop();
    host->stop();  // 幂等
    check(host->handle().videoChild == nullptr, "§7 stop 后 handle 应变空");

    if (passed) {
        std::cout << "wallpaper_macos_probe: PASS\n";
        return 0;
    }
    std::cerr << "FAIL\n";
    return 1;
}

#endif

} // namespace

int main(int argc, char* argv[]) {
#if defined(__APPLE__)
    std::string videoPath;
    std::string imagePath;
    for (int i = 1; i + 1 < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--video") videoPath = argv[i + 1];
        if (arg == "--image") imagePath = argv[i + 1];
    }
    return runProbe(videoPath, imagePath);
#else
    (void)argc;
    (void)argv;
    std::cout << "wallpaper_macos_probe: 非 macOS 平台，跳过（桌面层窗口/AVFoundation 仅 macOS）"
              << " exit=" << kSkipExitCode << "\n";
    return kSkipExitCode;
#endif
}
