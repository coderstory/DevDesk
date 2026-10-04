// src/module_about/about_anim.h
// 关于页「动态光晕」状态机：6 个柔光球（利萨如漂移 + 呼吸）+ 聚光扫过带。
// 纯值类型 + inline 函数，零 EUI 依赖；独立成头以便单测直接驱动。
// 坑：绘制元素闸门 ≤ 20（光斑 6×3 层 + 扫带 2）；球心 center + amp ≤ 1.0 不出页面。
#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace module::about {

// 极简确定性 RNG（golden-ratio LCG），初始散布每次启动可复现。
struct GlowRng {
    std::uint64_t s;
    explicit GlowRng(std::uint64_t seed) : s(seed) {}
    float next() {  // [0,1)
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<float>((s >> 33) & 0xFFFFFFu) / 16777216.0f;
    }
};

// 单个光斑。中心/振幅存页面比例（窗口改尺寸时 compose 现算像素），半径存像素。
struct GlowBlob {
    float radius = 0.0f;                     // 最大层半径（= 直径/2）
    float centerX = 0.0f, centerY = 0.0f;    // 利萨如中心（页面宽/高比例）
    float ampX = 0.0f, ampY = 0.0f;          // 利萨如振幅（页面宽/高比例）
    float periodX = 0.0f, periodY = 0.0f;    // 两轴漂移周期（秒）
    float phaseX = 0.0f, phaseY = 0.0f;      // 相位（弧度），互相错开
    float breathPeriod = 0.0f;               // 呼吸周期（秒）
    float breathPhase = 0.0f;
    float colorMix = 0.0f;                   // 0 = primary，1 = 暖琥珀
};

struct AboutAnimState {
    // 光斑数 6：≤20 元素闸门与观感的折中（再多数就得减层，2 层叠不出平滑剖面）。
    static constexpr int kBlobCount = 6;
    // 每球 3 层：半径 0.70/0.85/1.00R，alpha 0.15/0.10/0.05。
    static constexpr int kLayersPerBlob = 3;
    static constexpr int kSweepHalves = 2;     // 扫带两半（对称 alpha 剖面）
    // 每帧新增绘制元素数（性能闸门 ≤ 20）：6×3 + 2 = 20。
    static constexpr int kGlowElementCount =
        kBlobCount * kLayersPerBlob + kSweepHalves;

    // 光斑直径 240-400px：上限 = 内容区宽 900 的 44%（单球不过半才读作「多个光团」）。
    static constexpr float kDiameterMin = 240.0f;
    static constexpr float kDiameterMax = 400.0f;
    static constexpr float kLayerAlphaMax = 0.15f;  // 最内层
    static constexpr float kLayerAlphaMin = 0.05f;  // 最外层
    // 层半径 = radius × (0.70 + 0.15k)。反同心环带的关键是相邻层边缘间距 < 柔边
    // 模糊宽（0.15R ≈ 18-30px < blur 56，三级 alpha 台阶并成连续渐变）。
    static constexpr float kLayerRadiusInner = 0.70f;
    static constexpr float kLayerRadiusStep = 0.15f;
    static constexpr float kBreathDepth = 0.35f;      // 呼吸 ±35%
    static constexpr float kBreathPeriodMin = 6.0f;
    static constexpr float kBreathPeriodMax = 9.0f;
    static constexpr float kPeriodMin = 18.0f;        // x 轴漂移周期
    static constexpr float kPeriodMax = 26.0f;
    static constexpr float kPeriodRatioY = 0.62f;     // y 轴周期 / x 轴周期
    static constexpr float kNeonShadowBlur = 48.0f;   // 最外圈霓虹边
    static constexpr float kNeonShadowAlpha = 0.35f;
    // 内层柔边 shadow（offset 0，同色）把离散硬边圆盘化成软边圆盘，抹平同心圆台阶；
    // 每层多一个 shadow pass，元素数不变。
    static constexpr float kLayerSoftBlur = 56.0f;
    // 柔边 halo alpha 倍率 2.0：blur 边界值是峰值一半，取 2 倍才使台阶归零。
    static constexpr float kLayerSoftAlphaScale = 2.0f;
    // 散布区（页面比例）：中心留边 + 振幅，球心不出页面（centerMax + ampMax ≤ 1.00）。
    static constexpr float kCenterMinX = 0.22f;
    static constexpr float kCenterMaxX = 0.78f;
    static constexpr float kCenterMinY = 0.24f;
    static constexpr float kCenterMaxY = 0.64f;
    static constexpr float kAmpMinX = 0.14f;
    static constexpr float kAmpMaxX = 0.22f;
    static constexpr float kAmpMinY = 0.10f;
    static constexpr float kAmpMaxY = 0.20f;

    static constexpr float kSweepPeriod = 14.0f;      // 秒（左→右一次）
    static constexpr float kSweepWidth = 220.0f;      // px
    static constexpr float kSweepAlpha = 0.05f;       // 峰值 alpha
    static constexpr float kSweepTiltRad = 0.31415926f;  // 18°
    static constexpr float kSweepShadowBlur = 48.0f;  // 边缘柔化（无硬边）
    static constexpr float kSweepShadowAlpha = 0.10f;
    static constexpr float kSweepHeightFactor = 1.5f; // 带高 = 页高 × 1.5（两端出画）

    // 驱动用 onTimer 30fps 节拍（非每帧重绘），dt 固定 kFrameSeconds；空闲 120s
    // 后休眠（compose 撤掉 onTimer → 零重绘），任意指针移动唤醒。
    static constexpr float kFrameSeconds = 1.0f / 30.0f;
    static constexpr float kWakeHoldSeconds = 120.0f;

