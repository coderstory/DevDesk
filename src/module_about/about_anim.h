// src/module_about/about_anim.h
// 关于页漂浮圆环背景的动画状态（Spring 物理 + awake 休眠）。
//
// 为什么独立成头（2026-09-28）：about_module.cpp 消费它，单测
//（tests/test_modules/test_ui_modern.cpp 的 uiAboutRingAnim）直接驱动
// step()/awake() 断言「可见度区间 / 真的在动 / 静止后休眠」——留在 .cpp 的
// 匿名命名空间里单测够不着，可见度回归（用户真机反馈「看不出动态效果」）
// 就没有守卫。纯值类型 + inline 函数，零 EUI 依赖（环色在 compose 侧按
// 当前主题现算，见 about_module.cpp 的 mixThemeColor）。
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace module::about {

// 极小确定性 RNG（golden-ratio LCG）：初始散布每次启动可复现，
// 免 <random> 依赖（零头文件成本）。
struct RingRng {
    std::uint64_t s;
    explicit RingRng(std::uint64_t seed) : s(seed) {}
    float next() {  // [0,1)
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<float>((s >> 33) & 0xFFFFFFu) / 16777216.0f;
    }
};

// 半隐式欧拉弹簧（framer-motion useSpring 同款）：软跟随漂移锚点，
// 静止后 settled() 为 true → awake() 注销帧回调 -> 休眠。
struct Spring {
    float position = 0, velocity = 0, target = 0, stiffness = 80, damping = 14, mass = 1;
    void setTarget(float t) { target = t; }
    void update(float dt) {
        // 半隐式欧拉
        float force = stiffness * (target - position);
        velocity = (velocity + force / mass * dt) / (1.0f + damping / mass * dt);
        position += velocity * dt;
    }
    bool settled() const { return std::abs(target - position) < 0.5f && std::abs(velocity) < 1.0f; }
};

// 单个浮环：静态参数 + 漂移锚点（x/y）；绘制位置 = 弹簧跟随位
//（锚点直线漂移 + 边界弹回，弹簧软跟 → trailing 有机感）。
struct FloatingRing {
    float x = 0.0f, y = 0.0f;  // 漂移锚点（逻辑圆点；边界弹回作用于此）
    float radius = 0.0f;       // kRadiusMin-Max px
    float opacity = 0.0f;      // kOpacityMin-Max（元素级透明度）
    float speedX = 0.0f, speedY = 0.0f;  // px/s，正负 kSpeedMin-Max
    float phase = 0.0f;        // 0-1：compose 侧按半程映射成混色系数（见 .cpp）
};

struct AboutAnimState {
    static constexpr int kRingCount = 7;            // 需求区间 6-8
    static constexpr float kWakeHoldSeconds = 2.0f; // 鼠标静止多久后休眠

    // ---- 可见度参数（2026-09-28 用户真机反馈「看不出动态效果」）----
    // 元素级透明度旧值 0.06-0.15：浅色档 surface #F5F2EC 上 1px 环线
    // 合成后 ΔRGB ≈ 2-4，肉眼等同不可见 → 上调到 0.18-0.28：明确可辨为
    // 装饰圆环，又不与卡片/正文抢注意力。单测断言此区间（uiAboutRingAnim）。
    static constexpr float kOpacityMin = 0.18f;
    static constexpr float kOpacityMax = 0.28f;
    // 半径 12-34px：相对 900 内容区是「点状」小装饰体量（卡片上下堆叠后
    // 页面留白变多，仍不构成干扰）
    static constexpr float kRadiusMin = 12.0f;
    static constexpr float kRadiusMax = 34.0f;
    // 漂移速度 15-30 px/s：唤醒窗口（kWakeHoldSeconds）内位移 30-60px，
    // 肉眼可辨「在动」；再快会与静态信息页的调性冲突
    static constexpr float kSpeedMin = 15.0f;
    static constexpr float kSpeedMax = 30.0f;

    std::array<FloatingRing, kRingCount> rings;
    std::array<Spring, kRingCount> springX;         // x 轴弹簧（追锚点）
    std::array<Spring, kRingCount> springY;
    float wakeHold = 0.0f;   // 距上次鼠标移动的秒数
    bool initialized = false;

    // 全弹簧 settled + 超时 → 休眠（compose 不再挂 onFrame）
    bool awake() const {
        if (wakeHold < kWakeHoldSeconds) return true;
        for (int i = 0; i < kRingCount; ++i) {
            if (!springX[i].settled() || !springY[i].settled()) return true;
        }
        return false;
    }

    // 每帧：唤醒期内锚点直线漂移 + 碰撞边界弹回；弹簧软跟随。
    // 休眠前最后一帧仍推进弹簧，让位置收敛到锚点再撤回调。
    void step(float dt, float w, float h) {
        wakeHold += dt;
        const bool drift = wakeHold < kWakeHoldSeconds;
        for (int i = 0; i < kRingCount; ++i) {
            FloatingRing& ring = rings[i];
            Spring& sx = springX[i];
            Spring& sy = springY[i];
            if (drift) {
                ring.x += ring.speedX * dt;
                ring.y += ring.speedY * dt;
                const float m = ring.radius + 6.0f;  // 边界 margin（含弹簧 lag 余量）
                const float loX = m;
                const float hiX = std::max(m, w - m);
                if (ring.x <= loX) { ring.x = loX; ring.speedX = std::abs(ring.speedX); }
                else if (ring.x >= hiX) { ring.x = hiX; ring.speedX = -std::abs(ring.speedX); }
                const float loY = m;
                const float hiY = std::max(m, h - m);
                if (ring.y <= loY) { ring.y = loY; ring.speedY = std::abs(ring.speedY); }
                else if (ring.y >= hiY) { ring.y = hiY; ring.speedY = -std::abs(ring.speedY); }
                sx.setTarget(ring.x);
                sy.setTarget(ring.y);
            }
            sx.update(dt);
            sy.update(dt);
        }
    }
};

// 首次进入页面：随机散布浮环（位置/半径/速度/透明度/色 mix 系数）。
// 弹簧初始 position = target → 直接 settled，首帧即可静默。
inline void initRingLayer(AboutAnimState& state, float w, float h) {
    RingRng rng(0x9E3779B97F4A7C15ULL);
    for (int i = 0; i < AboutAnimState::kRingCount; ++i) {
        FloatingRing& ring = state.rings[i];
        ring.radius = AboutAnimState::kRadiusMin +
                      rng.next() * (AboutAnimState::kRadiusMax - AboutAnimState::kRadiusMin);
        const float m = ring.radius + 6.0f;
        ring.x = m + rng.next() * std::max(1.0f, w - 2.0f * m);
        ring.y = m + rng.next() * std::max(1.0f, h - 2.0f * m);
        const float sp = AboutAnimState::kSpeedMin +
                         rng.next() * (AboutAnimState::kSpeedMax - AboutAnimState::kSpeedMin);
        ring.speedX = rng.next() < 0.5f ? -sp : sp;
        ring.speedY = rng.next() < 0.5f ? -sp : sp;
        ring.opacity = AboutAnimState::kOpacityMin +
                       rng.next() * (AboutAnimState::kOpacityMax - AboutAnimState::kOpacityMin);
        ring.phase = rng.next();
        state.springX[i].setTarget(ring.x);
        state.springX[i].position = ring.x;
        state.springY[i].setTarget(ring.y);
        state.springY[i].position = ring.y;
    }
    state.wakeHold = 0.0f;
    state.initialized = true;
}

} // namespace module::about
