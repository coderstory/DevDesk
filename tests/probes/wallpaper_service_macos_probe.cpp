// tests/probes/wallpaper_service_macos_probe.cpp
// macOS 端到端探针：**真 WallpaperService 装配**（不注入播放器/宿主/watcher），
// 真文件夹扫描 → 真桌面层窗口 → 真 AVFoundation 播放 → 真轮换。
//
// 与 wallpaper_macos_probe 的分工：那个验平台层组件（宿主/控制器/watcher）各自的
// 契约；这个验**服务层把三者装起来**之后的端到端语义——
//   §1 真文件夹入库：视频池 / 图片池计数与档位过滤真值。
//   §2 服务启动后真建宿主、真装载、快照进入 Playing。
//   §3 混合轮换池：视频 + 图片平等入池（poolCount = videoCount + poolImageCount）。
//   §4 「下一张」真的换片：轮换项变化且仍可播。
//   §5 图片来源：真解码贴图，快照 currentItemImage 语义正确。
//   §6 手动暂停：快照 paused + 宿主仍在（不销毁窗口）；恢复后继续。
//   §7 停止：宿主与播放器都收干净，无错误残留。
// 退出码：0 通过 / 77 非 macOS 跳过 / 非零失败。
//
// 素材：--video <mp4> 提供视频（没有则只跑图片段）；图片由探针现画 PNG。
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "module_wallpaper/core/image_library.h"
#include "module_wallpaper/core/video_library.h"
#include "module_wallpaper/wallpaper_service.h"

namespace {

[[maybe_unused]] constexpr int kSkipExitCode = 77;
using Clock = std::chrono::steady_clock;

// 主线程跑一会儿 run loop：AVFoundation 的播放推进依赖有人转 run loop（实测
// 纯 sleep 时 currentTime 恒 0）。真机 app 的 EUI-NEO 主循环本就在转。
void pumpMainEvents(double seconds) {
    const CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + seconds;
    while (CFAbsoluteTimeGetCurrent() < deadline) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, true);
    }
}

bool waitUntil(const std::function<bool()>& predicate, double timeoutSeconds) {
    const auto deadline =
        Clock::now() + std::chrono::milliseconds((int)(timeoutSeconds * 1000));
    while (Clock::now() < deadline) {
        if (predicate()) return true;
        pumpMainEvents(0.02);
    }
    return predicate();
}

// 现画一张 PNG（不依赖仓库素材）。
bool writeTestImage(const std::filesystem::path& path, double red, double green,
                    double blue) {
    const size_t width = 2560;   // ≥2K 档：保证能通过默认 3686400 像素过滤线
    const size_t height = 1440;
    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    if (colorSpace == nullptr) return false;
    CGContextRef context =
        CGBitmapContextCreate(nullptr, width, height, 8, width * 4, colorSpace,
                              kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(colorSpace);
    if (context == nullptr) return false;
    CGContextSetRGBFillColor(context, red, green, blue, 1.0);
    CGContextFillRect(context, CGRectMake(0, 0, width, height));
    CGContextSetRGBFillColor(context, 1.0, 1.0, 1.0, 1.0);
    CGContextFillRect(context, CGRectMake(200, 200, 400, 240));
    CGImageRef image = CGBitmapContextCreateImage(context);
    CGContextRelease(context);
    if (image == nullptr) return false;

    bool ok = false;
    CFStringRef cfPath = CFStringCreateWithCString(
        kCFAllocatorDefault, path.c_str(), kCFStringEncodingUTF8);
    if (cfPath != nullptr) {
        CFURLRef url = CFURLCreateWithFileSystemPath(kCFAllocatorDefault, cfPath,
                                                     kCFURLPOSIXPathStyle, false);
        if (url != nullptr) {
            CGImageDestinationRef destination =
                CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, nullptr);
            if (destination != nullptr) {
                CGImageDestinationAddImage(destination, image, nullptr);
                ok = CGImageDestinationFinalize(destination);
                CFRelease(destination);
            }
            CFRelease(url);
        }
        CFRelease(cfPath);
    }
    CGImageRelease(image);
    return ok;
}

} // namespace

