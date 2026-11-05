// src/module_terminal/terminal_metrics.h
// 终端 cell 度量（need241 字体改造）：弃 fontSize×0.6/×1.25 估算，改主字体真实
// advance / 行高（设计 §4.3）。查询走 EUI 文本度量面（monospace 解析 Win=Cascadia、
// mac=Menlo），查询失败回落旧估算值（valid=false 标注，不阻塞可用性）。
// 纯函数部分 doctest 直测（无字体依赖）。
#pragma once

namespace terminal {

struct TerminalCellMetrics {
    float cellW = 0.0f;  // 半角 advance 取整
    float cellH = 0.0f;  // 实际行高 clamp[1.20,1.35]×fontSize 取整
    bool valid = false;  // false = 字体查询失败，走了估算回落
};

// 纯函数：由 advance 与自然行高折算 cell 度量（取整 + clamp，设计 §4.3 表）。
// 非法输入（advance/行高 ≤0）返回 valid=false 的估算回落值。
TerminalCellMetrics metricsFromAdvance(float advance, float naturalLineHeight, float fontSize);

// 运行时查询（EUI 度量 + 按 fontSize 缓存；每帧调用零成本）。
TerminalCellMetrics terminalCellMetrics(float fontSize);

} // namespace terminal
