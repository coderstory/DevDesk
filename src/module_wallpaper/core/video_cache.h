// src/module_wallpaper/core/video_cache.h
// 视频内存缓存判定（need:311）：开关 × 文件大小 → 缓存形态。
// 纯逻辑零平台依赖；两平台播放器共用同一判定，UI/服务只透传布尔开关。
//
// 口径（PRD 决策 D1~D3）：默认关；开关开且视频文件 ≤512MB → 缓存；
// 超限 / 读大小失败 / 非视频 → 回落现状直读，不报错。
#pragma once

#include <cstdint>
#include <string>

namespace module::wallpaper {

// 缓存上限 512MB（含边界：恰好 512MB 仍缓存，>512MB 忽略）。
inline constexpr std::uint64_t kVideoMemCacheLimitBytes = 512ull * 1024ull * 1024ull;

struct VideoMemCacheDecision {
    bool useMemory = false;   // true = 走内存缓存路径；false = 现状直读
    std::uint64_t fileBytes = 0;
    std::string reason;  // 中文原因（useMemory=true 时为空；供日志/测试）
};

// enabled=设置开关；isVideoItem=扩展名是否视频（图片恒不缓存）；
// fileSizeKnown=false 表示 stat 失败（按「回落直读」处理）。
VideoMemCacheDecision decideVideoMemCache(bool enabled, bool isVideoItem,
                                          bool fileSizeKnown,
                                          std::uint64_t fileBytes);

// stat 取文件大小；失败返回 false（调用方按未知处理）。
bool queryFileSizeBytes(const std::string& path, std::uint64_t* fileBytes);

// 视频扩展名白名单（与 core/video_library.cpp 的 kVideoExtensions 同口径：
// mp4/mov/m4v，大小写不敏感）。平台控制器据此在图片装载时跳过缓存。
bool isVideoFilePath(const std::string& path);

} // namespace module::wallpaper
