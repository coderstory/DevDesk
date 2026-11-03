// src/module_wallpaper/platform/fullscreen_geometry.h
// 全屏判定的纯值计算：零 AppKit / CoreGraphics 依赖，跨平台单测直接覆盖。
//
// 判定口径（沿用参考实现实跑结论）：**屏幕被应用窗口铺满就让路** —— 原生全屏与
// 「最大化」一视同仁，因此判定 = 覆盖率 ≥ 1.0，不额外要求事件信号（最大化/还原
// 本身不产生 Space 或前台切换事件，合取式设计会让「盖住了却还在播」悬到下次事件）。
//
// 两个坐标系必须分开：
//   * CGWindowList 报的 bounds 是**左上角原点**；
//   * NSScreen.visibleFrame 是**左下角原点**。
// 不翻到同一原点覆盖率恒为 0。顺序固定：先内缩补偿 → 再翻转 → 最后求交
//（内缩在原始矩形上外扩、翻转只换原点不换尺寸，两者不可交换）。
#pragma once

#include <algorithm>
#include <map>
#include <vector>

namespace module::wallpaper::fullscreen_geometry {

struct Rect {
    double x = 0;
    double y = 0;
    double w = 0;
    double h = 0;

    double area() const { return (w > 0 ? w : 0) * (h > 0 ? h : 0); }
};

// 桌面层 borderless 窗口在 CGWindowList 里的系统性内缩（单屏实测值）。
// 基准量「屏幕层窗口」时用零值。
struct DesktopWindowInset {
    double left = 0;
    double top = 0;
    double right = 0;
    double bottom = 0;
};

inline DesktopWindowInset measuredInset() {
    // 1470×956 单屏实测。换屏要重新量——量偏的表现是「真全屏没被识别」。
    return DesktopWindowInset{14, 9, 14, 9};
}

// 一扇窗口的几何样本。raw 是 CGWindowList 原样报的值（左上角原点，未补偿）。
struct WindowSample {
    long long pid = 0;
    Rect raw;
};

struct CoverageResult {
    double global = 0;      // 按 pid 聚合后的全局覆盖率，封顶 1.0
    long long globalPid = -1;
    int rectCount = 0;
    double perWindowBest = 0;  // 同 pid 下逐矩形覆盖率最大值（诊断用）
};

inline Rect flipTopLeftToBottomLeft(const Rect& r, double screenHeight) {
    return Rect{r.x, screenHeight - (r.y + r.h), r.w, r.h};
}

inline Rect intersect(const Rect& a, const Rect& b) {
    const double x1 = std::max(a.x, b.x);
    const double y1 = std::max(a.y, b.y);
    const double x2 = std::min(a.x + a.w, b.x + b.w);
    const double y2 = std::min(a.y + a.h, b.y + b.h);
    return Rect{x1, y1, x2 - x1, y2 - y1};
}

inline Rect compensatingInset(const Rect& raw, const DesktopWindowInset& inset) {
    return Rect{raw.x - inset.left, raw.y - inset.top,
                raw.w + inset.left + inset.right, raw.h + inset.top + inset.bottom};
}

// 内缩补偿 + 翻转 + 按 pid 聚合。**这里不判全屏**（阈值归调用方）。
//
// 口径：按 pid 分组求和、除以 visibleFrame 面积、封顶 1.0，不同 pid 之间取最大值。
// 分组与求和一起是承重的：同一 app 双窗口上下拼接（各占半屏）逐窗口口径必漏判。
//
//   samples     窗口矩形样本（调用方负责去重/过滤自身进程与非常规层）
//   visible     NSScreen.visibleFrame（左下角原点）。分母用它不用 frame：真全屏会
//               因刘海/菜单栏永远差一点
//   screenHeight 翻转用的屏幕高
inline CoverageResult aggregate(const std::vector<WindowSample>& samples,
                                const Rect& visible, double screenHeight,
                                const DesktopWindowInset& inset) {
    std::map<long long, std::vector<Rect>> groups;
    for (const WindowSample& sample : samples) {
        groups[sample.pid].push_back(sample.raw);
    }

    const double visibleArea = visible.area();
    CoverageResult out;
    // 按 pid 升序遍历：coverage 相同时取 pid 小的那个，结果不随容器顺序漂移。
    for (const auto& entry : groups) {
        double sum = 0.0;
        double best = 0.0;
        for (const Rect& raw : entry.second) {
            const Rect compensated = compensatingInset(raw, inset);
            const Rect flipped = flipTopLeftToBottomLeft(compensated, screenHeight);
            const double area = intersect(flipped, visible).area();
            sum += area;
            if (visibleArea > 0 && area / visibleArea > best) best = area / visibleArea;
        }
        const double coverage =
            visibleArea > 0 ? std::min(1.0, sum / visibleArea) : 0.0;
        if (coverage > out.global) {
            out.global = coverage;
            out.globalPid = entry.first;
            out.rectCount = static_cast<int>(entry.second.size());
            out.perWindowBest = best;
        }
    }
    return out;
}

// 判定：覆盖率到满分即让路。没有阈值旋钮——唯一的常量就是 1.0 本身。
inline bool covers(const CoverageResult& coverage) { return coverage.global >= 1.0; }

} // namespace module::wallpaper::fullscreen_geometry
