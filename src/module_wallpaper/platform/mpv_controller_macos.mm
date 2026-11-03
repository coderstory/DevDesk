// src/module_wallpaper/platform/mpv_controller_macos.mm
// macOS 播放控制器：**同名接口的 AVFoundation 实现**（替代 Windows 的 mpv 进程 +
// 命名管道）。文件名跟控制器族走，但这里没有任何 mpv 进程——视频走 AVQueuePlayer +
// AVPlayerLooper 原生硬解，图片走宿主自带的自绘 CALayer。
//
// 为什么不是 mpv：AVFoundation 原生硬解 mp4/mov/m4v，画质与功耗都优于拉一个外部
// 进程，也没有 GPL 分发负担。服务层只认识 PlayerBackend / MpvController 接口，
// 两条路线在它眼里同形；`createMpvPlayer()` 名字保持不变（装配层零改动）。
//
// 线程模型：
//   - 全部 AVFoundation / CALayer 状态只在一个串行 dispatch queue 上读写（queue）。
//   - 公开方法只投递、不等待（PlayerBackend 契约要求快速返回）；stop 例外：要拿到
//     「已停」的确切状态，走同步投递 + 有界等待。
//   - positionSec() 读原子毫秒镜像，任意线程安全；真值由主队列定时采样刷新。
//
// 关键约束（改动前先读）：
//   * 图片解码必须在后台线程：一张 4K HEIC 足以卡住主线程一帧，而轮换是定时触发的，
//     卡帧会被读成「换壁纸时整个系统顿一下」。
//   * 速度挂 AVPlayer 而非 item：AVPlayerLooper 的模板 item 属性在 init 时冻结，
//     挂 item 会让「改设置立即生效」变成假的。恢复播放必须回放 desiredRate 而不是
//     调 play()——play() 把 rate 顶回 1.0，用户设的 0.5/1.5 在一次让路之后就没了。
//   * 先插后扫：新 item 先入队再扫掉旧 item，队列全程非空——清空后等 looper 异步
//     补位那一段里图层无 currentItem 可呈现，会闪屏。
#import <AppKit/AppKit.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <QuartzCore/QuartzCore.h>

#include "module_wallpaper/platform/mpv_controller.h"
#include "module_wallpaper/platform/wallpaper_host_macos_view.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

// 主队列上的播放器状态。做成 ObjC 对象：它持有 AVQueuePlayer / looper / 当前图层
// 选择，全部生命周期都在主队列上，避免 C++ 对象跨线程析构。
// 必须声明在全局作用域（ObjC 声明不能进命名空间）。
@interface DevDeskMacPlayerBox : NSObject
@property(nonatomic, strong) AVQueuePlayer* player;
@property(nonatomic, strong) AVPlayerLooper* looper;
@property(nonatomic, strong) DevDeskWallpaperHostView* host;
@property(nonatomic) float desiredRate;
@property(nonatomic) float volume;
@property(nonatomic) BOOL muted;
@property(nonatomic) int fit;  // module::wallpaper::ImageFit 的整数值
@property(nonatomic) BOOL imageItem;
@end

@implementation DevDeskMacPlayerBox
@end

