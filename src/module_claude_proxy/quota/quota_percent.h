// src/module_claude_proxy/quota/quota_percent.h
// usedPercent 范围裁剪（单一入口）
//
// 为什么需要它：quota_provider.h:20-22 的约定 3 只要求 provider 负责「量纲归一」
// （上游给的是「剩余 %」就自己反转），**没有要求裁剪范围**；G2 更是明写
// 「百分比不做 0-100 裁剪，下游渲染层负责显示策略」（provider_minimax_cn.cpp:18）。
// 但渲染层（G7）**尚不存在**——于是那句「下游负责」在本仓库当前形态下无人负责，
// 上游给 150% 或 -3 就会原样流出去。
//
// 处置：裁剪在本层做，且**由代码保证**（每个 provider 构造 QuotaWindow 时都过这个
// 函数），不再依赖任何一层的注释承诺。新增 provider 时照此调用即可。
//
// 为什么不改 quota_provider.h 的约定 3：那份契约自 G0 冻结以来零提交（见审计
// 「契约冻结面」），是本层「签名零变化」纪律的锚点；把裁剪做成一个显式调用的
// helper，既不破坏冻结，又让「裁剪发生过」变成可被断言的事实。
#pragma once

namespace module::claude::quota {

// 已用百分比裁剪到 [0, 100]。
//   上游给小数（如 1.672568%）→ 原样保留（本函数不取整）
//   上游给 >100（超额 / 量纲漂移）→ 100
//   上游给 <0 或 NaN（形态漂移）→ 0
// NaN 必须显式处理：`x < 0.0` 与 `x > 100.0` 对 NaN 都是 false，
// 直接 clamp 会让 NaN 原样穿过并污染下游格式化。
inline double clampUsedPercent(double usedPercent) {
    if (!(usedPercent > 0.0)) return 0.0;  // 负数与 NaN 都落这里（比较对 NaN 恒假）
    return usedPercent > 100.0 ? 100.0 : usedPercent;
}

}  // namespace module::claude::quota