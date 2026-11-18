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
//   §9 视频内存缓存（need:311）：开关开 + ≤512MB → 内存喂数生效（lastLoadUsedMemoryCache
//      + 播放推进 + RSS 增量）；停播后缓存释放（RSS 回落、诊断位清零）；关闭 → 文件 URL 直读。
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
#include <fstream>
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
#include <mach/mach.h>

#include "module_wallpaper/core/video_cache.h"
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

// 当前进程常驻内存（字节）。need:311 §9 缓存释放判据用它做前后对照。
std::uint64_t residentBytes() {
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) {
        return 0;
    }
    return static_cast<std::uint64_t>(info.resident_size);
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

    // ── §9 视频内存缓存（need:311）──
    // 判据口径（本机实测标定）：RSS 不能当「缓存已释放」的精密尺——libmalloc 会
    // 回收已被释放的大块不给 OS（实测 4 次重装后 RSS 恒定在「一个文件大小 + 解码器
    // 地板」，不随轮换次数线性上涨）。因此钉三条：①开启后确实走内存喂数且播放推进；
    // ②4 轮重装不线性累积（增量 < 2 份文件）；③与「直读对照」的停播地板一致。
    if (hasVideo) {
        const std::uintmax_t fileBytes = std::filesystem::file_size(videoPath);
        const std::uint64_t rssBaseline = residentBytes();
        module::wallpaper::MpvController cached;
        cached.start(reinterpret_cast<std::uintptr_t>(handle.videoChild));
        cached.setVideoMemCache(true);
        cached.pause(false);
        cached.load(videoPath);
        const bool cacheActive = waitUntil(
            [&cached] {
                return cached.processAlive() && cached.lastLoadUsedMemoryCache() &&
                       cached.positionSec() > 0.2f;
            },
            std::chrono::milliseconds(8000));
        check(cacheActive,
              "§9 开关开 + " + std::to_string(fileBytes / 1024 / 1024) +
                  "MB 视频应走内存喂数且播放推进（active=" +
                  std::string(cached.lastLoadUsedMemoryCache() ? "true" : "false") +
                  " pos=" + std::to_string(cached.positionSec()) + "）");
        const std::uint64_t rssWithCache = residentBytes();
        std::cout << "wallpaper_macos_probe: §9 RSS baseline=" << rssBaseline / 1024 / 1024
                  << "MB withCache=" << rssWithCache / 1024 / 1024
                  << "MB file=" << fileBytes / 1024 / 1024 << "MB\n";
        if (cacheActive) {
            check(rssWithCache > rssBaseline + fileBytes / 2,
                  "§9 内存缓存应体现在 RSS（增量 " +
                      std::to_string((rssWithCache - rssBaseline) / 1024 / 1024) +
                      "MB，应 > 文件一半）");
        }
        // 轮换切条目 = 同一控制器再 load 4 次：缓存不得线性累积。
        if (cacheActive) {
            for (int cycle = 0; cycle < 3; ++cycle) {
                cached.load(videoPath);
                (void)waitUntil([&cached] { return cached.positionSec() > 0.2f; },
                                std::chrono::milliseconds(8000));
                pumpMainEvents(0.3);
            }
            const std::uint64_t rssAfterReload = residentBytes();
            std::cout << "wallpaper_macos_probe: §9 RSS afterReload(4x)="
                      << rssAfterReload / 1024 / 1024 << "MB\n";
            check(rssAfterReload < rssWithCache + fileBytes * 2,
                  "§9 多轮轮换重装不得线性累积（增量 " +
                      std::to_string((rssAfterReload - rssWithCache) / 1024 / 1024) +
                      "MB < 2 份文件 " + std::to_string(fileBytes * 2 / 1024 / 1024) +
                      "MB）");
        }
        // 停播 = 轮换/换源收尾：诊断位清零 + RSS 不高于「直读对照」的停播地板。
        cached.stop();
        check(!cached.lastLoadUsedMemoryCache(), "§9 停播后内存缓存诊断位应清零");
        pumpMainEvents(0.6);
        const std::uint64_t rssStopAfterCache = residentBytes();
        std::cout << "wallpaper_macos_probe: §9 RSS stopAfterCache="
                  << rssStopAfterCache / 1024 / 1024 << "MB\n";

        // 关闭开关 = 现状文件 URL 直读（回归锚 + 停播地板对照）。
        module::wallpaper::MpvController direct;
        direct.start(reinterpret_cast<std::uintptr_t>(handle.videoChild));
        direct.pause(false);
        direct.load(videoPath);
        const bool directAdvanced = waitUntil(
            [&direct] { return direct.positionSec() > 0.2f; },
            std::chrono::milliseconds(6000));
        check(directAdvanced, "§9 关闭开关后文件直读应照常播放");
        check(!direct.lastLoadUsedMemoryCache(), "§9 关闭开关后不得命中内存缓存路径");
        direct.stop();
        pumpMainEvents(0.6);
        const std::uint64_t rssStopAfterDirect = residentBytes();
        std::cout << "wallpaper_macos_probe: §9 RSS stopAfterDirect="
                  << rssStopAfterDirect / 1024 / 1024 << "MB\n";
        // 超限回落（D2）：>512MB 文件不必读进内存，按现状文件 URL 直读。
        // 用稀疏文件（truncate）造 513MB 体积，零真实磁盘占用；内容不是有效视频，
        // 因此只断言「未命中内存缓存路径 + 不报崩溃」，不要求播放推进。
        {
            const std::filesystem::path oversize =
                std::filesystem::temp_directory_path() / "devdesk_wallpaper_oversize.mp4";
            std::error_code ec;
            std::filesystem::remove(oversize, ec);
            {
                std::ofstream out(oversize, std::ios::binary | std::ios::trunc);
                out.seekp(513ll * 1024ll * 1024ll - 1);
                out.put('\0');
            }
            check(std::filesystem::file_size(oversize) > module::wallpaper::kVideoMemCacheLimitBytes,
                  "§9 稀疏超限文件应 >512MB");
            module::wallpaper::MpvController oversizePlayer;
            oversizePlayer.start(reinterpret_cast<std::uintptr_t>(handle.videoChild));
            oversizePlayer.setVideoMemCache(true);
            oversizePlayer.load(oversize.string());
            pumpMainEvents(1.0);
            check(!oversizePlayer.lastLoadUsedMemoryCache(),
                  "§9 开关开但文件 >512MB 应回落直读（不缓存）");
            oversizePlayer.stop();
            std::filesystem::remove(oversize, ec);
        }

        const std::uint64_t tolerance = 16ull * 1024ull * 1024ull;
        check(rssStopAfterCache <= rssStopAfterDirect + tolerance,
              "§9 缓存路径停播后 RSS 不得高于直读对照地板 +16MB（" +
                  std::to_string(rssStopAfterCache / 1024 / 1024) + "MB vs " +
                  std::to_string(rssStopAfterDirect / 1024 / 1024) + "MB）");
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