namespace module::wallpaper {
namespace {

// 与扫描器扩展名白名单同口径：判定本条是静图还是视频。
bool isImagePath(const std::string& path) {
    std::string extension = std::filesystem::path(path).extension().string();
    if (!extension.empty() && extension.front() == '.') extension.erase(extension.begin());
    for (char& ch : extension) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return extension == "jpg" || extension == "jpeg" || extension == "png" ||
           extension == "webp" || extension == "heic";
}

// C 字节路径 → 文件 URL。
//
// **不要**用 [NSString stringWithUTF8String:] 或 stringWithFileSystemRepresentation:
// ——前者在非 UTF-8 字节上返回 nil，后者按原生字节解码后交给 fileURLWithPath: 会
// 二次解释（实测含中文的临时目录直接退化成 nil → "无法识别的文件路径"）。
// CFURLCreateFromFileSystemRepresentation 是唯一「按文件系统原生表示」的解释方式。
NSURL* fileURLFromUtf8Path(const std::string& path) {
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault, reinterpret_cast<const UInt8*>(path.c_str()),
        static_cast<CFIndex>(path.size()), false);
    return url != nullptr ? (__bridge_transfer NSURL*)url : nil;
}

// 后台同步解码。EXIF orientation 在本函数内应用（返回的 CGImage 已是摆正后的像素）。
// 返回 nullptr = 打不开（损坏 / 不是真图 / 无权限），调用方按「跳过这一张」处理。
CGImageRef decodeImage(const std::string& path) CF_RETURNS_RETAINED {
    @autoreleasepool {
        NSURL* url = fileURLFromUtf8Path(path);
        CGImageSourceRef source = CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr);
        if (source == nullptr) return nullptr;

        CGImageRef image = nullptr;
        NSDictionary* properties =
            CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(source, 0, nullptr));
        const int orientation =
            properties != nil
                ? [[properties objectForKey:(NSString*)kCGImagePropertyOrientation] intValue]
                : 1;
        if (orientation == 1) {
            // 绝大多数图是 Up：保留最快的直通解码。
            NSDictionary* options = @{(NSString*)kCGImageSourceShouldCacheImmediately : @YES};
            image = CGImageSourceCreateImageAtIndex(source, 0,
                                                    (__bridge CFDictionaryRef)options);
        } else {
            const int pixelWidth =
                [[properties objectForKey:(NSString*)kCGImagePropertyPixelWidth] intValue];
            const int pixelHeight =
                [[properties objectForKey:(NSString*)kCGImagePropertyPixelHeight] intValue];
            const int maxPixelSize = std::max(pixelWidth, pixelHeight);
            if (maxPixelSize > 0) {
                // 摆正路径：让 ImageIO 按 EXIF 旋转重绘。MaxPixelSize 设成存储像素的
                // 最大边——约束是「不超过」，等于原尺寸时不发生缩放，只拿到旋转效果。
                NSDictionary* options = @{
                    (NSString*)kCGImageSourceCreateThumbnailFromImageAlways : @YES,
                    (NSString*)kCGImageSourceCreateThumbnailWithTransform : @YES,
                    (NSString*)kCGImageSourceThumbnailMaxPixelSize : @(maxPixelSize),
                    (NSString*)kCGImageSourceShouldCacheImmediately : @YES,
                };
                image = CGImageSourceCreateThumbnailAtIndex(
                    source, 0, (__bridge CFDictionaryRef)options);
            } else {
                NSDictionary* fallback =
                    @{(NSString*)kCGImageSourceShouldCacheImmediately : @YES};
                image = CGImageSourceCreateImageAtIndex(source, 0,
                                                        (__bridge CFDictionaryRef)fallback);
            }
        }
        CFRelease(source);
        return image;  // Create 规则：调用方负责 CGImageRelease
    }
}

} // namespace

} // namespace module::wallpaper

namespace module::wallpaper {

struct MpvController::Impl {
    Impl()
        : queue(dispatch_queue_create("com.devdesk.wallpaper.player",
                                      DISPATCH_QUEUE_SERIAL)) {
        queueKey = const_cast<void*>(static_cast<const void*>(this));
        dispatch_queue_set_specific(queue, queueKey, queueKey, nullptr);
    }

    dispatch_queue_t queue;
    // 队列身份标记：positionSec() 用它判断「当前是否已在 queue 上」，避免
    // dispatch_sync 自死锁。
    void* queueKey = nullptr;
    std::atomic<bool> alive{false};
    // 装载代次：每次 load / stop 递增。后台图片解码晚到时对不上号就丢弃（审计
    // F6/F7：既防「stop 后图片复活」，也防「快速换片时旧图盖掉新视频」）。
    std::atomic<std::uint64_t> loadGeneration{0};

    std::mutex errorMutex;
    std::string error;

    // 播放器盒子（主队列对象）。每个控制器各自持有一个，不做进程级共享——
    // 共享会让「同一进程里两个 controller」（测试 / 探针 / 多模块实例）互相踩状态。
    DevDeskMacPlayerBox* box = nil;

    void setError(const std::string& message) {
        std::lock_guard<std::mutex> lock(errorMutex);
        error = message;
    }

    // 取盒子（幂等创建）。**只在 queue 上调用**。
    DevDeskMacPlayerBox* ensureBox() {
        if (box == nil) {
            box = [[DevDeskMacPlayerBox alloc] init];
            box.desiredRate = 1.0f;
            box.volume = 1.0f;
            box.muted = YES;  // 静音缺省（需求 B2）
            box.fit = static_cast<int>(ImageFit::Fill);
            box.imageItem = NO;
        }
        return box;
    }

