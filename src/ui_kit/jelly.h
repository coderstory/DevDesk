// src/ui_kit/jelly.h
// presentation 能力层：果冻卡（L2 业务卡 = 容器内一层条目）。半透底 + 描边 + 阴影 +
// hover 一次性「压扁-拉伸-回弹」时间线（need201 v2）。只给选择项 / 按钮这类 L2
// 交互控件用，L1 容器卡不用。
//
// v2 触发模型（need201 基线）：hover 进入沿（per-card 边沿检测）起表走 560ms 时间线；
// hover 保持期动画走完回正常态（12/1.0，不是旧版 1.02 悬停态）；hover 退出中断 →
// 目标置常态、0.18s OutBack 平滑回收；再次进入重新触发。动画进行中 zIndex(20)
// 盖过邻卡，结束后回落默认 0。
#pragma once

#include <core/component_theme.h>  // core::theme::componentTokensWeChat（panelShadow 入参）
#include <core/theme.h>            // core::theme::Tokens
#include <components/theme.h>      // components::theme::panelShadow
#include <eui/dsl.h>               // eui::Ui / RectBuilder
#include <ui_kit/glass.h>          // glassFill（与 itemSurface 同源）

#include <chrono>
#include <cmath>
#include <functional>
#include <ratio>
#include <string>
#include <unordered_map>
#include <utility>

namespace ui_kit {

struct JellyOptions {
    // ── need201 v2 时间线基线（定稿照抄勿自创；调参只动这组常量）─────────
    // P1 压扁 [0,90]    (1,1)→(kP1Sx,kP1Sy) easeOutQuad，radius 12→kP1Radius，w=0
    // P2 拉伸 (90,240]  →(kP2Sx,kP2Sy) easeInOut，radius→kP2Radius（过冲感）
    // P3 回摆 (240,420] →(kP3Sx,kP3Sy) 线性（基线未指定缓动），radius→12
    // P4 回正 (420,560) →(1,1) easeOutCubic，radius 保持 12
    // 扭摆 P2 起叠加：w(t)=kWobbleAmp·sin(2π·kWobbleFreq·t)·exp(−kWobbleDecay·t)
    //（t 秒，自 hover 起算），scaleX=s·(1+w)、scaleY=s·(1−w)（差分反相）。
    static constexpr float kRestRadius = 12.0f;  // 时间线基准常态圆角
    static constexpr float kSquashMs = 90.0f;    // P1 结束
    static constexpr float kStretchMs = 240.0f;  // P2 结束
    static constexpr float kReboundMs = 420.0f;  // P3 结束
    static constexpr float kTotalMs = 560.0f;    // P4 结束 = 动画窗
    static constexpr float kP1Sx = 1.04f;
    static constexpr float kP1Sy = 0.90f;
    static constexpr float kP1Radius = 10.0f;
    static constexpr float kP2Sx = 0.96f;
    static constexpr float kP2Sy = 1.07f;
    static constexpr float kP2Radius = 17.0f;
    static constexpr float kP3Sx = 1.02f;
    static constexpr float kP3Sy = 0.99f;
    static constexpr float kWobbleAmp = 0.04f;
    static constexpr float kWobbleFreq = 2.2f;
    static constexpr float kWobbleDecay = 3.5f;

