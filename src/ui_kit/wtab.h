// src/ui_kit/wtab.h
// presentation 能力层：微信风格 Tab helper（spec §4.2）。
// 本层被 module_* 与 app 层链接，core / platform 不得反向依赖；被各页面共用。
//
// W2c 新增：与既有 Tab 渲染并存（spec D-03「新旧并存、逐页切换」），既有页面
// Tab 调用不动，W3 / W4 逐页改调本 helper。色值走 wechat 令牌
//（core::theme::current().colors.wechat.*），禁裸 hex（D-03 / §8 #11）。
#pragma once

#include <core/theme.h>
#include <eui/dsl.h>
#include <eui/types.h>

#include <functional>
#include <string>

namespace ui_kit {

// Tab 常量（§4.2）：2px 底部下划线、14px 主文字。
constexpr float kWtabUnderlineHeight = 2.0f;   // §4.2 "底部 2px 微信绿下划线"
constexpr float kWtabFontSize = 14.0f;         // §3.3 主文字

// 渲染单 Tab：stack(id) 内含 rect.hit（透明 hit area 挂 onClick）+ text（文字）
// + rect.underline（active 时 2px 底部下划线，非 active 不创建）。
// 几何：(x, y) 左上角，(w, h) 尺寸；调用方决定宽高。
// 节点 id = `<id>.hit`（透明 hit rect）/ `<id>.text`（文字）/ `<id>.underline`
//（active 时下划线 rect，非 active 时不存在），单测按此定位。
// spec §4.2 禁止项：使用实心填充按钮作为 Tab 切换——本 helper 不画填充背景，
// 只文字 + 下划线；hit area 透明（alpha=0），不构成"实心填充"。
// 下划线宽 = 调用方传入的 w（tab 槽宽）；若需贴合文字宽，调用方传 w=文字宽。
void wtab(eui::Ui& ui, const std::string& id, const std::string& label,
          bool active, float x, float y, float w, float h,
          const core::theme::Tokens& t,
          const std::function<void()>& onClick = {});

} // namespace ui_kit
