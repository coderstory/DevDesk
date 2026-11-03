// src/module_wallpaper/platform/wallpaper_host_macos.mm
// macOS 桌面层壁纸宿主：一扇铺满主屏的 borderless 窗口挂在桌面层
// （CGWindowLevelForKey(kCGDesktopWindowLevelKey)，位于系统壁纸之上、桌面图标之下），
// 窗口里并列挂着视频层（AVPlayerLayer）与图片层（自绘 CALayer）。
//
// 关键约束（改动前先读）：
//   * AppKit 窗口操作必须主线程。本实现统一 dispatch_async 到主队列再同步等待——
//     实测 DevDesk 的 GLFW 事件泵（NSDefaultRunLoopMode）会排空主队列，因此不需要
//     自建宿主线程（与 Windows 版的消息窗线程模型不同）。等待有界
//     （kHostTimeoutMs）：主队列真被堵死时按失败返回，不挂死调用方。
//   * 层级值必须取 CoreGraphics 符号：AppKit 没有对应常量，写数字字面量会盖住桌面图标。
//   * 窗口四条属性（不吃鼠标 / 不能当 key / 不能当 main / borderless）合起来保证它
//     不可能截获其它 app 的输入，也不改变焦点。
//   * 只做主显示器（需求「永久不做」清单第 3 条：多显示器不做）。
//   * 视频层/图片层由本窗口创建并持有——播放后端不另造，否则同一个 player 上会挂
//     两个 AVPlayerLayer，渲染哪一路变成不确定的事。媒体对象由播放后端持有，
//     窗口只经 HostHandle.videoChild 暴露类型安全视图。
#import <AppKit/AppKit.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <QuartzCore/QuartzCore.h>

#include "module_wallpaper/platform/image_fit_geometry.h"
#include "module_wallpaper/platform/wallpaper_host.h"
#include "module_wallpaper/platform/wallpaper_host_macos_view.h"

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>

namespace module::wallpaper {
namespace {

// 所有跨线程等待的上界：主队列真被堵死时按失败返回，不挂死服务线程。
constexpr int kHostTimeoutMs = 3000;

// 主队列同步投递：返回 false = 超时（主队列未在时限内执行到本任务）。
template <typename Fn>
bool runOnMainThreadSync(Fn&& fn) {
    if ([NSThread isMainThread]) {
        fn();
        return true;
    }
    auto done = std::make_shared<bool>(false);
    auto mutex = std::make_shared<std::mutex>();
    auto cv = std::make_shared<std::condition_variable>();
    dispatch_async(dispatch_get_main_queue(), ^{
        fn();
        {
            std::lock_guard<std::mutex> lock(*mutex);
            *done = true;
        }
        cv->notify_all();
    });
    std::unique_lock<std::mutex> lock(*mutex);
    return cv->wait_for(lock, std::chrono::milliseconds(kHostTimeoutMs),
                        [&] { return *done; });
}

} // namespace
} // namespace module::wallpaper

// 图片层：四种铺法自己画。宿主持有实例，播放后端经接口赋值。
@implementation DevDeskWallpaperImageLayer {
    CGImageRef _image;
    module::wallpaper::ImageFit _fit;
}

- (instancetype)init {
    self = [super init];
    if (self != nil) {
        _fit = module::wallpaper::ImageFit::Fill;
        self.needsDisplayOnBoundsChange = YES;
    }
    return self;
}

- (void)dealloc {
    if (_image != nullptr) CGImageRelease(_image);
}

- (void)setImage:(CGImageRef)image fit:(module::wallpaper::ImageFit)fit {
    if (_image != nullptr) CGImageRelease(_image);
    _image = (image != nullptr) ? CGImageRetain(image) : nullptr;
    _fit = fit;
    [self setNeedsDisplay];
}

- (void)setFit:(module::wallpaper::ImageFit)fit {
    _fit = fit;
    [self setNeedsDisplay];
}

- (void)drawInContext:(CGContextRef)ctx {
    if (_image == nullptr || self.bounds.size.width <= 0 || self.bounds.size.height <= 0) {
        return;
    }
    using namespace module::wallpaper::image_fit_geometry;
    const Placement result = placement(
        (double)CGImageGetWidth(_image), (double)CGImageGetHeight(_image),
        (double)self.bounds.size.width, (double)self.bounds.size.height, _fit);

    CGContextSaveGState(ctx);
    // 几何给的是**左上原点**；CG 上下文在 isGeometryFlipped == false 时原点在左下，
    // 翻一次即可直接画。不能假定宿主视图的 flipped 状态——假定错了图片会上下颠倒。
    if (!self.isGeometryFlipped) {
        CGContextTranslateCTM(ctx, 0, self.bounds.size.height);
        CGContextScaleCTM(ctx, 1, -1);
    }
    const CGRect rect = CGRectMake(result.rect.x, result.rect.y,
                                   result.rect.w, result.rect.h);
    if (result.tiles) {
        // 平铺：rect 是一块砖的尺寸、锚在左上角，由 CGContextDrawTiledImage 在
        // 裁剪区内重复铺。不能先 DrawImage 再覆盖——那会在砖缝外留一块放大的图。
        CGContextDrawTiledImage(ctx, rect, _image);
    } else {
        CGContextDrawImage(ctx, rect, _image);
    }
    CGContextRestoreGState(ctx);
}

