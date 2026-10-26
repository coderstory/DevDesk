// src/module_wallpaper/core/settings.h
// 壁纸设置持久化：路径 core::paths::appDataDir()/"wallpaper"/"settings.json"，
// 键名/默认值严格照需求 §8；损坏/越界安全（不崩、不抛）。
//
// 契约：
//   - 读写均为同步小文件；写用 core::atomicWriteFile（临时文件 + 覆盖式替换）。
//   - 未知枚举字符串 / 缺键回默认；数值越界 clamp；间隔与分辨率档吸附到允许档；
//     路径空串 = 未配置；JSON 损坏按默认值 + 日志。
//   - last* 是运行时续播字段，本批只做读写透传，不参与 sanitize 之外的业务。
#pragma once

#include <filesystem>
#include <string>

#include "core/result.h"
#include "module_wallpaper/core/model.h"

namespace module::wallpaper {

struct WallpaperSettings {
    WallpaperKind kind = WallpaperKind::Video;
    std::string sourceFolder;     // 视频目录（绝对路径，空 = 未配置）
    std::string imageFolderPath;  // 图片目录（绝对路径，空 = 未配置）
    float rate = 1.0f;            // 0.5–2.0
    float volume = 1.0f;          // 0–1
    bool muted = false;
    PlayMode mode = PlayMode::LoopSingle;
    int rotationIntervalSec = 300;  // UI 档位 5/10/15/30/60/120 分钟
    bool pauseOnBattery = false;
    int imageMinPixels = 3686400;  // 1080P=2073600 / 2K=3686400 / 4K=8294400
    ImageFit imageFit = ImageFit::Fill;
    std::string lastPlayedPath;     // 运行时：单循环续播
    double lastPlayedPosition = 0;  // 秒
    std::string lastImagePath;      // 运行时：单张图片续播
};

WallpaperSettings defaultWallpaperSettings();

// 越界收敛与未知枚举回落都在这里；load 完成后必然再过一次。
WallpaperSettings sanitizeWallpaperSettings(const WallpaperSettings& settings);

// 当前设置文件的默认路径（调用时读取 appDataDir，测试可用 DEVDESK_DATA_ROOT 隔离）。
std::filesystem::path wallpaperSettingsPath();

// 文件不存在 → 默认值（不记错误）；读取失败/JSON 损坏 → 默认值 + LOG_WARN。
WallpaperSettings loadWallpaperSettings(const std::filesystem::path& path);

// 原子写；父目录不存在时自动创建。失败返回 Result，不抛异常。
core::Result<void> saveWallpaperSettings(const std::filesystem::path& path,
                                         const WallpaperSettings& settings);

// 按来源取当前目录（空串 = 未配置）。
std::string folderForKind(const WallpaperSettings& settings, WallpaperKind kind);

} // namespace module::wallpaper