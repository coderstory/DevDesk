#include "module_wallpaper/core/video_library.h"

#include "module_wallpaper/core/library_scan.h"

namespace module::wallpaper {

VideoScanResult scanVideoLibrary(const std::filesystem::path& folder, std::size_t entryCap,
                                 const std::function<bool()>& shouldCancel) {
    const library_scan::ScanCore scan = library_scan::scanLibraryFolder(
        folder, {"mp4", "mov", "m4v"}, entryCap, shouldCancel, "视频");
    VideoScanResult result;
    result.status = scan.status;
    result.files = scan.files;
    result.scannedEntries = scan.scannedEntries;
    result.extensionRejected = scan.extensionRejected;
    result.skippedHidden = scan.skippedHidden;
    result.skippedSymlinks = scan.skippedSymlinks;
    result.excludedByContainment = scan.excludedByContainment;
    result.hitEntryCap = scan.hitEntryCap;
    return result;
}

} // namespace module::wallpaper