    float radius = 12.0f;       // 常态圆角（时间线半径随它平移，见 jellyCard）
    float hoverRadius = 16.0f;  // hover 更圆 = 更软。v2 起 jellyCard 的 hover 形变
                                // 走时间线不再消费本字段；保留给手写果冻段
                                //（agent/sidebar）取值复用，字段语义未动。
    float pressedRadius = 8.0f;   // 按下压扁
    float hoverScale = 1.02f;     // 同 hoverRadius：v2 jellyCard 不消费，手写段复用
    float pressedScale = 0.97f;
    // 按压形变来源元素 id（通常 = 卡的命中面 <id>.hit，命中面是兄弟中最上层）。
    // 非空时对卡本体挂 visualStateFrom(该 id, pressedScale)——按下果冻由运行时
    // 依赖式驱动（源元素须同帧存在），hover 形变走时间线。
    std::string pressedFrom;
};

// 时间线姿态（纯数据）：jellyPose 的返回值。radius 为基线绝对值（常态 12），
// jellyCard 侧随 opt.radius 平移。
struct JellyPose {
    float sx;      // 水平缩放目标
    float sy;      // 垂直缩放目标
    float radius;  // 圆角目标（基线绝对值）
    bool active;   // 时间线窗 [0, 560ms) 内 true
};

namespace detail {

// 边沿检测状态（键 = 元素 id，存 hover 边沿与起表时刻）
struct JellyAnimState {
    bool wasHovered = false;
    bool animating = false;
    std::chrono::steady_clock::time_point t0{};
};

// C++17 inline 函数局部 static：头文件多 TU 包含也是全程序单实例。键=元素 id，
// hover id 集合有限不淘汰（need201 明令）；map 值直存，无裸指针。
inline std::unordered_map<std::string, JellyAnimState>& jellyAnimTable() {
    static std::unordered_map<std::string, JellyAnimState> table;
    return table;
}

inline float easeOutQuad(float p) { return 1.0f - (1.0f - p) * (1.0f - p); }
inline float easeInOutQuad(float p) {
    return p < 0.5f ? 2.0f * p * p
                    : 1.0f - (-2.0f * p + 2.0f) * (-2.0f * p + 2.0f) * 0.5f;
}
inline float easeOutCubic(float p) {
    return 1.0f - (1.0f - p) * (1.0f - p) * (1.0f - p);
}

// 左右扭摆：差分衰减振荡（P2 起叠加；t 自 hover 起算，单位秒）
inline float jellyWobble(float elapsedMs) {
    const float tSec = elapsedMs * 0.001f;
    return JellyOptions::kWobbleAmp *
           std::sin(6.2831853f * JellyOptions::kWobbleFreq * tSec) *
           std::exp(-JellyOptions::kWobbleDecay * tSec);
}

}  // namespace detail

// 果冻时间线姿态（纯函数，不碰 clock，可确定性单测）：
// 输入自 hover 进入沿起算的毫秒数，输出该时刻的缩放/圆角目标与 active 窗口标记。
// 相位端点约定：t=90 归 P1（压扁端点精确命中基线值）、t=240 归 P2（拉伸端点）。
inline JellyPose jellyPose(float elapsedMs) {
    using JO = JellyOptions;
    if (!(elapsedMs >= 0.0f) || elapsedMs >= JO::kTotalMs) {
        return {1.0f, 1.0f, JO::kRestRadius, false};  // 窗外（含负/NaN）= 常态
    }
    float sx = 1.0f, sy = 1.0f, radius = JO::kRestRadius, w = 0.0f;
    if (elapsedMs <= JO::kSquashMs) {  // P1 压扁：easeOutQuad，压扁本身已含水平鼓出
        const float e = detail::easeOutQuad(elapsedMs / JO::kSquashMs);
        sx = 1.0f + (JO::kP1Sx - 1.0f) * e;
        sy = 1.0f + (JO::kP1Sy - 1.0f) * e;
        radius = JO::kRestRadius + (JO::kP1Radius - JO::kRestRadius) * e;
    } else if (elapsedMs <= JO::kStretchMs) {  // P2 拉伸：easeInOut + 扭摆起
        const float p =
            (elapsedMs - JO::kSquashMs) / (JO::kStretchMs - JO::kSquashMs);
        const float e = detail::easeInOutQuad(p);
        sx = JO::kP1Sx + (JO::kP2Sx - JO::kP1Sx) * e;
        sy = JO::kP1Sy + (JO::kP2Sy - JO::kP1Sy) * e;
        radius = JO::kP1Radius + (JO::kP2Radius - JO::kP1Radius) * e;
        w = detail::jellyWobble(elapsedMs);
    } else if (elapsedMs <= JO::kReboundMs) {  // P3 回摆：基线未指定缓动 → 线性
        const float p =
            (elapsedMs - JO::kStretchMs) / (JO::kReboundMs - JO::kStretchMs);
        sx = JO::kP2Sx + (JO::kP3Sx - JO::kP2Sx) * p;
        sy = JO::kP2Sy + (JO::kP3Sy - JO::kP2Sy) * p;
        radius = JO::kP2Radius + (JO::kRestRadius - JO::kP2Radius) * p;
        w = detail::jellyWobble(elapsedMs);
    } else {  // P4 回正：easeOutCubic
        const float p =
            (elapsedMs - JO::kReboundMs) / (JO::kTotalMs - JO::kReboundMs);
        const float e = detail::easeOutCubic(p);
        sx = JO::kP3Sx + (1.0f - JO::kP3Sx) * e;
        sy = JO::kP3Sy + (1.0f - JO::kP3Sy) * e;
        radius = JO::kRestRadius;  // P3 已线性回 12，P4 保持
        w = detail::jellyWobble(elapsedMs);
    }
    sx *= 1.0f + w;  // 左右扭摆：差分反相叠加
    sy *= 1.0f - w;
    return {sx, sy, radius, true};
}

// 果冻时间线公开缝：手写卡段（如 agent 卡 bg——材质走 states 非 glassFill，不能
// 整卡换 jellyCard）用它取形变目标值。内部与 jellyCard 消费同一状态表/边沿检测
//（同 id 同 hovered 序列 → 同 pose，单测护着）。返回 radius 为基线绝对值口径
//（常态 12；调用方 .radius() 直接写入，常态卡=kJelly 默认档正好无缝）。
// pressed 优先级由调用方自理：pressed 值分支盖过本返回值时，时间线仍在同一状态表
// 后台推进（与 visualStateFrom 通道现状一致）。
inline JellyPose jellyFrame(const std::string& id, bool hovered) {
    detail::JellyAnimState& st = detail::jellyAnimTable()[id];
    if (hovered && !st.wasHovered) {
        st.t0 = std::chrono::steady_clock::now();
        st.animating = true;
    }
    if (!hovered) st.animating = false;  // 退出中断：目标置常态
    st.wasHovered = hovered;
    float elapsedMs = -1.0f;
    if (st.animating) {
        elapsedMs = std::chrono::duration<float, std::milli>(
                        std::chrono::steady_clock::now() - st.t0)
                        .count();
        if (elapsedMs >= JellyOptions::kTotalMs) {
            st.animating = false;  // 走完 → 正常态
            elapsedMs = -1.0f;
        }
    }
    if (elapsedMs < 0.0f) {
        return {1.0f, 1.0f, JellyOptions::kRestRadius, false};
    }
    return jellyPose(elapsedMs);
}

// ── 测试缝（只读/清场，不改运行语义）：探针 A0v2 需 t0 换算逐帧 elapsed ──
inline void jellyResetForTests() { detail::jellyAnimTable().clear(); }
inline bool jellyAnimStartForTests(
    const std::string& id, std::chrono::steady_clock::time_point& outT0) {
    const auto& table = detail::jellyAnimTable();
    const auto it = table.find(id);
    if (it == table.end() || !it->second.animating) return false;
    outT0 = it->second.t0;
    return true;
}

// 果冻卡：`hovered` 必须由调用方**每帧 compose 时**传（来源见方案 §10.0.1 的 hover
// 状态集合）。传常量 false 的话本函数就是个静态玻璃卡——果冻永远不会动，设计如此。
//
// 五条坑（逐条读过 EUI-NEO 源码，任一条不满足都是静默失效：不报错、不崩、只是没效果）：
// ① `.transition()` + `.animate()` 只是「允许动」，不会自己动。目标值来自 compose 期
//    写入的 element.radius（runtime_update.h 的 instance.radius.setTarget(element.radius)），
//    而 `.states()` 只改颜色（dsl.h）改不了圆角 → 圆角想动必须每帧 compose 出不同的
//    radius 值（v2：动画期每帧写 jellyPose(elapsed)），动画只负责平滑追目标。
// ② `.scale()` 同理是 compose 期写入的静态值 → v2 逐轴写 (sx,sy)（扭摆差分反相
//    已烘进 jellyPose 返回值），每帧换值。
// ③ `.transformOrigin` 是归一化坐标，默认 (0.5, 0.5) 即卡中心；不写也对，但**不能改成
//    0/1 之类**，否则 hover 时整卡朝角落缩、看着像错位。
// ④ 按下态别在这里自己算 scale：EUI 的 `.visualStateFrom(id + ".bg", pressedScale)` 会把
//    pressedScale clamp 到 [0.80, 1.0]（dsl.h）——0.97 合法，0.9 会被抬回 0.97 一档。
//    且它是依赖式的，源元素必须同帧存在。
// ⑤ `hoverOpacityFrom` 与 `runtimePointerTransformFrom` 不要挂在同一个元素上（都写
//    opacity/transform 同一通道会打架）。卡本体形变走本函数，操作组显隐走前者。
inline void jellyCard(eui::Ui& ui, const std::string& id, float x, float y,
                      float w, float h, const core::theme::Tokens& t,
                      const JellyOptions& opt = {},
                      bool hovered = false,
                      std::function<void()> onClick = {}) {
    // v2：hover 进入沿起表（边沿检测），动画期每帧 compose 时间线目标值。
    // 状态表/边沿/t0 逻辑在 jellyFrame——jellyCard 与手写段（jellyFrame 直调）
    // 同源消费，行为一致。
    JellyPose pose = jellyFrame(id, hovered);
    // 时间线半径以基线常态 12 为基准，随 opt.radius 平移（schedule 卡用
    // radiusLarge，非 12：不平移则动画结束会跳回基线值）
    pose.radius += opt.radius - JellyOptions::kRestRadius;

    auto r = ui.rect(id)
        .position(x, y)
        .size(w, h)
        .color(glassFill(t))
        // 坑①：圆角目标值每帧重新给；动画中 0.035s 紧贴目标，非动画 0.18s 回收
        .radius(pose.radius)
        // 坑③：缩放中心必须在卡中心
        .transformOrigin(0.5f, 0.5f)
        // 坑②：逐轴缩放目标值每帧重新给
        .scale(pose.sx, pose.sy)
        .border(1.0f, t.colors.borderDefault)
        .shadow(components::theme::panelShadow(
            ::core::theme::componentTokensWeChat(t)))
        .transition(pose.active
                        ? core::Transition::make(0.035f, core::Ease::Linear)
                        : core::Transition::make(0.18f, core::Ease::OutBack))
        .animate(core::AnimProperty::Radius | core::AnimProperty::Transform);
    if (pose.active) {
        r.zIndex(20);  // 动画进行中盖过邻卡；走完不写（回落默认 0）
    }
    if (!opt.pressedFrom.empty()) {
        r.visualStateFrom(opt.pressedFrom, opt.pressedScale);
    }
    if (onClick) r.onClick(std::move(onClick));
    r.build();
}

} // namespace ui_kit
