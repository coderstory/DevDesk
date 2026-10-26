#include "module_wallpaper/core/availability.h"

namespace module::wallpaper {

Availability evaluateAvailability(bool folderConfigured, LibraryScanStatus status,
                                  int playableCount) {
    if (!folderConfigured) return Availability::FolderUnconfigured;
    if (status != LibraryScanStatus::Ok) return Availability::FolderMissing;
    if (playableCount < 1) return Availability::NoPlayableVideos;
    return Availability::Playing;
}

bool shouldShowWallpaper(Availability availability) {
    return availability == Availability::Playing;
}

} // namespace module::wallpaper