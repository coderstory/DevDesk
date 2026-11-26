// src/module_terminal/terminal_dir_menu.cpp
#include "terminal_dir_menu.h"

#include <algorithm>

namespace terminal {

float dirMenuHeight(int dirCount) {
    const int n = std::min(std::max(dirCount, 0), kStartupDirsMax);
    return 134.0f + 36.0f * static_cast<float>(n);
}

DirMenuGeometry dirMenuGeometry(float pageW, float pageH, float anchorRightX, float menuH) {
    DirMenuGeometry g;
    g.width = std::min(320.0f, std::max(0.0f, pageW - 24.0f));
    const float maxX = std::max(12.0f, pageW - 12.0f - g.width);
    g.x = std::clamp(anchorRightX - g.width, 12.0f, maxX);
    const float below = pageH - 50.0f - 12.0f;
    if (menuH > below) {
        g.flipped = true;
        g.y = std::max(12.0f, 44.0f - 6.0f - menuH);  // 底缘 = 工具栏顶−6，顶缘钳 ≥12
    } else {
        g.y = 50.0f;
    }
    return g;
}

DirMenuGeometry dirMenuGeometryAbove(float pageW, float pageH, float anchorCenterX,
                                     float anchorTopY, float menuH) {
    (void)pageH;
    DirMenuGeometry g;
    g.width = std::min(320.0f, std::max(0.0f, pageW - 24.0f));
    const float maxX = std::max(12.0f, pageW - 12.0f - g.width);
    g.x = std::clamp(anchorCenterX - g.width * 0.5f, 12.0f, maxX);
    g.y = std::max(12.0f, anchorTopY - 6.0f - menuH);
    g.flipped = true;
    return g;
}

float startupDirsBlockHeight(int dirCount) {
    if (dirCount <= 0) return 86.0f;  // 20 + 4 + 18 + 12 + 32（§4.1 空态）
    const int n = std::min(dirCount, kStartupDirsMax);
    return 44.0f * static_cast<float>(n) + 8.0f * static_cast<float>(n - 1) + 12.0f + 32.0f;
}

std::string elidedPathForDisplay(const std::string& path, const std::string& homeDir,
                                 float maxW,
                                 const std::function<float(const std::string&)>& measure) {
    if (path.empty()) return path;
    std::string shown = path;
    // ① 主目录前缀 → ~（要求前缀后紧跟分隔符，防 /home/u 误替 /home/u2）
    if (!homeDir.empty() && shown.size() > homeDir.size() &&
        shown.compare(0, homeDir.size(), homeDir) == 0 &&
        (shown[homeDir.size()] == '/' || shown[homeDir.size()] == '\\')) {
        shown = "~" + shown.substr(homeDir.size());
    }
    if (!measure || measure(shown) <= maxW) return shown;
    // ② 首段（盘符根 "X:\" / POSIX 根或 ~/）+「…」+ 末 2 段
    const size_t s2 = shown.find_last_of("/\\");
    const size_t s1 = (s2 == std::string::npos || s2 == 0)
                          ? std::string::npos
                          : shown.find_last_of("/\\", s2 - 1);
    if (s1 == std::string::npos) return shown;  // 段数不足，交由控件尾部省略
    std::string firstSeg;
    if (shown.size() >= 3 && shown[1] == ':') {
        firstSeg = shown.substr(0, 3);
    } else {
        const size_t p = shown.find_first_of("/\\");
        firstSeg = (p == std::string::npos) ? shown : shown.substr(0, p + 1);
    }
    const std::string mid = firstSeg + "…" + shown[s2] + shown.substr(s1 + 1);
    if (mid.size() >= shown.size()) return shown;  // 中段省略不省长度时不退化变长
    return mid;  // ③ 仍超宽 → 控件 TextOverflow::Ellipsis 兜底
}

} // namespace terminal