@end

// 宿主视图：两层并列常驻，切换只翻 isHidden（反复增删子层会与 layout 打架，
// 表现为换来源后有一层停在旧尺寸上）。
@implementation DevDeskWallpaperHostView

- (BOOL)isFlipped {
    return YES;
}

- (void)layout {
    [super layout];
    self.videoLayer.frame = self.bounds;
    self.imageLayer.frame = self.bounds;
    // 不经窗口重建的 scale 变化（同屏改分辨率）也要跟屏：contentsScale 决定光栅化
    // 分辨率，不改的话图片层会停在旧 scale 上继续发虚。
    NSWindow* window = self.window;
    if (window != nil && self.imageLayer != nil &&
        self.imageLayer.contentsScale != window.backingScaleFactor) {
        self.imageLayer.contentsScale = window.backingScaleFactor;
        [self.imageLayer setNeedsDisplay];
    }
}

@end

@interface DevDeskWallpaperWindow : NSWindow
@end

@implementation DevDeskWallpaperWindow

// 这一对保证这扇窗永远不可能拿到 key/main 焦点。
- (BOOL)canBecomeKeyWindow {
    return NO;
}
- (BOOL)canBecomeMainWindow {
    return NO;
}

@end

namespace module::wallpaper {

class WallpaperHostMac final : public WallpaperHost {
public:
    ~WallpaperHostMac() override { stop(); }

    // start/rebuild 保持同步：调用方需要拿到 handle（非空才启动播放器），
    // 且这两条路径由服务在**主线程**发起（模块 onStart / 换屏回调），走
    // runOnMainThreadSync 的 isMainThread 快路径，不会发生上面那个等待环。
    bool start(std::string* error) override {
        std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (window_ != nil) return true;  // 幂等
        }
        return buildLocked(error);
    }

    void stop() override {
        std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
        DevDeskWallpaperWindow* window = nil;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            window = window_;
            window_ = nil;
            hostView_ = nil;
            videoLayer_ = nil;
            imageLayer_ = nil;
            width_ = 0;
            height_ = 0;
        }
        if (window == nil) return;
        // 收尾路径：等不到主队列也不能崩——窗口由 ARC 持有，投递即放手，不等待。
        dispatch_async(dispatch_get_main_queue(), ^{
            [window orderOut:nil];
        });
    }

    // show/hide **必须异步投递**，不得同步等主线程。
    //
    // 原因（2026-11-03 真机复现）：服务在持有 mu_ 时调用 show（applyVideoScanLocked
    // → host_->show），而主线程可能正等着拿 mu_（snapshot/setMode）。同步投递两边
    // 互等，3s 后报「显示壁纸宿主超时」。Windows 版投递到自己的宿主线程、不经过
    // 主线程，所以没有这个倒置；macOS 的窗口操作只能在主线程，只能靠不等待来消除
    // 这个环。可见性变化不需要与调用方同步——服务下一步装卸媒体与窗口可见性无关。
    bool show(std::string* error) override {
        DevDeskWallpaperWindow* window = nil;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            window = window_;
        }
        if (window == nil) {
            if (error != nullptr) *error = "壁纸宿主尚未创建";
            return false;
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            [window orderFrontRegardless];
        });
        return true;
    }

    void hide() override {
        DevDeskWallpaperWindow* window = nil;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            window = window_;
        }
        if (window == nil) return;
        dispatch_async(dispatch_get_main_queue(), ^{
            [window orderOut:nil];
        });
    }

    HostHandle handle() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        HostHandle out;
        if (window_ == nil) return out;
        // macOS 没有「子窗口句柄」概念：宿主视图就是媒体挂载点，播放后端经它拿
        // videoLayer/imageLayer。非空即代表宿主就绪、可以启动播放器。
        // void* 只作为不透明传递载体：播放后端按同一类型转回
        //（见 mpv_controller_macos.mm 的 start(windowId)）。
        out.videoChild = (void*)(__bridge void*)hostView_;
        out.imageChild = (void*)(__bridge void*)imageLayer_;
        out.width = width_;
        out.height = height_;
        return out;
    }

    void rebuild(std::string* error) override {
        std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
        DevDeskWallpaperWindow* existing = nil;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            existing = window_;
        }
        // 降级态（从未建窗 / 已 stop）不该被一次换屏唤醒——那会让「没有可播内容」
        // 的会话凭空冒出一扇黑窗。
        if (existing == nil) return;

        // 重建要保留原可见性：先记下是否在屏上，重建后按原态恢复。
        bool wasVisible = false;
        runOnMainThreadSync([existing, &wasVisible] {
            wasVisible = [existing isVisible];
            [existing orderOut:nil];
        });
        {
            std::lock_guard<std::mutex> lock(mutex_);
            window_ = nil;
            hostView_ = nil;
            videoLayer_ = nil;
            imageLayer_ = nil;
            width_ = 0;
            height_ = 0;
        }
        if (!buildLocked(error)) return;
        if (!wasVisible) return;
        DevDeskWallpaperWindow* rebuilt = nil;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            rebuilt = window_;
        }
        if (rebuilt != nil) {
            runOnMainThreadSync([rebuilt] { [rebuilt orderFrontRegardless]; });
        }
    }

