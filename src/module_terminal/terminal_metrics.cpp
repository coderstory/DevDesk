// src/module_terminal/terminal_metrics.cpp
#include "terminal_metrics.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <unordered_map>

#include "core/logging.h"
#include "core/render/text.h"

namespace terminal {
namespace {

constexpr float kCellWFactorFallback = 0.6f;   // Phase 1 估算（查询失败回落）
constexpr float kCellHFactorFallback = 1.25f;

// advance 采样串：等宽字体全 ASCII 同 advance，10 个 M 摊平逐字形取整噪声
constexpr const char* kAdvanceSample = "MMMMMMMMMM";

} // namespace

TerminalCellMetrics metricsFromAdvance(float advance, float naturalLineHeight, float fontSize) {
    TerminalCellMetrics m;
    if (fontSize <= 0.0f) return m;
    if (advance <= 0.0f || naturalLineHeight <= 0.0f) {
        m.cellW = std::round(fontSize * kCellWFactorFallback);
        m.cellH = std::round(fontSize * kCellHFactorFallback);
        return m; // valid=false：估算回落
    }
    m.cellW = std::round(advance);
    if (m.cellW < 1.0f) m.cellW = 1.0f;
    const float lo = fontSize * 1.20f;
    const float hi = fontSize * 1.35f;
    m.cellH = std::round(std::clamp(naturalLineHeight, lo, hi));
    if (m.cellH < 1.0f) m.cellH = 1.0f;
    m.valid = true;
    return m;
}

TerminalCellMetrics terminalCellMetrics(float fontSize, const std::string& fontFamily) {
    // "" = 内置等宽（与 terminal_grid_widget.h setTypography 同口径）。空串必须在这里
    // 归一为 "monospace"：直接把 "" 传进 EUI 会解析成默认 UI **比例**字体，M 宽
    // ≈1.5× 等宽 advance → cellW 虚胖，光标/IME/选区列坐标整体右漂（老板 2026-11-09
    // 「打开终端光标跑右侧」根因；绘制侧 setTypography 已归一，两侧口径必须一致）。
    const std::string family = fontFamily.empty() ? std::string("monospace") : fontFamily;
    // fontSize 档位有限 + fontFamily 选项有限（need250 设置面板），复合键 map 缓存
    // 即稳态零查询
    static std::mutex mu;
    static std::unordered_map<std::string, TerminalCellMetrics> cache;
    const std::string key = family + "/" + std::to_string(static_cast<int>(std::lround(fontSize)));
    {
        std::lock_guard<std::mutex> lock(mu);
        const auto it = cache.find(key);
        if (it != cache.end()) return it->second;
    }
    // 主字体半角 advance：字体解析在 EUI（"monospace" = 内置等宽 token：Win=Cascadia
    // Mono→Consolas、mac=Menlo；具名族直传，解析失败走估算回落 valid=false）
    const float advance = core::TextPrimitive::measureTextWidth(
        kAdvanceSample, family, fontSize, 400) / 10.0f;
    const auto line = core::TextPrimitive::measureFontLineMetrics(family, fontSize, 400);
    const float naturalLineH = line.ascent - line.descent + line.lineGap;
    TerminalCellMetrics m = metricsFromAdvance(advance, naturalLineH, fontSize);
    // CellTrace 头（bug244 临时诊断，一次性）：实际字体文件路径 + advance 取整前后
    static bool cellTraceHeadDone = false;
    if (!cellTraceHeadDone) {
        cellTraceHeadDone = true;
        const std::string fontPath = core::TextPrimitive::resolveFontPath(family, 400);
        char lineBuf[320];
        std::snprintf(lineBuf, sizeof(lineBuf),
                      "CellTrace: font=%s advanceRaw=%.3f cellW=%.1f cellH=%.1f valid=%d",
                      fontPath.c_str(), static_cast<double>(advance), static_cast<double>(m.cellW),
                      static_cast<double>(m.cellH), m.valid ? 1 : 0);
        LOG_INFO(lineBuf);
    }
    {
        std::lock_guard<std::mutex> lock(mu);
        cache.emplace(key, m);
    }
    return m;
}

} // namespace terminal