    std::array<GlowBlob, kBlobCount> blobs;
    float time = 0.0f;       // 动画时间（秒；休眠时冻结）
    float wakeHold = 0.0f;   // 距上次指针移动的秒数
    bool initialized = false;

    // 休眠判定：空闲超时 → false（compose 撤掉 onTimer，运行时不再出帧）
    bool awake() const { return wakeHold < kWakeHoldSeconds; }

    // 每个节拍推进一次；休眠期内 time 冻结，画面停在原处不跳变。
    void step(float dt) {
        wakeHold += dt;
        if (wakeHold < kWakeHoldSeconds) {
            time += dt;
        }
    }
};

inline float blobLayerRadius(float blobRadius, int layer) {
    return blobRadius * (AboutAnimState::kLayerRadiusInner +
                         AboutAnimState::kLayerRadiusStep *
                             static_cast<float>(layer));
}

inline float blobLayerAlpha(int layer) {
    constexpr float span = static_cast<float>(AboutAnimState::kLayersPerBlob - 1);
    const float t = static_cast<float>(layer) / span;
    return AboutAnimState::kLayerAlphaMax +
           (AboutAnimState::kLayerAlphaMin - AboutAnimState::kLayerAlphaMax) * t;
}

// 呼吸系数 1 ± kBreathDepth；各光斑相位/周期不同，互不同步。
inline float blobBreath(const AboutAnimState& state, int index) {
    const GlowBlob& blob = state.blobs[static_cast<std::size_t>(index)];
    const float omega = 6.28318531f / blob.breathPeriod;
    return 1.0f + AboutAnimState::kBreathDepth *
                      std::sin(omega * state.time + blob.breathPhase);
}

// 利萨如漂移（px）：两轴周期不成整数比 → 长时间不重复。
inline void blobCenter(const AboutAnimState& state, int index, float w, float h,
                       float& outX, float& outY) {
    const GlowBlob& blob = state.blobs[static_cast<std::size_t>(index)];
    outX = blob.centerX * w +
           blob.ampX * w * std::sin(6.28318531f * state.time / blob.periodX +
                                    blob.phaseX);
    outY = blob.centerY * h +
           blob.ampY * h * std::sin(6.28318531f * state.time / blob.periodY +
                                    blob.phaseY);
}

inline float sweepLeft(const AboutAnimState& state, float w) {
    const float progress =
        std::fmod(state.time, AboutAnimState::kSweepPeriod) /
        AboutAnimState::kSweepPeriod;
    const float travel = w + AboutAnimState::kSweepWidth * 2.0f;
    return -AboutAnimState::kSweepWidth + progress * travel;
}

// 首次进入：按固定种子散布光斑（半径/中心/振幅/周期/相位），可复现。
inline void initGlowLayer(AboutAnimState& state) {
    GlowRng rng(0x9E3779B97F4A7C15ULL);
    constexpr float kDiameterHalfMin = AboutAnimState::kDiameterMin * 0.5f;
    constexpr float kDiameterHalfMax = AboutAnimState::kDiameterMax * 0.5f;
    for (int i = 0; i < AboutAnimState::kBlobCount; ++i) {
        GlowBlob& blob = state.blobs[static_cast<std::size_t>(i)];
        const float radiusT = rng.next();
        blob.radius = kDiameterHalfMin + radiusT * (kDiameterHalfMax - kDiameterHalfMin);
        const float cX = rng.next();
        const float cY = rng.next();
        const float aX = rng.next();
        const float aY = rng.next();
        blob.centerX = AboutAnimState::kCenterMinX +
                       cX * (AboutAnimState::kCenterMaxX - AboutAnimState::kCenterMinX);
        blob.centerY = AboutAnimState::kCenterMinY +
                       cY * (AboutAnimState::kCenterMaxY - AboutAnimState::kCenterMinY);
        blob.ampX = AboutAnimState::kAmpMinX +
                    aX * (AboutAnimState::kAmpMaxX - AboutAnimState::kAmpMinX);
        blob.ampY = AboutAnimState::kAmpMinY +
                    aY * (AboutAnimState::kAmpMaxY - AboutAnimState::kAmpMinY);
        const float periodT = rng.next();
        blob.periodX = AboutAnimState::kPeriodMin +
                       periodT * (AboutAnimState::kPeriodMax - AboutAnimState::kPeriodMin);
        blob.periodY = blob.periodX * AboutAnimState::kPeriodRatioY;
        // 相位互相错开 2π/N（光斑不同步，不会一起呼吸像闪灯）
        const float phaseOffset = 6.28318531f * static_cast<float>(i) /
                                  static_cast<float>(AboutAnimState::kBlobCount);
        blob.phaseX = phaseOffset + rng.next() * 0.6f;
        blob.phaseY = phaseOffset * 1.7f + rng.next() * 0.6f + 1.57079633f;
        const float breathT = rng.next();
        blob.breathPeriod = AboutAnimState::kBreathPeriodMin +
                            breathT * (AboutAnimState::kBreathPeriodMax -
                                       AboutAnimState::kBreathPeriodMin);
        blob.breathPhase = phaseOffset + rng.next() * 0.8f;
        // 配色按序号均匀铺满 primary→暖琥珀
        blob.colorMix = static_cast<float>(i) /
                        static_cast<float>(AboutAnimState::kBlobCount - 1);
    }
    state.time = 0.0f;
    state.wakeHold = 0.0f;
    state.initialized = true;
}

} // namespace module::about
