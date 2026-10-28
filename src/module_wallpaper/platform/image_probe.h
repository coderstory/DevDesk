// src/module_wallpaper/platform/image_probe.h
// 图片尺寸头解码（平台无关接口）。Windows 实现 = WIC 建解码器读首帧宽高；
// 其它平台 stub 恒 unknown。
//
// 契约：
//   - 只取宽高，不解码像素（装饰图片墙级别的开销都不允许进扫描路径）。
//   - 失败（文件不存在 / 不支持的格式 / 解码器拒开 / COM 不可用）一律返回
//     known=false，不区分原因——调用方的统一语义是「尺寸未知 → 不过滤」
//     （宁滥勿缺：解码能力缺失不得吃掉用户的轮换池）。
//   - 允许任意线程调用；实现内部自管 COM 初始化。
// 生产装配：WallpaperService 的默认扫描路径把本函数注入 scanImageLibrary 的
// ImageSizeProbe 缝；core 层（image_library）不反向依赖本文件。
#pragma once

#include <string>

#include "module_wallpaper/core/image_library.h"

namespace module::wallpaper {

// Windows 返回 WIC 头解码实现；其它平台返回 stub（恒 unknown）。
ProbedImageSize probeImageSize(const std::string& path);

} // namespace module::wallpaper
