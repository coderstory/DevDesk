// src/module_terminal/terminal_dir_menu.h
// need#452 常用目录的纯几何/展示函数（设计 §4.1/§4.3/§4.4）：零 EUI 依赖，doctest 直测。
// 页面按这些结果布局——几何规则改动必须先改这里，测试即契约。
#pragma once

#include <functional>
#include <string>

#include "terminal_settings.h"

namespace terminal {

struct DirMenuGeometry {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    bool flipped = false;
};

// 弹层总高：6+32+n×36+9+36+9+36+6 = 134+36n（§4.3，n 钳 [0,12]）。
float dirMenuHeight(int dirCount);

// 工具栏锚定：右缘 = 锚点右缘（新建钮右缘），宽 min(320, pageW−24)，左右钳
// [12, pageW−12−w]；下方可用（pageH−50−12）不足 → 上翻（底缘 = 44−6），顶缘钳 ≥12。
DirMenuGeometry dirMenuGeometry(float pageW, float pageH, float anchorRightX, float menuH);

// 空态链接锚定：水平居中于链接中心、位于锚点上方 6（§5 空态行）；其余钳制同上。
DirMenuGeometry dirMenuGeometryAbove(float pageW, float pageH, float anchorCenterX,
                                     float anchorTopY, float menuH);

// 设置面板「常用目录」区块高（组头之下）：空态 86（20+4+18+12+32）；
// n 条 = 44n + 8(n−1) + 12 + 32（§4.1）。
float startupDirsBlockHeight(int dirCount);

// 长路径显示（§4.4）：主目录前缀替 `~`；仍超宽 → 首段 + … + 末 2 段；再超由控件
// 尾部省略兜底。measure = 文本测宽（单测注入假度量；空 = 不测宽）。
std::string elidedPathForDisplay(const std::string& path, const std::string& homeDir,
                                 float maxW,
                                 const std::function<float(const std::string&)>& measure);

} // namespace terminal