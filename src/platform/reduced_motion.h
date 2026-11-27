// src/platform/reduced_motion.h
// 系统减动效查询（平台无关接口）：实现按平台拆文件
// （_win = SystemParametersInfo SPI_GETCLIENTAREAANIMATION；
//  _macos = NSWorkspace accessibilityDisplayShouldReduceMotion，objc runtime
//  C API，纯 .cpp 同 window_visibility_macos 手法）。
// 消费方：首页持续动画静态化降级（need506 §2.2 设计）。
#pragma once

#include <optional>

namespace platform::reduced_motion {

// 系统当前是否要求减少动态效果（macOS 27「减少动态效果」开 / Windows「动画效果」关）。
//
// 线程：仅主线程调用。语义：true = 常驻环境动画应静止（构图保留、运动归零）。
// 失败开放方向 = false（判定不了 → 不减动效，维持现状动画）——与
// window_visibility 的 fail-open 相反方向：这里是「不额外剥夺动画」，查询
// 失败不应让动画凭空消失。
bool systemReducedMotion();

// ── 测试缝 ──────────────────────────────────────────────────────────────
// 平台查询在单测里不可控（CI 机器的系统设置不同），override 只供测试钉住；
// 生产路径不设置 = 走真实平台查询。
namespace detail {
inline std::optional<bool>& overrideSlot() {
    static std::optional<bool> slot;
    return slot;
}
}  // namespace detail

inline void setReducedMotionOverride(std::optional<bool> v) {
    detail::overrideSlot() = v;
}
inline void clearReducedMotionOverride() { detail::overrideSlot().reset(); }

}  // namespace platform::reduced_motion
