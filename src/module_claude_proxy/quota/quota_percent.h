// src/module_claude_proxy/quota/quota_percent.h
// usedPercent 范围裁剪（唯一入口）：上游可能给 >100 / <0 / NaN 原样流出，渲染层不负责裁剪，故在本层由代码保证。
#pragma once

namespace module::claude::quota {

// 已用百分比裁剪到 [0, 100]：小数原样保留（不取整），>100 → 100，<0 或 NaN → 0。
// NaN 必须显式处理：x < 0.0 与 x > 100.0 对 NaN 都是 false，直接 clamp 会让它穿过。
inline double clampUsedPercent(double usedPercent) {
    if (!(usedPercent > 0.0)) return 0.0;  // 负数与 NaN 都落这里（比较对 NaN 恒假）
    return usedPercent > 100.0 ? 100.0 : usedPercent;
}

}  // namespace module::claude::quota
