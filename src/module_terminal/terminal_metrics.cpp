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

TerminalCellMetrics terminalCellMetrics(float fontSize) {
    // fontSize 档位数有限（页面常量 13，字号设置后续需求），map 缓存即稳态零查询
    static std::mutex mu;
    static std::unordered_map<int, TerminalCellMetrics> cache;
    const int key = static_cast<int>(std::lround(fontSize));
    {
        std::lock_guard<std::mutex> lock(mu);
        const auto it = cache.find(key);
        if (it != cache.end()) return it->second;
    }
    // 主字体半角 advance：monospace 族解析在 EUI（Win=Cascadia Mono→Consolas、
    // mac=Menlo，text_shaping resolveSystemMonospaceFontPath）
    const float advance = core::TextPrimitive::measureTextWidth(
        kAdvanceSample, "monospace", fontSize, 400) / 10.0f;
    const auto line = core::TextPrimitive::measureFontLineMetrics("monospace", fontSize, 400);
    const float naturalLineH = line.ascent - line.descent + line.lineGap;
    TerminalCellMetrics m = metricsFromAdvance(advance, naturalLineH, fontSize);
    // CellTrace 头（bug244 临时诊断，一次性）：实际字体文件路径 + advance 取整前后
    static bool cellTraceHeadDone = false;
    if (!cellTraceHeadDone) {
        cellTraceHeadDone = true;
        const std::string fontPath = core::TextPrimitive::resolveFontPath("monospace", 400);
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
