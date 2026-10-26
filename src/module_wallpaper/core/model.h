// src/module_wallpaper/core/model.h
// 壁纸模块纯逻辑共享类型。本文件零 Win32 / EUI-NEO / platform 依赖，跨平台可单测。
#pragma once

#include <cstdint>

namespace module::wallpaper {

// 壁纸来源。切换来源 = 新一轮（清单与计时重建），见 rotation.h 的 setItems/start。
enum class WallpaperKind { Video, Image };

enum class PlayMode { LoopSingle, LoopList, Shuffle };

enum class ImageFit { Fill, Fit, Center, Tile };

// 暂停否决原因。声明顺序 = UI 原因文案排序（需求 §7.7：系统原因之间用顿号）。
// ManualPause 是运行时手动静音，**不进**快照的 systemHolds（UI 对手动暂停不显示原因）。
enum class HoldReason : std::uint8_t {
    ManualPause = 0,
    Fullscreen,
    ScreenLocked,
    DisplayAsleep,
    SystemSleeping,
    Battery,
};

// 位集合。快照里的 systemHolds 只允许出现「系统原因」位（ManualPause 位恒 0）。
using HoldReasonSet = std::uint32_t;

constexpr HoldReasonSet holdReasonBit(HoldReason reason) {
    return HoldReasonSet{1} << static_cast<std::uint8_t>(reason);
}

// 迭代顺序固定（升序 = 声明顺序），presentation 的原因拼接依赖它。
inline constexpr HoldReason kSystemHoldReasons[] = {
    HoldReason::Fullscreen,   HoldReason::ScreenLocked, HoldReason::DisplayAsleep,
    HoldReason::SystemSleeping, HoldReason::Battery,
};

constexpr HoldReasonSet kSystemHoldMask() {
    HoldReasonSet mask = 0;
    for (HoldReason reason : kSystemHoldReasons) mask |= holdReasonBit(reason);
    return mask;
}

constexpr bool holdsReason(HoldReasonSet set, HoldReason reason) {
    return (set & holdReasonBit(reason)) != 0;
}

inline void setHoldReason(HoldReasonSet& set, HoldReason reason, bool active) {
    if (active) {
        set |= holdReasonBit(reason);
    } else {
        set &= ~holdReasonBit(reason);
    }
}

// 片库四态：未配置 / 目录不在 / 目录在但 0 条可播 / 有可播。
enum class Availability { FolderUnconfigured, FolderMissing, NoPlayableVideos, Playing };

} // namespace module::wallpaper