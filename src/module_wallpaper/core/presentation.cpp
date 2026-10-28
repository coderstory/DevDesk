#include "module_wallpaper/core/presentation.h"

#include <cmath>
#include <cstdio>

#include "module_wallpaper/core/rotation.h"

namespace module::wallpaper::presentation {

namespace {

constexpr float kRateMin = 0.5f;
constexpr float kRateMax = 2.0f;

std::string formatCountdown(int secondsUntilRotation) {
    const int total = secondsUntilRotation > 0 ? secondsUntilRotation : 0;
    const int minutes = total / 60;
    const int seconds = total % 60;
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d", minutes, seconds);
    return std::string(buffer);
}

} // namespace

std::string playModeLabel(PlayMode mode, WallpaperKind kind) {
    if (kind == WallpaperKind::Video) {
        switch (mode) {
            case PlayMode::LoopSingle: return "单循环";
            case PlayMode::LoopList: return "列表循环";
            case PlayMode::Shuffle: return "随机";
        }
        return "单循环";
    }
    switch (mode) {
        case PlayMode::LoopSingle: return "单张不变";
        case PlayMode::LoopList: return "顺序轮播";
        case PlayMode::Shuffle: return "随机轮播";
    }
    return "单张不变";
}

std::string rotationIntervalLabel(int seconds) {
    const int snapped = snapRotationIntervalSec(seconds);
    const int minutes = snapped / 60;
    if (minutes >= 60) return std::to_string(minutes / 60) + " 小时";
    return std::to_string(minutes) + " 分钟";
}

std::string rateLabel(float rate) {
    if (!std::isfinite(rate)) rate = 1.0f;
    if (rate < kRateMin) rate = kRateMin;
    if (rate > kRateMax) rate = kRateMax;
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.2f×", static_cast<double>(rate));
    return std::string(buffer);
}

std::string volumeLabel(float volume) {
    if (!std::isfinite(volume)) volume = 1.0f;
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    const long percent = std::lround(static_cast<double>(volume) * 100.0);
    return std::to_string(percent) + "%";
}

std::string countdownLabel(WallpaperKind kind, PlayMode mode, bool paused,
                           int secondsUntilRotation) {
    if (mode == PlayMode::LoopSingle || secondsUntilRotation < 0) return "不轮换";
    if (paused) return "等待恢复";
    const std::string prefix = kind == WallpaperKind::Image ? "距下次换图" : "距下次换片";
    return prefix + " " + formatCountdown(secondsUntilRotation);
}

std::string holdReasonsLabel(HoldReasonSet systemHolds) {
    std::string joined;
    for (HoldReason reason : kSystemHoldReasons) {
        if (!holdsReason(systemHolds, reason)) continue;
        if (!joined.empty()) joined += "、";
        switch (reason) {
            case HoldReason::Fullscreen: joined += "检测到全屏/最大化窗口"; break;
            case HoldReason::ScreenLocked: joined += "屏幕已锁定"; break;
            case HoldReason::DisplayAsleep: joined += "显示器已熄屏"; break;
            case HoldReason::SystemSleeping: joined += "系统正在睡眠"; break;
            case HoldReason::Battery: joined += "电池供电中"; break;
            case HoldReason::ManualPause: break;  // 系统原因文案里不出现手动暂停
        }
    }
    return joined;
}

std::string statusTitle(const StatusInput& input) {
    if (input.availability != Availability::Playing) {
        const auto empty = emptyStateCopy(input.availability, input.kind);
        return empty.has_value() ? empty->title : std::string();
    }
    if (input.systemHolds != 0) {
        return "已暂停 · " + holdReasonsLabel(input.systemHolds);
    }
    if (input.manualPaused || input.paused) return "已暂停";
    // 混合池当前项为静态图片：pause/seek 语义不适用，标题显式点名（§7.7 仍不出现文件名）。
    if (input.kind == WallpaperKind::Video && input.currentItemImage) {
        return "正在显示 · 静态图片";
    }
    const std::string mode = playModeLabel(input.mode, input.kind);
    if (input.kind == WallpaperKind::Video) return "正在播放 · " + mode;
    if (input.mode == PlayMode::LoopSingle) return "正在显示 · " + mode;
    return "正在轮播 · " + mode;
}

std::string statusSubtitle(const StatusInput& input) {
    if (input.availability != Availability::Playing) {
        const auto empty = emptyStateCopy(input.availability, input.kind);
        return empty.has_value() ? empty->reason : std::string();
    }
    if (input.systemHolds != 0) return "条件解除后自动续播，不会从头开始。";
    if (input.manualPaused || input.paused) return "点『继续』恢复播放。";

    // 播放态概览副题（§7.2）：12 个视频 · 每 10 分钟换一个。
    // 计数段按池构成分派：全视频「N 个视频」/ 全图片「N 张图片」/ 混合「V 个视频 · M 张图片」。
    const int count = input.itemCount > 0 ? input.itemCount : 0;
    // Image 来源全为图片；Video 来源按 imageItemCount（缺省 0 = 纯视频，保持旧文案）。
    const int images = input.imageItemCount > 0
                           ? input.imageItemCount
                           : (input.kind == WallpaperKind::Image ? count : 0);
    const bool hasVideo = count > images;  // imageItemCount 越界时按全图片收敛
    std::string countPart;
    if (images <= 0) {
        countPart = std::to_string(count) + " 个视频";
    } else if (!hasVideo) {
        countPart = std::to_string(count) + " 张图片";
    } else {
        countPart = std::to_string(count - images) + " 个视频 · " +
                    std::to_string(images) + " 张图片";
    }
    if (input.mode == PlayMode::LoopSingle) {
        return countPart + (hasVideo ? " · 单循环播放" : " · 单张不变");
    }
    if (count <= 1) {
        return countPart + " · 不轮换";
    }
    const std::string every = rotationIntervalLabel(input.rotationIntervalSec);
    if (!hasVideo) {
        return countPart + " · 每 " + every + "换一张";
    }
    return countPart + " · 每 " + every + "换一个";
}

std::optional<EmptyStateCopy> emptyStateCopy(Availability availability, WallpaperKind kind) {
    switch (availability) {
        case Availability::FolderUnconfigured:
            if (kind == WallpaperKind::Image) {
                return EmptyStateCopy{
                    "还没有设置图片文件夹", "选一个文件夹，里面支持的图片会进入轮播池。",
                    "选择文件夹…", "重新扫描"};
            }
            return EmptyStateCopy{
                "还没有设置壁纸文件夹", "选一个文件夹，里面支持的视频会进入轮换池。",
                "选择文件夹…", "重新扫描"};
        case Availability::FolderMissing:
            return EmptyStateCopy{
                "壁纸文件夹找不到了",
                "上次选的位置现在不存在或读不了。可能被改名、被移动，或在没挂载的磁盘上。",
                "重新选择…", "重新扫描"};
        case Availability::NoPlayableVideos:
            if (kind == WallpaperKind::Image) {
                // 本批不解码、不按像素档过滤：只按扩展名判定，不得宣称档位筛选已生效。
                return EmptyStateCopy{
                    "没有符合条件的图片",
                    "支持的格式是 jpg / jpeg / png / webp / heic；分辨率筛选待后续版本接入。",
                    "更换文件夹", "重新扫描"};
            }
            return EmptyStateCopy{
                "这个文件夹里没有能播的视频",
                "支持的格式是 mp4 / mov / m4v；其他格式不会被扫描。",
                "更换文件夹", "重新扫描"};
        case Availability::Playing:
            return std::nullopt;
    }
    return std::nullopt;
}

std::string libraryCountLabel(WallpaperKind kind, int count) {
    if (count < 0) count = 0;
    if (kind == WallpaperKind::Image) return std::to_string(count) + " 张可用图片";
    return std::to_string(count) + " 个视频";
}

} // namespace module::wallpaper::presentation