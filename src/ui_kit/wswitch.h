// src/ui_kit/wswitch.h
// presentation 能力层：微信风格开关 helper（spec §4.4）。
// 本层被 module_* 与 app 层链接，core / platform 不得反向依赖；被各页面共用。
//
// W2c 新增：与既有开关渲染并存（spec D-03「新旧并存、逐页切换」），既有页面
// 开关调用不动，W3 / W4 逐页改调本 helper。色值走 wechat 令牌
//（core::theme::current().colors.wechat.*），禁裸 hex（D-03 / §8 #11）。
#pragma once

#include <core/theme.h>
#include <eui/dsl.h>
#include <eui/types.h>

#include <functional>
#include <string>

namespace ui_kit {

// 开关常量（§4.4）：滑块与轨道留 2px 视觉留白；圆形滑块 + 圆角轨道（pill）。
constexpr float kWswitchThumbPadding = 2.0f;   // 滑块与轨道内缘的留白

// 渲染单开关：stack(id) 内含 rect.track（pill 轨道，挂 onClick 切换）+
// rect.thumb（白色圆形滑块，位置按 on 左 / 右切换）。
// 几何：(x, y) 左上角，(w, h) 尺寸；调用方决定宽高（典型 w=44, h=24）。
// 节点 id = `<id>.track`（轨道 rect）/ `<id>.thumb`（滑块 rect），单测按此定位。
// spec §4.4 视觉：
//   关 = 灰轨道（wechat.dividerSecondary #EDEDED）+ 白滑块；
//   开 = wechat.primary（#07C160）绿轨道 + 白滑块。
// spec §4.4 禁止项：开关非绿 / 非灰——本 helper 走 wechat 令牌派生，绿 / 灰
// 二态钉死。滑块位移用简单 position（不强制动画；EUI 若有 transition 可后续加 .animate）。
void wswitch(eui::Ui& ui, const std::string& id, bool on,
             float x, float y, float w, float h,
             const core::theme::Tokens& t,
             const std::function<void(bool)>& onToggle = {});

} // namespace ui_kit