private:
    // 建窗（全部在主线程完成，AppKit 是主线程隔离的）。持 lifecycleMutex_ 调用。
    bool buildLocked(std::string* error) {
        DevDeskWallpaperWindow* created = nil;
        DevDeskWallpaperHostView* hostView = nil;
        AVPlayerLayer* videoLayer = nil;
        DevDeskWallpaperImageLayer* imageLayer = nil;
        int pixelWidth = 0;
        int pixelHeight = 0;
        std::string failure;

        const bool ok = runOnMainThreadSync([&]() {
            NSScreen* screen = [NSScreen mainScreen];
            if (screen == nil) {
                failure = "找不到主显示器（无 GUI 会话？）";
                return;
            }
            const NSRect frame = screen.frame;
            if (frame.size.width <= 0 || frame.size.height <= 0) {
                failure = "主显示器尺寸无效";
                return;
            }

            DevDeskWallpaperWindow* window =
                [[DevDeskWallpaperWindow alloc] initWithContentRect:frame
                                                          styleMask:NSWindowStyleMaskBorderless
                                                            backing:NSBackingStoreBuffered
                                                              defer:NO];
            if (window == nil) {
                failure = "创建壁纸窗口失败";
                return;
            }
            // 层级取 CoreGraphics 运行时符号：AppKit 无对应常量，写数字字面量会
            // 落到桌面图标之上。
            window.level = (NSWindowLevel)CGWindowLevelForKey(kCGDesktopWindowLevelKey);
            // 系统默认 Space 行为：跨 Space 跟随、不参与切换循环、全屏时可作辅助窗口。
            window.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                                        NSWindowCollectionBehaviorStationary |
                                        NSWindowCollectionBehaviorIgnoresCycle |
                                        NSWindowCollectionBehaviorFullScreenAuxiliary;
            window.opaque = YES;
            window.hasShadow = NO;
            window.ignoresMouseEvents = YES;
            window.backgroundColor = [NSColor blackColor];

            DevDeskWallpaperHostView* host =
                [[DevDeskWallpaperHostView alloc] initWithFrame:frame];
            host.wantsLayer = YES;

            AVPlayerLayer* video = [[AVPlayerLayer alloc] init];
            video.frame = host.bounds;
            video.videoGravity = AVLayerVideoGravityResizeAspectFill;
            [host.layer addSublayer:video];

            DevDeskWallpaperImageLayer* image = [[DevDeskWallpaperImageLayer alloc] init];
            image.frame = host.bounds;
            image.hidden = YES;
            // 手动 addSublayer 挂上的子层不继承宿主视图的 contentsScale；不设的话
            // 默认 1x 光栅化再被放大到屏，Retina 上图片发虚。AVPlayerLayer 自管 scale。
            image.contentsScale = screen.backingScaleFactor;
            [host.layer addSublayer:image];

            window.contentView = host;
            host.videoLayer = video;
            host.imageLayer = image;

            // 契约：width/height 是物理像素，与 Windows 版（rcMonitor 物理像素）同口径。
            const NSRect backing = [screen convertRectToBacking:frame];
            pixelWidth = (int)llround(backing.size.width);
            pixelHeight = (int)llround(backing.size.height);

            created = window;
            hostView = host;
            videoLayer = video;
            imageLayer = image;
        });

        if (!ok) {
            if (error != nullptr) {
                *error = failure.empty() ? "创建壁纸宿主超时（主线程无响应）" : failure;
            }
            return false;
        }
        if (created == nil) {
            if (error != nullptr) *error = failure.empty() ? "创建壁纸宿主失败" : failure;
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        window_ = created;
        hostView_ = hostView;
        videoLayer_ = videoLayer;
        imageLayer_ = imageLayer;
        width_ = pixelWidth;
        height_ = pixelHeight;
        return true;
    }

    mutable std::mutex mutex_;
    std::mutex lifecycleMutex_;
    DevDeskWallpaperWindow* window_ = nil;
    DevDeskWallpaperHostView* hostView_ = nil;
    AVPlayerLayer* videoLayer_ = nil;
    DevDeskWallpaperImageLayer* imageLayer_ = nil;
    int width_ = 0;
    int height_ = 0;
};

std::unique_ptr<WallpaperHost> createWallpaperHost() {
    return std::make_unique<WallpaperHostMac>();
}

} // namespace module::wallpaper
