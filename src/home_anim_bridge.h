// src/home_anim_bridge.h
// 首页留白动画配置缝（need506 定稿：点阵波纹 × 打字机组合）。
// module-safe 零依赖头（toast_notice_bridge 模式）；实现 TU = src/home_anim_impl.cpp
// （devdesk_home_anim 小库，DevDesk / devdesk_tests / 探针三类消费方一次链接）。
#pragma once

#include <cstdint>
#include <optional>

namespace home_anim {

// 设置档：true = 组合动效（默认）/ false = 关（静态，零泵）。持久化归设置侧。
bool enabled();
void setEnabled(bool on);

// 系统减动效查询（override 缝供测试钉住；真值 = platform::reduced_motion）。
bool reducedMotion();
void setReducedMotionOverride(std::optional<bool> on);

// 泵门控：开 && 减动效关 && Router 当前页==Home && 主窗可见。
// drawFluidBackground 每帧查询推泵（app::compose 每帧都跑 → 切页/不可见下一帧即停）。
bool pumpWanted();

// 壁钟相位秒（本 TU 独立壁钟，不耦合 app::fluidPhaseSeconds——探针不拖 app 层）。
double phaseSeconds();

// 测试缝：运行时镜像回厂（缓存失效 + 减动效覆写复位）。
void resetForTests();

}  // namespace home_anim
