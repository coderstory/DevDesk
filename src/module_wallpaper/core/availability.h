// src/module_wallpaper/core/availability.h
// 片库可用性判定：扫描结果 → Availability。纯函数，无 IO。
#pragma once

#include "module_wallpaper/core/model.h"

namespace module::wallpaper {

// 片库扫描的目录状态。Ok = 目录可读；Missing = 不存在；Unreadable = 存在但不是目录/读不了。
// Missing 与 Unreadable 在 Availability 层合并为 FolderMissing（UI 同一空态）。
enum class LibraryScanStatus { Ok, Missing, Unreadable };

// folderConfigured == false 压过一切（空串 = 未配置）。
Availability evaluateAvailability(bool folderConfigured, LibraryScanStatus status, int playableCount);

// 只有 Playing 才显示壁纸；其余三态都隐藏宿主。
bool shouldShowWallpaper(Availability availability);

} // namespace module::wallpaper