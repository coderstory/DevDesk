#include "module_wallpaper/core/video_library.h"

#include <algorithm>
#include <cctype>
#include <system_error>

#include "core/logging.h"

namespace module::wallpaper {

namespace {

constexpr const char* kVideoExtensions[] = {"mp4", "mov", "m4v"};

bool isHiddenName(const std::filesystem::path& path) {
    const std::string name = path.filename().string();
    return !name.empty() && name.front() == '.';
}

std::string lowerExtension(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    if (!extension.empty() && extension.front() == '.') extension.erase(extension.begin());
    for (char& ch : extension) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return extension;
}

bool isAllowedExtension(const std::filesystem::path& path) {
    const std::string extension = lowerExtension(path);
    for (const char* allowed : kVideoExtensions) {
        if (extension == allowed) return true;
    }
    return false;
}

// candidate 是否位于 root 之下（逐组件比较；两者都已 canonical 化）。
bool isWithinRoot(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    auto rootIt = root.begin();
    auto candidateIt = candidate.begin();
    for (; rootIt != root.end(); ++rootIt, ++candidateIt) {
        if (candidateIt == candidate.end()) return false;
        if (*rootIt != *candidateIt) return false;
    }
    return true;
}

std::filesystem::path canonicalOrEmpty(const std::filesystem::path& path,
                                       std::error_code& ec) {
    std::filesystem::path result = std::filesystem::weakly_canonical(path, ec);
    if (ec) return {};
    return result;
}

} // namespace

VideoScanResult scanVideoLibrary(const std::filesystem::path& folder, std::size_t entryCap,
                                 const std::function<bool()>& shouldCancel) {
    VideoScanResult result;
    if (folder.empty()) return result;

    std::error_code ec;
    const std::filesystem::file_status folderStatus = std::filesystem::status(folder, ec);
    if (ec || !std::filesystem::exists(folderStatus)) {
        result.status = LibraryScanStatus::Missing;
        return result;
    }
    if (!std::filesystem::is_directory(folderStatus)) {
        result.status = LibraryScanStatus::Unreadable;
        return result;
    }

    std::filesystem::path root = canonicalOrEmpty(folder, ec);
    if (ec || root.empty()) {
        result.status = LibraryScanStatus::Unreadable;
        return result;
    }
    result.status = LibraryScanStatus::Ok;

    std::error_code iterateEc;
    for (std::filesystem::recursive_directory_iterator it(
             folder, std::filesystem::directory_options::skip_permission_denied, iterateEc),
         end;
         !iterateEc && it != end; it.increment(iterateEc)) {
        if (shouldCancel && shouldCancel()) return result;

        ++result.scannedEntries;
        if (result.scannedEntries > entryCap) {
            result.hitEntryCap = true;
            break;
        }

        const std::filesystem::path path = it->path();
        std::error_code entryEc;
        const std::filesystem::file_status symlinkStatus = it->symlink_status(entryEc);
        if (entryEc) continue;
        if (std::filesystem::is_symlink(symlinkStatus)) {
            ++result.skippedSymlinks;
            it.disable_recursion_pending();
            continue;
        }
        if (isHiddenName(path)) {
            ++result.skippedHidden;
            it.disable_recursion_pending();
            continue;
        }
        if (std::filesystem::is_directory(symlinkStatus)) continue;
        if (!std::filesystem::is_regular_file(symlinkStatus)) continue;

        if (!isAllowedExtension(path)) {
            ++result.extensionRejected;
            continue;
        }

        std::error_code canonicalEc;
        const std::filesystem::path resolved = canonicalOrEmpty(path, canonicalEc);
        if (canonicalEc || resolved.empty() || !isWithinRoot(root, resolved)) {
            ++result.excludedByContainment;
            continue;
        }
        result.files.push_back(path.string());
    }

    if (iterateEc) {
        // 目录中途读不了时不能假装扫描完整：清单可能只是部分结果。
        LOG_WARN("壁纸视频扫描中断（" + folder.string() + "）: " + iterateEc.message());
    }
    std::sort(result.files.begin(), result.files.end());
    return result;
}

} // namespace module::wallpaper