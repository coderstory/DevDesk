// src/core/window_geometry.cpp
// 窗体记忆几何账本实现（接口与规则见 window_geometry.h）。账本是进程级单值：
// 读写都发生在主线程（compose / 退出钩子），无需原子量
#include "window_geometry.h"

#include <cmath>

namespace core::window_geometry {
namespace {

// 账本：hasSize=false 表示从未拿到过可用尺寸（未播种且没见过非最大化帧）
struct Ledger {
    float width = 0.0f;
    float height = 0.0f;
    bool hasSize = false;
    bool maximized = false;
};

Ledger& ledger() {
    static Ledger instance;
    return instance;
}

bool usableSize(float w, float h) {
    return std::isfinite(w) && std::isfinite(h) && w > 0.0f && h > 0.0f;
}

} // namespace

void seedRestoreSize(int logicalWidth, int logicalHeight) {
    if (logicalWidth <= 0 || logicalHeight <= 0) return;
    Ledger& l = ledger();
    // 已有帧记录（非 0 启动路径）不回退成播种值：播种只兜「全程最大化、无帧可记」
    if (!l.hasSize) {
        l.width = static_cast<float>(logicalWidth);
        l.height = static_cast<float>(logicalHeight);
        l.hasSize = true;
    }
}

void noteFrame(float logicalWidth, float logicalHeight, bool maximized) {
    Ledger& l = ledger();
    l.maximized = maximized;
    // 最大化帧不记尺寸（保留 = 还原态）；异常几何不落账
    if (maximized || !usableSize(logicalWidth, logicalHeight)) return;
    l.width = logicalWidth;
    l.height = logicalHeight;
    l.hasSize = true;
}

Snapshot current() {
    const Ledger& l = ledger();
    Snapshot snap;
    snap.valid = l.hasSize;
    snap.maximized = l.maximized;
    snap.logicalWidth = static_cast<int>(std::lround(l.width));
    snap.logicalHeight = static_cast<int>(std::lround(l.height));
    return snap;
}

void resetForTest() {
    ledger() = Ledger{};
}

} // namespace core::window_geometry
