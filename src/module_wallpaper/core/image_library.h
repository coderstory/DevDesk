// src/module_wallpaper/core/image_library.h
// 图片片库扫描：递归、扩展名白名单、隐藏/符号链接跳过、目录包含性校验、入口上限。
//
// 像素档过滤（P3b 落地）：尺寸探针经 ImageSizeProbe 注入（core 零平台依赖，生产由
// service 传入 platform 层的 WIC 头解码实现）。语义：
//   - minPixels <= 0：过滤关闭，探针不调用。
//   - 探针返回 known=false（解码失败/格式不支持）：不过滤（宁滥勿缺——解码能力
//     缺失不得吃掉用户轮换池）。
//   - known 且 宽×高 < minPixels：记 filtered，不进 files。
//   - total = 扩展名命中数（过滤前）；passing = files 数；filtered = total - passing。
//
// 契约：只认 jpg/jpeg/png/webp/heic（大小写不敏感；不收 gif，动图会把图片模式混回视频语义）；
// 隐藏判定按「文件名以 . 开头」；符号链接一律跳过；路径为绝对路径、字典序。
#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "module_wallpaper/core/availability.h"
#include "module_wallpaper/core/video_library.h"  // kLibraryEntryCap（同一入口上限口径）

namespace module::wallpaper {

// 头解码结果。known=false = 尺寸未知（调用方不过滤）。
struct ProbedImageSize {
    bool known = false;
    int width = 0;
    int height = 0;
};

// 尺寸探针注入缝：实现必须快速返回、不得抛异常、不得阻塞扫描线程的取消检查。
using ImageSizeProbe = std::function<ProbedImageSize(const std::string& path)>;

struct ImageScanResult {
    LibraryScanStatus status = LibraryScanStatus::Missing;
    std::vector<std::string> files;  // 通过档位过滤的图片，绝对路径，字典序
    std::size_t scannedEntries = 0;
    std::size_t extensionRejected = 0;
    std::size_t skippedHidden = 0;
    std::size_t skippedSymlinks = 0;
    std::size_t excludedByContainment = 0;
    bool hitEntryCap = false;
    // 统计三格：total = 扩展名命中的图片数（过滤前）；passing = 过滤后入池数；
    // filtered = 被档位线滤掉的数（无探针 / 关闭过滤时恒 0）。
    int total = 0;
    int passing = 0;
    int filtered = 0;
};

// probe 为空 = 不做尺寸过滤（passing == total），与 P3b 之前的行为一致。
ImageScanResult scanImageLibrary(
    const std::filesystem::path& folder, int minPixels,
    std::size_t entryCap = kLibraryEntryCap,
    const std::function<bool()>& shouldCancel = {},
    const ImageSizeProbe& sizeProbe = {});

} // namespace module::wallpaper