    // 清空视频队列。三步顺序不可换：先解绑 looper（空队列上的 looper 会立刻报错）
    // → 置空（它是 AVQueuePlayer 的拷贝源，留着会让下次 disableLooping 作用在已拆
    // 掉的队列上）→ 清空队列。主队列内调用。
    void clearVideoQueue() {
        if (box == nil) return;
        [box.looper disableLooping];
        box.looper = nil;
        [box.player removeAllItems];
    }
};

MpvController::MpvController() : impl_(std::make_shared<Impl>()) {}

MpvController::~MpvController() { stop(); }

void MpvController::start(std::uintptr_t windowId) {
    if (!impl_) return;
    auto impl = impl_;  // 块按值捕 shared_ptr：Impl 与「块是否在飞」解绑
    if (windowId == 0) {
        impl->setError("壁纸宿主尚未就绪，无法挂载播放画面");
        return;
    }
    DevDeskWallpaperHostView* host =
        (__bridge DevDeskWallpaperHostView*)(void*)windowId;
    dispatch_async(impl->queue, ^{
        @autoreleasepool {
            DevDeskMacPlayerBox* box = impl->ensureBox();
            box.host = host;
            // 幂等：重复 start 不重建播放器（否则每次换屏都丢进度）。
            if (box.player == nil) {
                box.player = [[AVQueuePlayer alloc] init];
                box.player.muted = box.muted;
                box.player.volume = box.volume;
            }
            if (host.videoLayer != nil && host.videoLayer.player != box.player) {
                host.videoLayer.player = box.player;
            }
            impl->alive.store(true, std::memory_order_relaxed);
        }
    });
}

void MpvController::stop() {
    if (!impl_) return;
    auto impl = impl_;  // 块按值捕 shared_ptr：析构后晚到的块也安全（审计 F6）
    // 先抬代次再投停止块：在飞图片解码的贴图块对不上号 → 丢弃（不复活、不盖层）。
    impl->loadGeneration.fetch_add(1, std::memory_order_relaxed);
    // stop 要「返回时已停」的确切语义：同步投递 + 有界等待（主队列真被堵死也
    // 不能挂死调用方）。
    auto done = std::make_shared<bool>(false);
    auto mutex = std::make_shared<std::mutex>();
    auto cv = std::make_shared<std::condition_variable>();
    dispatch_async(impl->queue, ^{
        @autoreleasepool {
            DevDeskMacPlayerBox* box = impl->box;
            if (box != nil) {
                // 不调 pause()：队列空了自然停，播放控制是 pause() 的唯一入口。
                impl->clearVideoQueue();
                if (box.host.imageLayer != nil) {
                    [box.host.imageLayer setImage:nullptr
                                              fit:static_cast<ImageFit>(box.fit)];
                }
                box.imageItem = NO;
            }
            impl->alive.store(false, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lock(*mutex);
                *done = true;
            }
            cv->notify_all();
        }
    });
    std::unique_lock<std::mutex> lock(*mutex);
    cv->wait_for(lock, std::chrono::milliseconds(3000), [&] { return *done; });
}

void MpvController::load(const std::string& path, float seekSeconds) {
    if (!impl_ || path.empty()) return;
    auto impl = impl_;  // 块按值捕 shared_ptr：Impl 与「块是否在飞」解绑
    const bool image = isImagePath(path);
    // 每次装载新代次：只让最新一次 load 生效（旧图解码晚到 / 旧视频块晚到一律丢弃）。
    const std::uint64_t generation =
        impl->loadGeneration.fetch_add(1, std::memory_order_relaxed) + 1;

    if (!image) {
        // 必须**拷贝**路径再进 block：path 是调用方的引用参数，block 异步执行时
        // 该引用早已失效（实测退化成空串 → "无法识别的文件路径: "）。
        auto pathCopy = std::make_shared<std::string>(path);
        // block 隐式按值捕获 impl/pathCopy/generation/seekSeconds（均为局部量，
        // 不捕 this）。带捕获的 C++ lambda 不能转 dispatch_block_t，必须 ^{}。
        dispatch_async(impl->queue, ^{
            @autoreleasepool {
                if (generation != impl->loadGeneration.load(std::memory_order_relaxed)) {
                    return;  // 已被更新的 load/stop 取代
                }
                DevDeskMacPlayerBox* box = impl->ensureBox();
                if (box.player == nil) {
                    box.player = [[AVQueuePlayer alloc] init];
                }
                NSURL* url = fileURLFromUtf8Path(*pathCopy);
                if (url == nil) {
                    impl->setError("无法识别的文件路径: " + *pathCopy);
                    return;
                }
                AVPlayerItem* item = [AVPlayerItem playerItemWithURL:url];
                // 保音高必须显式设：macOS 12+ 默认 timeDomain 会变调。
                item.audioTimePitchAlgorithm = AVAudioTimePitchAlgorithmSpectral;
                // 定值 3.0（沿用已验证实现）。与上面一条同理，必须在 looper 之前设：
                // looper 的克隆体不继承 item 属性，晚设只对本次生效。
                item.preferredForwardBufferDuration = 3.0;

                // 先插后扫：队列全程非空，避免 looper 异步补位期间闪屏。
                [box.looper disableLooping];
                [box.player insertItem:item afterItem:nil];
                for (AVPlayerItem* existing in box.player.items) {
                    if (existing != item) [box.player removeItem:existing];
                }
                box.looper = [AVPlayerLooper playerLooperWithPlayer:box.player
                                                       templateItem:item];
                box.imageItem = NO;
                if (box.host.imageLayer != nil) {
                    [box.host.imageLayer setImage:nullptr
                                              fit:static_cast<ImageFit>(box.fit)];
                    box.host.imageLayer.hidden = YES;
                }
                if (box.host.videoLayer != nil) box.host.videoLayer.hidden = NO;

                box.player.volume = box.volume;
                box.player.muted = box.muted;
                // 速度挂在 player 上（不是 item）：looper 的模板 item 属性 init 时
                // 冻结，挂 item 会让「改设置立即生效」变成假的。
                box.player.rate = box.desiredRate;
                if (seekSeconds >= 0.0f) {
                    [box.player seekToTime:CMTimeMakeWithSeconds(seekSeconds, 600)
                           toleranceBefore:kCMTimeZero
                            toleranceAfter:kCMTimeZero];
                }
                impl->alive.store(true, std::memory_order_relaxed);
            }
        });
        return;
    }

    // 图片：后台解码 → 回主队列贴图。解码走独立后台队列，避免阻塞同一条串行队列
    // 上的播放控制命令。
    auto pathCopy = std::make_shared<std::string>(path);
    // 两个 block 都隐式按值捕获局部量（impl/pathCopy/generation/decoded），
    // 不捕 this；带捕获的 C++ lambda 不能转 dispatch_block_t，必须 ^{}。
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        CGImageRef decoded = decodeImage(*pathCopy);
        dispatch_async(impl->queue, ^{
            @autoreleasepool {
                // 代次对不上 = 这张图已过时（stop / 更新的 load 已发生）：丢弃，
                // 绝不贴层（否则视频播着播着被旧图翻回图片层，或 stop 后复活）。
                if (generation != impl->loadGeneration.load(std::memory_order_relaxed)) {
                    if (decoded != nullptr) CGImageRelease(decoded);
                    return;
                }
                DevDeskMacPlayerBox* box = impl->ensureBox();
                if (decoded == nullptr) {
                    impl->setError("图片解码失败（已跳过）: " + *pathCopy);
                    return;
                }
                // 图片项：视频队列清空并隐藏视频层，图片层点亮。
                impl->clearVideoQueue();
                box.imageItem = YES;
                if (box.host.imageLayer != nil) {
                    [box.host.imageLayer setImage:decoded
                                              fit:static_cast<ImageFit>(box.fit)];
                    box.host.imageLayer.hidden = NO;
                }
                CGImageRelease(decoded);
                if (box.host.videoLayer != nil) box.host.videoLayer.hidden = YES;
                impl->alive.store(true, std::memory_order_relaxed);
            }
        });
    });
}