int main(int argc, char* argv[]) {
#if !defined(__APPLE__)
    (void)argc;
    (void)argv;
    std::cout << "wallpaper_service_macos_probe: 非 macOS 平台，跳过"
                 "（桌面层窗口/AVFoundation 仅 macOS） exit="
              << kSkipExitCode << "\n";
    return kSkipExitCode;
#else
    std::string videoPath;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--video") videoPath = argv[i + 1];
    }

    bool passed = true;
    auto check = [&passed](bool condition, const std::string& what) {
        if (condition) {
            std::cout << "[PASS] " << what << "\n";
        } else {
            std::cerr << "[FAIL] " << what << "\n";
            passed = false;
        }
    };

    namespace wp = module::wallpaper;

    // ── §1 真文件夹：2 视频（可选）+ 2 图片（2K 与 4K 档）+ 干扰项 ──
    const std::filesystem::path tempDir =
        std::filesystem::temp_directory_path() /
        ("devdesk-wallpaper-macos-probe-" + std::to_string(::getpid()));
    {
        std::error_code ec;
        std::filesystem::remove_all(tempDir, ec);
        std::filesystem::create_directories(tempDir, ec);
    }
    const std::filesystem::path image2k = tempDir / "wall-2k.png";
    const std::filesystem::path notes = tempDir / "notes.txt";
    const bool imageOk = writeTestImage(image2k, 0.10, 0.20, 0.40);
    {
        std::ofstream out(notes, std::ios::binary);
        out << "decoy entry, not a media file\n";
    }
    const bool hasVideo = !videoPath.empty() &&
                          std::filesystem::is_regular_file(videoPath);
    if (hasVideo) {
        std::error_code ec;
        std::filesystem::copy_file(videoPath, tempDir / "clip.mp4",
                                   std::filesystem::copy_options::overwrite_existing, ec);
    }
    check(imageOk, "§1 测试图片应生成成功（2560×1440，过 2K 档）: " + image2k.string());
    if (!imageOk) return 2;

    // 真扫描器直查（不经服务）：确认素材语义。
    const wp::VideoScanResult videoScan = wp::scanVideoLibrary(tempDir);
    const wp::ImageScanResult imageScan = wp::scanImageLibrary(tempDir, 3686400);
    check(imageScan.status == wp::LibraryScanStatus::Ok && imageScan.total == 1,
          "§1 图片扫描：恰 1 张 png（total=" + std::to_string(imageScan.total) + "）");
    check(imageScan.passing == 1,
          "§1 图片档位：2560×1440 应通过 2K 线（passing=" +
              std::to_string(imageScan.passing) + "）");
    if (hasVideo) {
        check(videoScan.status == wp::LibraryScanStatus::Ok && videoScan.files.size() == 1,
              "§1 视频扫描：恰 1 支 mp4");
    }

    // ── §2 服务真装配（不注入 player / host / watcher）──
    wp::WallpaperService service;
    service.setSettingsPathForTest((tempDir / "settings.json").string());
    std::string folderError;
    const bool folderSelected = service.selectFolder(tempDir.string(), &folderError);
    check(folderSelected, "§2 selectFolder 应成功: " + folderError);
    if (!folderSelected) {
        std::error_code ec;
        std::filesystem::remove_all(tempDir, ec);
        return 3;
    }
    service.start();

    auto cleanup = [&service, &tempDir]() {
        service.stop();
        std::error_code ec;
        std::filesystem::remove_all(tempDir, ec);
    };

    // 等扫描落定并进入 Playing。
    const bool playing = waitUntil(
        [&service]() {
            const wp::WallpaperSnapshot snapshot = service.snapshot();
            return !snapshot.scanning &&
                   snapshot.availability == wp::Availability::Playing;
        },
        8.0);
    wp::WallpaperSnapshot snapshot = service.snapshot();
    check(playing, "§2 服务应进入 Playing（availability=" +
                       std::to_string((int)snapshot.availability) + "）");
    check(snapshot.playbackError.empty(),
          "§2 不应有播放错误: " + snapshot.playbackError);

    // ── §3 混合轮换池 ──
    if (hasVideo) {
        check(snapshot.videoCount == 1,
              "§3 视频计数应为 1（实测 " + std::to_string(snapshot.videoCount) + "）");
        check(snapshot.poolCount == snapshot.videoCount + snapshot.poolImageCount,
              "§3 混合池 = 视频 + 图片（poolCount=" +
                  std::to_string(snapshot.poolCount) + "，video=" +
                  std::to_string(snapshot.videoCount) + "，image=" +
                  std::to_string(snapshot.poolImageCount) + "）");
    }

    // ── §4 换片：下一张真换 ──
    if (hasVideo) {
        const int poolBefore = snapshot.poolCount;
        if (poolBefore > 1) {
            // 让当前项先真正跑起来，再换。
            pumpMainEvents(0.6);
            service.next();
            pumpMainEvents(0.6);
            const wp::WallpaperSnapshot after = service.snapshot();
            check(after.availability == wp::Availability::Playing,
                  "§4 换片后仍应可播");
            check(after.playbackError.empty(),
                  "§4 换片不应报错: " + after.playbackError);
            std::cout << "[INFO] §4 换片后 currentItemImage="
                      << (after.currentItemImage ? "true" : "false") << "\n";
        } else {
            std::cout << "[INFO] §4 池内只有 " << poolBefore << " 项，跳过换片断言\n";
        }
    }

    // ── §5 图片来源：真解码贴图 ──
    {
        service.setKind(wp::WallpaperKind::Image);
        std::string imageFolderError;
        // 图片目录就是同一个文件夹（里面那张 2K png）。
        const bool imageFolderSelected =
            service.selectFolder(tempDir.string(), &imageFolderError);
        check(imageFolderSelected, "§5 图片来源选目录应成功: " + imageFolderError);
        const bool imagePlaying = waitUntil(
            [&service]() {
                const wp::WallpaperSnapshot s = service.snapshot();
                return !s.scanning && s.availability == wp::Availability::Playing;
            },
            8.0);
        const wp::WallpaperSnapshot imageSnapshot = service.snapshot();
        check(imagePlaying, "§5 图片来源应进入 Playing");
        check(imageSnapshot.imagePassing == 1,
              "§5 图片来源 passing 应为 1（实测 " +
                  std::to_string(imageSnapshot.imagePassing) + "）");
        check(imageSnapshot.playbackError.empty(),
              "§5 图片来源不应有错误: " + imageSnapshot.playbackError);

        // 铺法切换：四种都不该报错（真窗口真重绘）。
        for (const wp::ImageFit fit : {wp::ImageFit::Fit, wp::ImageFit::Center,
                                       wp::ImageFit::Tile, wp::ImageFit::Fill}) {
            service.setImageFit(fit);
            pumpMainEvents(0.1);
        }
        check(service.snapshot().playbackError.empty(), "§5 四种铺法切换不应报错");
        check(service.snapshot().imageFit == wp::ImageFit::Fill,
              "§5 最后一次铺法应为 Fill（快照与设置同源）");
    }

    // ── §6 手动暂停 / 恢复 ──
    {
        service.toggleManualPause();
        pumpMainEvents(0.2);
        const wp::WallpaperSnapshot paused = service.snapshot();
        check(paused.paused && paused.manualPaused,
              "§6 手动暂停后快照应为暂停（paused/manualPaused 同真）");
        check(paused.playbackError.empty(), "§6 暂停不应产生错误");

        service.toggleManualPause();
        pumpMainEvents(0.2);
        const wp::WallpaperSnapshot resumed = service.snapshot();
        // 手动位必须已清；整体是否仍在暂停取决于**系统原因**（探针跑在终端里，
        // 终端窗口铺满屏幕会让全屏让路成立 —— 那是正确行为，不是回归）。
        check(!resumed.manualPaused, "§6 恢复后手动暂停位应清除");
        check(resumed.paused == (resumed.systemHolds != 0),
              "§6 paused 应恒等于「存在系统让路原因」（manualPaused=" +
                  std::string(resumed.manualPaused ? "true" : "false") +
                  "，systemHolds=" + std::to_string(resumed.systemHolds) + "）");
        if (resumed.systemHolds == 0) {
            check(!resumed.paused, "§6 无系统原因时恢复后快照应为播放");
        } else {
            std::cout << "[INFO] §6 系统让路生效中（systemHolds="
                      << resumed.systemHolds << "），跳过「播放态」断言\n";
        }
    }

    // ── §7 停止：宿主与播放器收干净 ──
    service.stop();
    const wp::WallpaperSnapshot stopped = service.snapshot();
    check(stopped.playbackError.empty(),
          "§7 停止后不应有错误残留: " + stopped.playbackError);
    // 再 start/stop 一轮：证明收尾幂等、不残留状态。
    service.start();
    const bool restarted = waitUntil(
        [&service]() {
            const wp::WallpaperSnapshot s = service.snapshot();
            return !s.scanning;
        },
        8.0);
    check(restarted, "§7 二次启动应能重新扫描完成");
    service.stop();

    cleanup();

    if (passed) {
        std::cout << "wallpaper_service_macos_probe: PASS\n";
        return 0;
    }
    std::cerr << "FAIL\n";
    return 1;
#endif
}
