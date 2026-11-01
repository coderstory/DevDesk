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
constexpr float kWtabSmallFontSize = 13.0f;    // S2.2 弹窗级小 Tab 字号

// 2026-11-01 统一：Tab 高度此前有 32 / 36 / 40 三档并存（memory 36、settings/md/
// wallpaper/release-config/claude-detail 40、claude-form 32），同一个组件在不同页面
// 高度不同，跨页切换时 pill 高矮不一。定死两档：
//   页面级 = 36（页内分区切换）
//   弹窗级 = 32（弹窗内小 tab，如 Profile 表单的 3 档）
constexpr float kWtabHeight = 36.0f;
constexpr float kWtabSmallHeight = 32.0f;

// 选中态底色比例：以 primary 按 14% 叠在页面底上（≈#D9F5E4 on #F5FAF7）。
// 原用不透明 primarySoftBg #E8F8EE，与页面底 #F5FAF7 的 RGB 差 < 13，
// 肉眼看不出「选中」——settings 页的 tab 因此退化成第三种样式（纯文字）。
// 走 primary 派生而非新增 token：4 档强调色主题自动跟随，不引入新令牌。
constexpr float kWtabSelectedBgOpacity = 0.14f;

// 渲染单 Tab（块面式）：stack(id) 内含 rect.bg（active 时 primarySoftBg + radius 8）
// + rect.hit（透明 hit area 挂 onClick）+ text（文字）。
// 非 active 时不创建 bg rect，无下划线。
// 几何：(x, y) 左上角，(w, h) 尺寸；调用方决定宽高。
// fontSize：默认 kWtabFontSize（页面级 14px）；弹窗级小 Tab 传 kWtabSmallFontSize（13px）。
// 节点 id = `<id>.bg`（active 时底色 rect，非 active 时不存在）
// / `<id>.hit`（透明 hit rect）/ `<id>.text`（文字），单测按此定位。
void wtab(eui::Ui& ui, const std::string& id, const std::string& label,
          bool active, float x, float y, float w, float h,
          const core::theme::Tokens& t,
          const std::function<void()>& onClick = {},
          float fontSize = kWtabFontSize);

} // namespace ui_kit
