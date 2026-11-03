// src/module_wallpaper/platform/image_fit_geometry.h
// 图片四种铺法的落位几何（纯值计算，零平台依赖，跨平台单测直接覆盖）。
//
// 坐标系：**左上角原点、y 向下**，与 CGImage 的绘制方向一致。真正落笔的 CGContext
// 默认原点在左下，由调用方翻转一次（不要让本层去猜画布是否 flipped）。
//
// 为什么不直接用系统的 contentsGravity：fill / fit / center 有对应 gravity，
// tile 没有；两套机制并存会让「换铺法」变成换绘制路径，出问题时无法区分是几何
// 算错还是 gravity 选错。所以四种铺法一律自己算。
#pragma once

#include "module_wallpaper/core/model.h"

namespace module::wallpaper::image_fit_geometry {

struct Rect {
    double x = 0;
    double y = 0;
    double w = 0;
    double h = 0;
};

struct Placement {
    // 落位矩形，相对画布左上角。fill 时可以超出画布（超出部分由层裁掉）——
    // 这是「填充」的定义，不是越界。
    Rect rect;
    // 平铺标记。为 true 时 rect 是**一块砖**（原始像素尺寸、贴左上角），
    // 由调用方在画布内重复铺；这时矩形不居中，居中会让平铺错位半块砖。
    bool tiles = false;
};

// 四种模式共用一条公式，只有 scale 取法不同：
//   Fill 取 max（铺满、超出裁掉）/ Fit 取 min（完整、留边）/ Center 取 1（原始尺寸）。
// 平铺不走缩放——缩放过的砖会露出接缝。
inline Placement placement(double imageWidth, double imageHeight,
                           double canvasWidth, double canvasHeight,
                           ImageFit fit) {
    // 任一边非正就没有可画的矩形。降级成零矩形而不是另起一条「什么都不画」的
    // 分支：两条路径里迟早有一条忘了处理。
    if (imageWidth <= 0 || imageHeight <= 0 || canvasWidth <= 0 || canvasHeight <= 0) {
        return Placement{};
    }
    if (fit == ImageFit::Tile) {
        return Placement{Rect{0, 0, imageWidth, imageHeight}, true};
    }

    double scale = 1.0;
    switch (fit) {
        case ImageFit::Fill:
            scale = (canvasWidth / imageWidth > canvasHeight / imageHeight)
                        ? canvasWidth / imageWidth
                        : canvasHeight / imageHeight;
            break;
        case ImageFit::Fit:
            scale = (canvasWidth / imageWidth < canvasHeight / imageHeight)
                        ? canvasWidth / imageWidth
                        : canvasHeight / imageHeight;
            break;
        case ImageFit::Center:
            scale = 1.0;
            break;
        case ImageFit::Tile:
            scale = 1.0;  // 上面已返回，这里只为 switch 穷举
            break;
    }
    const double w = imageWidth * scale;
    const double h = imageHeight * scale;
    return Placement{Rect{(canvasWidth - w) / 2, (canvasHeight - h) / 2, w, h}, false};
}

} // namespace module::wallpaper::image_fit_geometry
