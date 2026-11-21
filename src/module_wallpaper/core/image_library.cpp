#include "module_wallpaper/core/image_library.h"

#include <algorithm>
#include <utility>

#include "module_wallpaper/core/library_scan.h"

namespace module::wallpaper {

ImageScanResult scanImageLibrary(const std::filesystem::path& folder, int minPixels,
                                 std::size_t entryCap,
                                 const std::function<bool()>& shouldCancel,
                                 const ImageSizeProbe& sizeProbe) {
    ImageScanResult result;
    const library_scan::ScanCore scan = library_scan::scanLibraryFolder(
        folder, {"jpg", "jpeg", "png", "webp", "heic"}, entryCap, shouldCancel, "图片");
    result.status = scan.status;
    result.files = scan.files;
    result.scannedEntries = scan.scannedEntries;
    result.extensionRejected = scan.extensionRejected;
    result.skippedHidden = scan.skippedHidden;
    result.skippedSymlinks = scan.skippedSymlinks;
    result.excludedByContainment = scan.excludedByContainment;
    result.hitEntryCap = scan.hitEntryCap;
    if (scan.status != LibraryScanStatus::Ok) return result;

    result.total = static_cast<int>(scan.files.size());
    // 档位过滤（契约见 image_library.h）：unknown 不过滤；宽×高 < minPixels 记 filtered。
    // 溢出防线：宽高按 long long 相乘（8K 图约 3.3e7，int 乘法在极端头数据下无此保障）。
    if (minPixels > 0 && sizeProbe) {
        std::vector<std::string> passing;
        passing.reserve(result.files.size());
        for (const std::string& path : result.files) {
            const ProbedImageSize size = sizeProbe(path);
            const bool knownSmall = size.known &&
                                    static_cast<long long>(size.width) *
                                            static_cast<long long>(size.height) <
                                        static_cast<long long>(minPixels);
            if (!knownSmall) {
                passing.push_back(path);
            } else {
                ++result.filtered;
            }
        }
        result.files = std::move(passing);
    }
    result.passing = static_cast<int>(result.files.size());
    result.filtered = result.total - result.passing;
    return result;
}

} // namespace module::wallpaper
