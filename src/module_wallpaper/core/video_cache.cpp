#include "module_wallpaper/core/video_cache.h"

#include <cctype>
#include <filesystem>
#include <system_error>

namespace module::wallpaper {

namespace {

std::string lowerExtension(const std::string& path) {
    std::string extension = std::filesystem::path(path).extension().string();
    if (!extension.empty() && extension.front() == '.') extension.erase(extension.begin());
    for (char& ch : extension) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return extension;
}

} // namespace

VideoMemCacheDecision decideVideoMemCache(bool enabled, bool isVideoItem,
                                          bool fileSizeKnown,
                                          std::uint64_t fileBytes) {
    VideoMemCacheDecision decision;
    decision.fileBytes = fileSizeKnown ? fileBytes : 0;
    if (!enabled) {
        decision.reason = "开关关闭";
        return decision;
    }
    if (!isVideoItem) {
        decision.reason = "非视频条目";
        return decision;
    }
    if (!fileSizeKnown) {
        decision.reason = "无法读取文件大小";
        return decision;
    }
    if (fileBytes > kVideoMemCacheLimitBytes) {
        decision.reason = "文件超过 512MB 上限";
        return decision;
    }
    decision.useMemory = true;
    return decision;
}

bool queryFileSizeBytes(const std::string& path, std::uint64_t* fileBytes) {
    if (path.empty()) return false;
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error || size == static_cast<std::uintmax_t>(-1)) return false;
    if (fileBytes != nullptr) *fileBytes = static_cast<std::uint64_t>(size);
    return true;
}

bool isVideoFilePath(const std::string& path) {
    const std::string extension = lowerExtension(path);
    return extension == "mp4" || extension == "mov" || extension == "m4v";
}

} // namespace module::wallpaper
