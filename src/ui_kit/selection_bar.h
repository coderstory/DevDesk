// src/ui_kit/selection_bar.h
// presentation 能力层：微信风格选中态 helper（spec §4.3）。
// 本层被 module_* 与 app 层链接，core / platform 不得反向依赖；被各页面共用。
//
// W2c 新增：与既有选中态渲染并存（spec D-03「新旧并存、逐页切换」），
// 既有页面选中态调用不动，W3 nav / W4 settings 逐页改调本 helper。色值走
// wechat 令牌（core::theme::current().colors.wechat.*），禁裸 hex（D-03 / §8 #11）。
#pragma once

#include <core/theme.h>
#include <eui/dsl.h>
#include <eui/types.h>

#include <string>

namespace ui_kit {

// 选中态常量（S2.5）：3px 左竖线、14px 主文字、12px 文字左 padding。
constexpr float kSelectionBarAccentWidth = 3.0f;    // S2.5 "左侧 3px 主色竖线"
constexpr float kSelectionBarTextPadX = 12.0f;      // 文字距竖线的视觉留白
constexpr float kSelectionBarFontSize = 14.0f;       // S2.5 主文字

// 渲染单选中条：stack(id) 内含 rect.bg（浅灰底）+ rect.accent（2px 左竖线）
// + text（深灰文字，左对齐 + 垂直居中）。用于导航 / 列表 / 卡片选中态（W3 nav 用）。
// 几何：(x, y) 左上角，(w, h) 尺寸；调用方决定宽高。
// 节点 id = `<id>.bg`（底色 rect）/ `<id>.accent`（左竖线 rect）/ `<id>.text`（文字），
// 单测按此定位。
// spec §4.3 禁止项：红色 / 橙色粗外边框作普通选中态、小对勾标记——本 helper
// 加 border 描边（H5 白卡 elevation 代 shallow background）但不加 shadow / check，
// 只 3px 绿竖线 + 白卡 border + 深灰文字。
void selectionBar(eui::Ui& ui, const std::string& id, const std::string& label,
                  float x, float y, float w, float h,
                  const core::theme::Tokens& t);

} // namespace ui_kit
