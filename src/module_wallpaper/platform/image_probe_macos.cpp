// src/module_wallpaper/platform/image_probe_macos.cpp
// CGImageSource 头解码：只取宽高，不解码像素。
// 失败一律返回 known=false（调用方语义 = 尺寸未知不过滤，见 image_probe.h）。
//
// 口径：用**存储像素**（kCGImagePropertyPixelWidth/Height），不套 EXIF 旋转——
// 宽高互换乘积不变，像素档判定不受 orientation 影响。显示侧摆正是解码器的事。
#include "module_wallpaper/platform/image_probe.h"

#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

#include <string>

namespace module::wallpaper {
namespace {

// 用 CoreFoundation 拼 URL：本文件是 .cpp（不需要 ObjC），避免为一个字符串转换
// 把整条探针路径拉进 ObjC++。
CFURLRef createFileURL(const std::string& utf8Path) {
    CFAllocatorRef allocator = kCFAllocatorDefault;
    CFStringRef path = CFStringCreateWithBytes(
        allocator, reinterpret_cast<const UInt8*>(utf8Path.data()),
        static_cast<CFIndex>(utf8Path.size()), kCFStringEncodingUTF8, false);
    if (path == nullptr) return nullptr;
    CFURLRef url = CFURLCreateWithFileSystemPath(allocator, path,
                                                 kCFURLPOSIXPathStyle, false);
    CFRelease(path);
    return url;
}

int propertyInt(CFDictionaryRef properties, CFStringRef key) {
    if (properties == nullptr) return 0;
    CFTypeRef value = CFDictionaryGetValue(properties, key);
    if (value == nullptr || CFGetTypeID(value) != CFNumberGetTypeID()) return 0;
    int out = 0;
    if (!CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberIntType, &out)) return 0;
    return out;
}

} // namespace

ProbedImageSize probeImageSize(const std::string& path) {
    ProbedImageSize result;
    if (path.empty()) return result;

    CFURLRef url = createFileURL(path);
    if (url == nullptr) return result;
    // 只建解码器读元数据头：kCGImageSourceShouldCache=false 保证不触发像素解码。
    const void* keys[] = {kCGImageSourceShouldCache};
    const void* values[] = {kCFBooleanFalse};
    CFDictionaryRef options = CFDictionaryCreate(
        kCFAllocatorDefault, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    CGImageSourceRef source = CGImageSourceCreateWithURL(url, options);
    if (options != nullptr) CFRelease(options);
    CFRelease(url);
    if (source == nullptr) return result;

    CFDictionaryRef properties = CGImageSourceCopyPropertiesAtIndex(source, 0, nullptr);
    if (properties != nullptr) {
        const int width = propertyInt(properties, kCGImagePropertyPixelWidth);
        const int height = propertyInt(properties, kCGImagePropertyPixelHeight);
        if (width > 0 && height > 0) {
            result.known = true;
            result.width = width;
            result.height = height;
        }
        CFRelease(properties);
    }
    CFRelease(source);
    return result;
}

} // namespace module::wallpaper
