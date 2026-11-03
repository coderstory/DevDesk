// src/module_wallpaper/platform/wallpaper_host_macos_view.h
// macOS 壁纸宿主的窗口/图层类型（**仅限 .mm 文件 include**，C++ TU 不可见）。
//
// 为什么要有这个头：宿主建窗、播放后端挂媒体，是两个 TU。宿主创建视频层与图片层
// 并持有它们（同一个 player 上不能挂两个 AVPlayerLayer），播放后端需要经
// `HostHandle.videoChild` 拿回这两层的类型安全视图。放到共享头里避免两处各写一份
// 强转，也避免把 ObjC 声明漏到跨平台编译单元。
#pragma once

#import <AppKit/AppKit.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <QuartzCore/QuartzCore.h>

#include "module_wallpaper/core/model.h"

// 壁纸图片层：四种铺法全部自己画（不混用 contentsGravity——tile 没有对应
// gravity，两套机制并存会让「换铺法」变成换绘制路径）。
@interface DevDeskWallpaperImageLayer : CALayer
// 赋图。解码必须由调用方在后台完成：本方法只接受已解好的 CGImage，
// 主线程不解图（一张 4K HEIC 足以卡住一帧）。
- (void)setImage:(CGImageRef)image fit:(module::wallpaper::ImageFit)fit;
// 换铺法：只翻 fit、保留 image——语义是「同一张图换个摆法」，不重解码。
- (void)setFit:(module::wallpaper::ImageFit)fit;
@end

// 壁纸宿主视图：承载视频层与图片层的容器，两层并列常驻，切换只翻 isHidden。
@interface DevDeskWallpaperHostView : NSView
@property(nonatomic, strong) AVPlayerLayer* videoLayer;
@property(nonatomic, strong) DevDeskWallpaperImageLayer* imageLayer;
@end
