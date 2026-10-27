// src/module_wallpaper/core/image_library.h
// 图片片库扫描：递归、扩展名白名单、隐藏/符号链接跳过、目录包含性校验、入口上限。
//
// **本批不做像素档过滤**：不解码图片尺寸，minPixels 参数被忽略，
// passing = total、filtered = 0。像素档过滤待 P3 解码接入，在那之前 UI 不得宣称已按分辨率筛选。
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

struct ImageScanResult {
    LibraryScanStatus status = LibraryScanStatus::Missing;
    std::vector<std::string> files;  // 通过扩展名的图片，绝对路径，字典序
    std::size_t scannedEntries = 0;
    std::size_t extensionRejected = 0;
    std::size_t skippedHidden = 0;
    std::size_t skippedSymlinks = 0;
    std::size_t excludedByContainment = 0;
    bool hitEntryCap = false;
    // 统计三格：total = 扩展名命中的图片数；passing 本批恒等于 total；filtered 恒 0。
    int total = 0;
    int passing = 0;
    int filtered = 0;
};

// minPixels 当前不参与筛选（见文件头）；保留参数是为了 P3 无需改签名。
ImageScanResult scanImageLibrary(
    const std::filesystem::path& folder, int minPixels,
    std::size_t entryCap = kLibraryEntryCap,
    const std::function<bool()>& shouldCancel = {});

} // namespace module::wallpaper