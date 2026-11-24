// src/ui_kit/home_anim.h
// 首页留白动画（need506 定稿：点阵波纹 × 打字机组合）。浅色唯一，无深色分支。
// 纯函数 + 可测试状态机，零静态态；参数 = 设计文档 §2/§3 全数值。
//
// 点阵：28px 网格 / 10px 点 / 3400ms 周期 / 对角 100ms 步进 / 600 点上限
//（密度优先降档 28→40→52）/ 深档点缀 (r*7+c*13)%17==0 / 波峰 45% 相位。
// 打字机：82ms 打 / 1700ms 停 / 26ms 擦 / 320ms 轮换 / 26 字截断 / 4 条轮换。
#pragma once

#include <eui/dsl.h>

#include "core/theme.h"

#include <cmath>
#include <string>
#include <vector>

namespace ui_kit::home_anim {

// ── 点阵几何（设计 §2/§5）──
constexpr float kGridSpacing = 28.0f;      // 基准网格
constexpr float kSpacingStep1 = 40.0f;     // 降档一
constexpr float kSpacingStep2 = 52.0f;     // 降档二
constexpr int kMaxDots = 600;              // 点数上限（性能红线）
constexpr float kDotDiameter = 10.0f;
constexpr float kPeriodSec = 3.4f;         // 单点周期 3400ms
constexpr float kDiagStepSec = 0.1f;       // 对角相邻 delay 步进 100ms
constexpr float kCrestPhase = 0.45f;       // 波峰相位（≈1530ms）
constexpr float kRestScale = 0.6f;
constexpr float kCrestScale = 1.22f;
constexpr float kRestAlpha = 0.28f;
constexpr float kCrestAlpha = 0.98f;

// 网格间距按留白区实时尺寸自适应（每次 compose 重算，无缓存几何）：面积÷间距²
//（+1 轴点数约定，与 drawDotGrid 同式）超 600 → 40 → 52（设计 §5 密度优先降档）。
inline float gridSpacingFor(float w, float h) {
    const float usableW = w - 40.0f;  // 左右各内缩 20
    const float usableH = h - 54.0f;  // 上 14 / 下 40
    if (usableW <= 0.0f || usableH <= 0.0f) return kSpacingStep2;
    const auto dots = [](float sw, float sh, float sp) {
        return (static_cast<int>(sw / sp) + 1) * (static_cast<int>(sh / sp) + 1);
    };
    if (dots(usableW, usableH, kGridSpacing) <= kMaxDots) return kGridSpacing;
    if (dots(usableW, usableH, kSpacingStep1) <= kMaxDots) return kSpacingStep1;
    return kSpacingStep2;
}

// 波形钟形包络（ease-inout 感）：local ∈ [0,1)，峰值在 kCrestPhase。
inline float waveBell(float local) {
    float d = local - kCrestPhase;
    if (d > 0.5f) d -= 1.0f;  // 环相位最短差（相邻周期衔接）
    if (d < -0.5f) d += 1.0f;
    const float x = d / 0.085f;  // sigma：波前空间宽度 ≈15 步 ×100ms / 3400ms
    return std::exp(-0.5f * x * x);
}

// 单点形态（供测试钉值）：返回 scale；alpha 由 drawLayer 按 bell 同式插值。
inline float dotScale(float phaseSec, int row, int col) {
    float local = (phaseSec / kPeriodSec) +
                  static_cast<float>(row + col) * kDiagStepSec / kPeriodSec;
    local -= std::floor(local);
    const float bell = waveBell(local);
    return kRestScale + (kCrestScale - kRestScale) * bell;
}

// ── 打字机状态机（设计 §3/§4；泵帧驱动 tick，20fps 粒度足够 82ms/字）──
struct Typer {
    enum class Phase { Typing, Hold, Erasing, Gap, Static };
    std::vector<std::string> items;  // 已截断轮换文本（≤26 字符）
    int idx = 0;
    int shown = 0;                   // 当前已显示字符数（code point 简化 = 字节安全用 u8 切）
    double lastTick = 0.0;
    Phase phase = Phase::Typing;

    // 推进状态机；nowSec = 壁钟秒。返回当前应显示文本（减动效直显 = staticFull()）。
    void tick(double nowSec);
    std::string visibleText() const;
    // 直显态（减动效 / 空列表占位 / 单条常显）。
    std::string staticFull() const;
    bool rotating() const { return items.size() > 1; }
};

// 空库轮播文案池（10 条，每条 ≤20 字适配 26 截断）。来源 = need506 用户拍板
//（UAT 追加令 2026-11-24：最近笔记 <2 条时轮播本池，时序参数与标题轮换一致）。
inline constexpr const char* kQuotePool[10] = {
    "山高路远，看世界也找自己",
    "把今天过成想回去的那天",
    "慢慢来，比较快",
    "先做起来，路会自己展开",
    "写下来的想法才算数",
    "今天也是值得记录的一天",
    "小步不停，远路也短",
    "留白处，正适合落笔",
    "专注一件事，就赢过大半",
    "记下来，别让灵感溜走",
};

// 26 字符截断（>26 → 前 25 + "…"）。
inline std::string truncateTitle(const std::string& title) {
    // 按 code point 数；UTF-8 每码点 1-4 字节
    int cp = 0;
    size_t i = 0;
    while (i < title.size()) {
        const unsigned char c = static_cast<unsigned char>(title[i]);
        const size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        i += len;
        ++cp;
    }
    if (cp <= 26) return title;
    // 回切到 25 码点字节位
    size_t cut = 0;
    int seen = 0;
    while (cut < title.size() && seen < 25) {
        const unsigned char c = static_cast<unsigned char>(title[cut]);
        cut += c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        ++seen;
    }
    return title.substr(0, cut) + "…";
}

// ── 绘制 ──

// 点阵波纹层。返回 true = 画了层（h 过最小舞台、opacity>0）。
// (x,y,w,h) = 留白区矩形；phase = 壁钟相位秒；layerOpacity = 入场 500ms 包络。
bool drawDotGrid(eui::Ui& ui, float x, float y, float w, float h,
                 float phase, float layerOpacity, const core::theme::Tokens& t);

// 打字机组（标签 + 垫板主行 + 副语 + 轮换指示条）。reduced = 直显态。
void drawTyper(eui::Ui& ui, float cx, float cy, float maxW, const Typer& typer,
               float phase, const std::string& subtitle, bool reduced,
               const core::theme::Tokens& t);

}  // namespace ui_kit::home_anim