void MpvController::pause(bool paused) {
    if (!impl_) return;
    auto impl = impl_;  // 块按值捕 shared_ptr：Impl 与「块是否在飞」解绑
    dispatch_async(impl->queue, ^{
        @autoreleasepool {
            DevDeskMacPlayerBox* box = impl->ensureBox();
            // 图片项没有播放语义：pause 必须是安全 no-op（服务层对图片项不查
            // position，但 pause 会被无条件调用）。
            if (box.imageItem || box.player == nil) return;
            if (paused) {
                [box.player pause];
            } else {
                // 恢复必须回放用户设定的速度，不能调 play()（它把 rate 顶回 1.0）。
                box.player.rate = box.desiredRate;
            }
        }
    });
}

void MpvController::seek(float seconds) {
    if (!impl_) return;
    auto impl = impl_;  // 块按值捕 shared_ptr：Impl 与「块是否在飞」解绑
    dispatch_async(impl->queue, ^{
        @autoreleasepool {
            DevDeskMacPlayerBox* box = impl->ensureBox();
            if (box.imageItem || box.player == nil) return;  // 静图无时间轴
            [box.player seekToTime:CMTimeMakeWithSeconds(seconds, 600)
                   toleranceBefore:kCMTimeZero
                    toleranceAfter:kCMTimeZero];
        }
    });
}

