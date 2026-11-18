// src/module_wallpaper/platform/video_mem_loader.h
// macOS 内存喂数纯逻辑（need:311 / clue#309）：contentType（UTI）映射与资源
// 加载器请求区间夹取。零 ObjC 依赖，跨平台单测；AVAssetResourceLoaderDelegate
// 实现（mpv_controller_macos.mm）只消费这里的结果。
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <string>

namespace module::wallpaper::video_mem_loader {

// 自定义 URL scheme：AVURLAsset 命中非 file scheme 才会交给 resource loader。
inline constexpr const char* kMemoryScheme = "devdesk-mem";

// AVAssetResourceLoadingContentInformationRequest.contentType（UTI 字符串）。
// mp4/m4v 同为 MPEG-4 家族；mov 是 QuickTime 容器 UTI。
inline std::string contentTypeForPath(const std::string& path) {
    std::string extension = std::filesystem::path(path).extension().string();
    if (!extension.empty() && extension.front() == '.') extension.erase(extension.begin());
    for (char& ch : extension) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (extension == "mov") return "com.apple.quicktime-movie";
    if (extension == "mp4" || extension == "m4v") return "public.mpeg-4";
    return "public.movie";
}

struct ByteRange {
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
};

// 把（requestedOffset, requestedLength, requestsAllDataToEndOfResource）
// 夹到 [0,totalBytes]；越界 offset 收敛到 EOF、长度 0。
inline ByteRange clampRange(std::uint64_t totalBytes, std::uint64_t requestedOffset,
                            std::uint64_t requestedLength, bool toEndOfResource) {
    ByteRange range;
    range.offset = std::min(requestedOffset, totalBytes);
    const std::uint64_t remaining = totalBytes - range.offset;
    range.length = toEndOfResource ? remaining : std::min(requestedLength, remaining);
    return range;
}

} // namespace module::wallpaper::video_mem_loader
