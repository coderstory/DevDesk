// src/ui_kit/wtab.h
// presentation 能力层：块面式 Tab helper（S2.2）。
// 本层被 module_* 与 app 层链接，core / platform 不得反向依赖；被各页面共用。
//
// S2.2 块面 Tab：选中 = primarySoftBg 底 + primary 文字 + 8px 圆角；
// 未选中 = 透明底 + textSecondary 文字。淘汰下划线式。
// 色值走 core::theme::Tokens 主槽位（theme.cpp 已迁 mint），禁裸 hex。
#pragma once

#include <core/theme.h>
#include <eui/dsl.h>
#include <eui/types.h>

#include <functional>
#include <string>

namespace ui_kit {

// Tab 常量（S2.2）：14px 主文字。
constexpr float kWtabFontSize = 14.0f;         // S2.2 页面级 Tab 字号

// 渲染单 Tab（块面式）：stack(id) 内含 rect.bg（active 时 primarySoftBg + radius 8）
// + rect.hit（透明 hit area 挂 onClick）+ text（文字）。
// 非 active 时不创建 bg rect，无下划线。
// 几何：(x, y) 左上角，(w, h) 尺寸；调用方决定宽高。
// 节点 id = `<id>.bg`（active 时底色 rect，非 active 时不存在）
// / `<id>.hit`（透明 hit rect）/ `<id>.text`（文字），单测按此定位。
void wtab(eui::Ui& ui, const std::string& id, const std::string& label,
          bool active, float x, float y, float w, float h,
          const core::theme::Tokens& t,
          const std::function<void()>& onClick = {});

} // namespace ui_kit
