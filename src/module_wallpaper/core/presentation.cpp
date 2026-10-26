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
    return {};
}

std::optional<EmptyStateCopy> emptyStateCopy(Availability availability, WallpaperKind kind) {
    switch (availability) {
        case Availability::FolderUnconfigured:
            if (kind == WallpaperKind::Image) {
                return EmptyStateCopy{
                    "还没选图片文件夹",
                    "DevDesk 还不知道该去哪里找图片。选一个文件夹，里面所有 JPG / PNG / HEIC / WEBP "
                    "都会进轮播池。",
                    "选择文件夹…"};
            }
            return EmptyStateCopy{
                "还没选壁纸文件夹",
                "DevDesk 还不知道该去哪里找视频。选一个文件夹，里面所有能播的视频都会进轮换池。",
                "选择文件夹…"};
        case Availability::FolderMissing:
            return EmptyStateCopy{
                "壁纸文件夹不见了",
                "上次选的位置现在不存在或读不了。可能被改名、被移动，或在没挂载的磁盘上。",
                "重新选择文件夹…"};
        case Availability::NoPlayableVideos:
            if (kind == WallpaperKind::Image) {
                return EmptyStateCopy{
                    "没有够档位的图片",
                    "文件夹能正常读取，但没有一张图片达到当前分辨率档位。把档位降到 1080P，"
                    "或换一个图片更大的文件夹。",
                    "去片库调整档位"};
            }
            return EmptyStateCopy{
                "没有能直接播的文件",
                "文件夹能正常读取，但里面 0 个能直接播的文件。MKV / AVI / WEBM 需要先转成 MP4 "
                "才能当壁纸。",
                "去片库转码"};
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