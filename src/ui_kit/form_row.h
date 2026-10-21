// src/ui_kit/form_row.h
// presentation 能力层：表单栅格 helper（spec §4.15）。
// W2b 新增：label 定宽 110px + control 填满剩余宽；垂直居中对齐。
// 与既有表单行渲染并存，逐页切换调用本 helper。
#pragma once

#include <core/theme.h>
#include <eui/dsl.h>

#include <functional>
#include <string>

namespace ui_kit {

// label 定宽（spec §4.15 区间 100-120px 取中值 110）：同表单内 label 宽度一致，
// 避免标签长短不一导致 control 起始位置漂移。
constexpr float kFormRowLabelWidth = 110.0f;

// control 区 compose 回调：消费方在回调里用 (controlX, controlY, controlW, controlH)
// 画自己的输入框/开关/下拉等控件。label 与 control 垂直居中于 totalH。
using ControlComposeFn = std::function<void(eui::Ui& ui, float controlX, float controlY,
                                            float controlW, float controlH)>;

// 渲染单表单行：左侧 label（定宽 110px）+ 右侧 control 区（填满剩余宽），
// 垂直居中对齐。label 与 control 间距用 t.sizes.spacing（8px，§3.1 8px 网格）。
// 节点 id = `<id>.label`（text）；control 由消费方在回调里画（自行命名 id）。
// label 文字色走 wechat.textPrimary，字号 14px 基准 × 全局字号档（§3.3 主文字）。
void formRow(eui::Ui& ui, const std::string& id, const std::string& label,
             float x, float y, float totalW, float totalH,
             const ControlComposeFn& composeControl,
             const core::theme::Tokens& t);

} // namespace ui_kit
