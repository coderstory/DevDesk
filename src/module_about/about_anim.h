// src/module_about/about_anim.h
// 关于页「动态光晕」（聚光灯 / 霓虹）动画状态。
//
// 沿革（2026-09-29）：初版背景是 7 个 1px 描边小圆环（弹簧跟随 + awake 休眠），
// 用户真机反馈「又简陋又不明显 几乎看不到」——1px 描边 + 0.18-0.28 不透明度
// 合成后只有个位数 ΔRGB，且 2s 无鼠标移动即休眠（看页面时几乎总是静止态）。
// 现按与用户确认的方案 A 重做：
//   1) 光斑 3 个，直径 380-620，同心圆叠层（6 层，alpha 0.10 → 0.01、半径递增）
//      伪造径向渐变 → 柔光球（不是硬边圆）；
//   2) 利萨如慢漂移（x 轴周期 18-26s，y 轴 = 0.62×x，互相错相位）+ 透明度呼吸 ±35%；
//   3) 聚光扫过带：宽 220（两半各 110，对称梯形 alpha 0→0.05→0，无硬边）、
//      倾斜 18°、每 14s 从左扫到右一次；
//   4) 霓虹感：最外圈用 shadow(blur 48, offset 0, primary@0.35) 提亮边缘；
//   5) 配色只用主题 primary + 暖琥珀（core::theme 的 warning token，方案 B 已定，
//      **不新增色**）；扫带用 primary。
//
// 状态机本体（GlowBlob / AboutAnimState / initGlowLayer / step / awake）留在这里、
// 独立成头，是为了单测能直接驱动：tests/test_modules/test_ui_modern.cpp 的
// uiAboutGlow 用**字面量**锁光斑数量 / 直径区间 / 呼吸 alpha 区间 / 扫带周期 /
// 漂移与休眠（拿头文件常量断言等于自己是恒真断言，调回去也不会红——先例见
// 旧版 uiAboutRingAnim 的注释）。纯值类型 + inline 函数：零 EUI 依赖
//（颜色在 compose 侧按当前主题现算，见 about_module.cpp 的 mixThemeColor）。
#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace module::about {

// 极小确定性 RNG（golden-ratio LCG）：初始散布每次启动可复现，
// 免 <random> 依赖（零头文件成本）。
struct GlowRng {
    std::uint64_t s;
    explicit GlowRng(std::uint64_t seed) : s(seed) {}
    float next() {  // [0,1)
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<float>((s >> 33) & 0xFFFFFFu) / 16777216.0f;
    }
};

// 单个光斑。中心/振幅存**页面比例**（窗口缩放/改尺寸时 compose 现算像素，
// 旧版存绝对像素、initialized 后不再重排，改窗口大小就漂到旧边界外），
// 半径存像素（直径区间是需求硬口径）。
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
    static constexpr int kBlobCount = 3;
    static constexpr int kLayersPerBlob = 6;   // 需求区间 6-8
    static constexpr int kSweepHalves = 2;     // 扫带两半（对称 alpha 剖面）
    // 每帧新增绘制元素数（性能闸门 ≤ 20）：光斑 3×6 + 扫带 2 = 20。
    // 光斑 6 层（不是 8 层）就是为了卡住这条：8 层 → 24+2 直接破闸。
    static constexpr int kGlowElementCount =
        kBlobCount * kLayersPerBlob + kSweepHalves;

    // ---- 光斑（方案 A 口径）----
    static constexpr float kDiameterMin = 380.0f;
    static constexpr float kDiameterMax = 620.0f;
    static constexpr float kLayerAlphaMax = 0.10f;  // 最内层
    static constexpr float kLayerAlphaMin = 0.01f;  // 最外层
    // 层半径 = radius × (kLayerRadiusInner + kLayerRadiusStep × k)，k = 0..5
    // → 最内层 0.42R、最外层 1.00R（层间 0.116R 递减重叠，叠出径向柔光）
    static constexpr float kLayerRadiusInner = 0.42f;
    static constexpr float kLayerRadiusStep = 0.116f;
    static constexpr float kBreathDepth = 0.35f;      // 呼吸 ±35%
    static constexpr float kBreathPeriodMin = 6.0f;
    static constexpr float kBreathPeriodMax = 9.0f;
    static constexpr float kPeriodMin = 18.0f;        // x 轴漂移周期
    static constexpr float kPeriodMax = 26.0f;
    static constexpr float kPeriodRatioY = 0.62f;     // y 轴周期 / x 轴周期
    static constexpr float kNeonShadowBlur = 48.0f;   // 最外圈霓虹边
    static constexpr float kNeonShadowAlpha = 0.35f;
    // 内层柔边：每层挂一个与自身同色的 shadow（offset 0），把硬边圆盘化成
    // 软边圆盘——6 层离散叠层原本能看见同心圆台阶（浅色档尤其明显），柔边后
    // 台阶被抹平，读起来才是「径向渐变柔光球」。代价：每层多一个 shadow pass
    //（SDF 着色，廉价），元素数不变（仍 20 ≤ 20 闸门）
    static constexpr float kLayerSoftBlur = 40.0f;
    static constexpr float kLayerSoftAlphaScale = 0.9f;
    // 散布区（页面比例）：中心留边 + 振幅，保证光斑始终压在内容区附近，
    // 不在页面角落空转
    static constexpr float kCenterMinX = 0.24f;
    static constexpr float kCenterMaxX = 0.76f;
    static constexpr float kCenterMinY = 0.26f;
    static constexpr float kCenterMaxY = 0.60f;
    static constexpr float kAmpMinX = 0.16f;
    static constexpr float kAmpMaxX = 0.26f;
    static constexpr float kAmpMinY = 0.12f;
    static constexpr float kAmpMaxY = 0.22f;

    // ---- 聚光扫过带 ----
    static constexpr float kSweepPeriod = 14.0f;      // 秒（左→右一次）
    static constexpr float kSweepWidth = 220.0f;      // px
    static constexpr float kSweepAlpha = 0.05f;       // 峰值 alpha
    static constexpr float kSweepTiltRad = 0.31415926f;  // 18°
    static constexpr float kSweepShadowBlur = 48.0f;  // 边缘柔化（无硬边）
    static constexpr float kSweepShadowAlpha = 0.10f;
    static constexpr float kSweepHeightFactor = 1.5f; // 带高 = 页高 × 1.5（两端出画）

    // ---- 驱动节拍与休眠 ----
    // 驱动用 onTimer 而不是 onFrame：光晕要一直动（用户要的「聚光灯/霓虹」），
    // 但不是**每帧重绘**——30fps 一个节拍，慢漂移肉眼看不出台阶，绘制/合成
    // 开销减半。dt 固定 kFrameSeconds（渲染节奏解耦，位置是时间的纯函数）。
    static constexpr float kFrameSeconds = 1.0f / 30.0f;
    // 空闲多久后休眠。旧值 2s 是「几乎看不到」的根因之一：人看页面时早停了。
    // 现在窗口打开 / 有任何指针交互后连续动效 120s，之后全静止 + 撤销驱动
    //（compose 不再挂 onTimer → 运行时 animating 归零 → 零重绘零合成）；
    // 任意指针移动（bg.onMove）立即唤醒。休眠机制本身（awake() 单点开关）
    // 与旧版一致，只是阈值从 2s 抬到 120s。
    static constexpr float kWakeHoldSeconds = 120.0f;

    std::array<GlowBlob, kBlobCount> blobs;
    float time = 0.0f;       // 动画时间（秒；休眠时冻结）
    float wakeHold = 0.0f;   // 距上次指针移动的秒数
    bool initialized = false;

    // 休眠判定：空闲超时 → false（compose 撤掉 onTimer，运行时不再出帧）
    bool awake() const { return wakeHold < kWakeHoldSeconds; }

    // 每个驱动节拍推进一次（dt 恒 kFrameSeconds；休眠期内 time 冻结，
    // 画面停在当前位置而不是跳变）
    void step(float dt) {
        wakeHold += dt;
        if (wakeHold < kWakeHoldSeconds) {
            time += dt;
        }
    }
};

