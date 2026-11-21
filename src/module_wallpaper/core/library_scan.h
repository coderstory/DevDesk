// src/module_wallpaper/core/library_scan.h
// 图片/视频片库公共扫描主循环（need:380 批C item5：image/video 双副本收敛）。
// 递归遍历、扩展名白名单（大小写不敏感）、隐藏/符号链接跳过、根包含性校验、入口上限；
// 取消语义/统计口径逐字段等价原两副本（契约见 image_library.h / video_library.h）。
#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "module_wallpaper/core/availability.h"

namespace module::wallpaper::library_scan {

// 两库共有的扫描统计与结果（ImageScanResult/VideoScanResult 的公共前缀）。
struct ScanCore {
    LibraryScanStatus status = LibraryScanStatus::Missing;
    std::vector<std::string> files;  // 绝对路径，字典序（UTF-8，bug362）
    std::size_t scannedEntries = 0;
    std::size_t extensionRejected = 0;
    std::size_t skippedHidden = 0;
    std::size_t skippedSymlinks = 0;
    std::size_t excludedByContainment = 0;
    bool hitEntryCap = false;
};

// logLabel 用于中断 WARN（"壁纸<logLabel>扫描中断"）；extensions 为小写白名单。
ScanCore scanLibraryFolder(const std::filesystem::path& folder,
                           const std::vector<std::string>& extensions,
                           std::size_t entryCap,
                           const std::function<bool()>& shouldCancel,
                           const char* logLabel);

} // namespace module::wallpaper::library_scan
