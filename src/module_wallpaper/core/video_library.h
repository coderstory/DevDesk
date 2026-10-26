// src/module_wallpaper/core/video_library.h
// 视频片库扫描：递归、扩展名白名单、隐藏/符号链接跳过、目录包含性校验、入口上限。
//
// 契约：
//   - 只认 mp4/mov/m4v（大小写不敏感）；不排除 Converted/ 目录（本轮无第二扫描器，见需求 §5.2）。
//   - 隐藏判定按「文件名以 . 开头」；Windows FILE_ATTRIBUTE_HIDDEN 未读取（P3 平台层补齐）。
//   - 符号链接一律跳过，不跟随目录链接。
//   - 返回条目按路径字典序，便于 UI/测试稳定比较；路径为绝对路径。
//   - 本批不做视频轨探测（probe）：扩展名命中即计入可播放数，P3 接入 probe 后此处会再收紧。
#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "module_wallpaper/core/availability.h"

namespace module::wallpaper {

// 递归入口上限（需求 §9.2）：超过后停止遍历并置 hitEntryCap。
inline constexpr std::size_t kLibraryEntryCap = 5000;

struct VideoScanResult {
    LibraryScanStatus status = LibraryScanStatus::Missing;
    std::vector<std::string> files;  // 绝对路径，字典序
    std::size_t scannedEntries = 0;
    std::size_t extensionRejected = 0;
    std::size_t skippedHidden = 0;
    std::size_t skippedSymlinks = 0;
    std::size_t excludedByContainment = 0;
    bool hitEntryCap = false;
};

// shouldCancel 非空时在每个目录条目处询问；返回 true 立即结束（结果由调用方按代次丢弃）。
VideoScanResult scanVideoLibrary(
    const std::filesystem::path& folder,
    std::size_t entryCap = kLibraryEntryCap,
    const std::function<bool()>& shouldCancel = {});

} // namespace module::wallpaper