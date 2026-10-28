// src/module_wallpaper/platform/image_probe_stub.cpp
// 非 Windows 尺寸头解码 stub：恒 unknown（调用方语义 = 不过滤）。
// 跨平台单测经由 ImageSizeProbe 注入假探针覆盖过滤逻辑，不依赖本实现。
#include "module_wallpaper/platform/image_probe.h"

namespace module::wallpaper {

ProbedImageSize probeImageSize(const std::string& path) {
    (void)path;
    return ProbedImageSize{};
}

} // namespace module::wallpaper
