#include "module_wallpaper/core/library_scan.h"

#include <algorithm>
#include <cctype>
#include <system_error>
#include <utility>

#include "core/logging.h"
#include "core/paths.h"

namespace module::wallpaper::library_scan {

namespace {

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

bool isAllowedExtension(const std::filesystem::path& path,
                        const std::vector<std::string>& allowed) {
    const std::string extension = lowerExtension(path);
    return std::find(allowed.begin(), allowed.end(), extension) != allowed.end();
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

ScanCore scanLibraryFolder(const std::filesystem::path& folder,
                           const std::vector<std::string>& extensions,
                           std::size_t entryCap,
                           const std::function<bool()>& shouldCancel,
                           const char* logLabel) {
    ScanCore result;
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

        if (!isAllowedExtension(path, extensions)) {
            ++result.extensionRejected;
            continue;
        }

        std::error_code canonicalEc;
        const std::filesystem::path resolved = canonicalOrEmpty(path, canonicalEc);
        if (canonicalEc || resolved.empty() || !isWithinRoot(root, resolved)) {
            ++result.excludedByContainment;
            continue;
        }
        // 清单最终喂 mpv JSON 命令（要求 UTF-8）；path::string() 在 Windows 按 ACP 转换，
        // 非 ASCII 文件名（中文壁纸目录）会变成 mpv 打不开的乱码路径（bug362）
        result.files.push_back(core::paths::toUtf8(path));
    }

    if (iterateEc) {
        // 目录中途读不了时不能假装扫描完整：统计只是部分结果。
        LOG_WARN("壁纸" + std::string(logLabel) + "扫描中断（" + folder.string() +
                 "）: " + iterateEc.message());
    }
    std::sort(result.files.begin(), result.files.end());
    return result;
}

} // namespace module::wallpaper::library_scan