void MpvController::setRate(float rate) {
    if (!impl_) return;
    auto impl = impl_;  // 块按值捕 shared_ptr：Impl 与「块是否在飞」解绑
    dispatch_async(impl->queue, ^{
        @autoreleasepool {
            DevDeskMacPlayerBox* box = impl->ensureBox();
            // 只记住「用户想要的速度」，不总去改 player.rate：让路暂停期间改速度若
            // 直接写 rate 会把已暂停的播放器重新拉起，那是绕过仲裁器。
            const bool wasPlaying = box.player != nil && box.player.rate > 0.0f;
            box.desiredRate = rate;
            if (wasPlaying && !box.imageItem) box.player.rate = rate;
        }
    });
}

void MpvController::setVolume(float volume) {
    if (!impl_) return;
    auto impl = impl_;  // 块按值捕 shared_ptr：Impl 与「块是否在飞」解绑
    dispatch_async(impl->queue, ^{
        @autoreleasepool {
            DevDeskMacPlayerBox* box = impl->ensureBox();
            box.volume = volume;
            if (box.player != nil) box.player.volume = volume;
        }
    });
}

void MpvController::setMute(bool muted) {
    if (!impl_) return;
    auto impl = impl_;  // 块按值捕 shared_ptr：Impl 与「块是否在飞」解绑
    dispatch_async(impl->queue, ^{
        @autoreleasepool {
            DevDeskMacPlayerBox* box = impl->ensureBox();
            box.muted = muted;
            if (box.player != nil) box.player.muted = muted;
        }
    });
}

void MpvController::setImageFit(ImageFit fit) {
    if (!impl_) return;
    auto impl = impl_;  // 块按值捕 shared_ptr：Impl 与「块是否在飞」解绑
    dispatch_async(impl->queue, ^{
        @autoreleasepool {
            DevDeskMacPlayerBox* box = impl->ensureBox();
            box.fit = static_cast<int>(fit);
            // 只翻铺法、保留已解好的图：语义是「同一张图换个摆法」，重解码会让
            // 主线程卡一帧。
            if (box.host.imageLayer != nil) [box.host.imageLayer setFit:fit];
        }
    });
}

float MpvController::positionSec() const {
    if (!impl_) return 0.0f;
    auto impl = impl_;  // 块按值捕 shared_ptr：Impl 与「块是否在飞」解绑
    // 直接问 AVPlayer：currentTime 是线程安全的读，不需要镜像/定时器。
    // 但 box/player 只在 queue 上读写，所以经同步投递拿值（自带「已在 queue 上」
    // 判定，避免自死锁）。静图项没有时间轴，恒 0。
    __block float seconds = 0.0f;
    const auto read = ^{
        @autoreleasepool {
            DevDeskMacPlayerBox* box = impl->box;
            if (box == nil || box.player == nil || box.imageItem) return;
            const double value = CMTimeGetSeconds([box.player currentTime]);
            if (std::isfinite(value) && value > 0.0) seconds = (float)value;
        }
    };
    if (dispatch_get_specific(impl->queueKey) != nullptr) {
        read();
    } else {
        dispatch_sync(impl->queue, read);
    }
    return seconds;
}

bool MpvController::processAlive() const {
    if (!impl_) return false;
    return impl_->alive.load(std::memory_order_relaxed);
}

std::uint32_t MpvController::processId() const {
    // macOS 路线没有独立播放器进程：0 = 无子进程（探针据此区分两条路线）。
    return 0;
}

std::string MpvController::takeError() {
    if (!impl_) return std::string();
    std::lock_guard<std::mutex> lock(impl_->errorMutex);
    std::string out = impl_->error;
    impl_->error.clear();
    return out;
}

} // namespace module::wallpaper