// 层半径（px）：blobRadius × (0.42 + 0.116k)
inline float blobLayerRadius(float blobRadius, int layer) {
    return blobRadius * (AboutAnimState::kLayerRadiusInner +
                         AboutAnimState::kLayerRadiusStep *
                             static_cast<float>(layer));
}

// 层基础 alpha（不叠呼吸）：最内层 kLayerAlphaMax → 最外层 kLayerAlphaMin
inline float blobLayerAlpha(int layer) {
    constexpr float span = static_cast<float>(AboutAnimState::kLayersPerBlob - 1);
    const float t = static_cast<float>(layer) / span;
    return AboutAnimState::kLayerAlphaMax +
           (AboutAnimState::kLayerAlphaMin - AboutAnimState::kLayerAlphaMax) * t;
}

// 呼吸系数：1 ± kBreathDepth（各光斑相位/周期不同，互不同步）
inline float blobBreath(const AboutAnimState& state, int index) {
    const GlowBlob& blob = state.blobs[static_cast<std::size_t>(index)];
    const float omega = 6.28318531f / blob.breathPeriod;
    return 1.0f + AboutAnimState::kBreathDepth *
                      std::sin(omega * state.time + blob.breathPhase);
}

// 利萨如漂移位置（px）：中心 + 振幅 × sin(2π t / 周期 + 相位)，
// 两轴周期不成整数比（y = 0.62x）→ 不闭合的花瓣路径，长时间看不重复
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

// 扫带左边缘（px）：-带宽 → 页宽 + 带宽（一个周期走完，含两端出画）
inline float sweepLeft(const AboutAnimState& state, float w) {
    const float progress =
        std::fmod(state.time, AboutAnimState::kSweepPeriod) /
        AboutAnimState::kSweepPeriod;
    const float travel = w + AboutAnimState::kSweepWidth * 2.0f;
    return -AboutAnimState::kSweepWidth + progress * travel;
}

// 首次进入页面：按固定种子散布光斑（可复现）——半径 / 中心 / 振幅 / 周期 / 相位。
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
        // 相位互相错开 2π/3（三个光斑不同步，不会一起呼吸像闪灯）
        const float phaseOffset = 6.28318531f * static_cast<float>(i) /
                                  static_cast<float>(AboutAnimState::kBlobCount);
        blob.phaseX = phaseOffset + rng.next() * 0.6f;
        blob.phaseY = phaseOffset * 1.7f + rng.next() * 0.6f + 1.57079633f;
        const float breathT = rng.next();
        blob.breathPeriod = AboutAnimState::kBreathPeriodMin +
                            breathT * (AboutAnimState::kBreathPeriodMax -
                                       AboutAnimState::kBreathPeriodMin);
        blob.breathPhase = phaseOffset + rng.next() * 0.8f;
        // 配色各半：primary（0）/ 半程混色（0.5）/ 暖琥珀（1.0）
        blob.colorMix = static_cast<float>(i) /
                        static_cast<float>(AboutAnimState::kBlobCount - 1);
    }
    state.time = 0.0f;
    state.wakeHold = 0.0f;
    state.initialized = true;
}

} // namespace module::about
