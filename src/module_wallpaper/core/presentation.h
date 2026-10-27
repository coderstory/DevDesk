// src/module_wallpaper/core/presentation.h
// 壁纸模块唯一文案源：状态标题/副题、模式标签、间隔/速度/音量/倒计时、空态三变体、
// 统计单位。UI 批次只消费这些结果，不得自己拼文案。
//
// 硬规则：
//   - 任何函数都**不接收、不返回文件名/路径**（需求 §7.7：不显示文件名）。
//   - 手动暂停不显示原因；系统让路列出全部原因（顿号分隔）。
//   - 单循环 / 单张不变 = 不轮换；暂停时倒计时显示「等待恢复」。
#pragma once

#include <optional>
#include <string>

#include "module_wallpaper/core/model.h"

namespace module::wallpaper::presentation {

// 视频：单循环 / 列表循环 / 随机；图片：单张不变 / 顺序轮播 / 随机轮播。
std::string playModeLabel(PlayMode mode, WallpaperKind kind);

// 秒 → UI 间隔标签：5/10/15/30 分钟 → 「N 分钟」；60/120 → 「N 小时」。
// 非档位值先就近吸附（与 RotationController 同口径）。
std::string rotationIntervalLabel(int seconds);

// 0.5–2.0 → 「1.00×」；0–1 → 「0%–100%」。
std::string rateLabel(float rate);
std::string volumeLabel(float volume);

// 倒计时标签：不轮换 → 「不轮换」；暂停 → 「等待恢复」；
// 否则「距下次换片 mm:ss」（视频）/「距下次换图 mm:ss」（图片）。
std::string countdownLabel(WallpaperKind kind, PlayMode mode, bool paused,
                           int secondsUntilRotation);

// 系统让路原因拼接（按声明顺序、顿号分隔）；输入里的 ManualPause 位被忽略。
std::string holdReasonsLabel(HoldReasonSet systemHolds);

struct StatusInput {
    WallpaperKind kind = WallpaperKind::Video;
    Availability availability = Availability::FolderUnconfigured;
    PlayMode mode = PlayMode::LoopSingle;
    bool paused = false;        // 手动暂停或系统让路
    bool manualPaused = false;  // 手动暂停位为真即 true，可与 systemHolds 并存
    HoldReasonSet systemHolds = 0;
    int itemCount = 0;              // 播放态副题：「N 个视频 / N 张图片」
    int rotationIntervalSec = 300;  // 播放态副题：「每 N 分钟换一个」
};

// 状态卡标题。播放中：视频「正在播放 · 模式」、图片单张「正在显示 · 单张不变」、
// 图片轮播「正在轮播 · 模式」；暂停：手动「已暂停」、系统「已暂停 · 原因1、原因2」。
std::string statusTitle(const StatusInput& input);

// 状态卡副题：手动暂停「点『继续』恢复播放。」；系统让路「条件解除后自动续播，不会从头开始。」；
// 播放态「N 个视频 · 每 10 分钟换一个」（单循环/单条为「单循环播放 / 单张不变 / 不轮换」）；
// 非播放态返回对应空态原因。
std::string statusSubtitle(const StatusInput& input);

// 空态卡文案（标题/正文/主按钮/次按钮）。UI 不另拼文案。
struct EmptyStateCopy {
    std::string title;
    std::string reason;
    std::string primaryAction;
    std::string secondaryAction;
};

// 空态三变体（未配置 / 目录不在 / 0 可播）；Playing 返回 nullopt。
// 文案逐字对齐需求 §7.3；视频不得出现「转码」类不可用入口，图片不得宣称已按像素档过滤。
std::optional<EmptyStateCopy> emptyStateCopy(Availability availability, WallpaperKind kind);

// 统计单位：视频「N 个视频」、图片「N 张可用图片」。
std::string libraryCountLabel(WallpaperKind kind, int count);

} // namespace module::wallpaper::presentation