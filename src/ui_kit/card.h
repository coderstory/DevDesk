// src/ui_kit/card.h
// presentation 能力层（任务 D / R6）：卡片底板绘制。
//
// 分层依据同 toast.h / dialog_form.h / metrics.h（spec
// 2026-09-27-refactor-incremental-design §4.1）：本层被 module_* 与 app 层
// 链接，core / platform 不得反向依赖本层。
//
// 收编对象（spec §4.1 card 行 / §5.2 R6）：settings_module.cpp 的私有
// cardFrame（现 settings 9 张卡各调一次）。提升为公共后零行为变化——同一实现
// 原样搬出，仅去掉「私有成员函数」身份（不再读任何成员）。
#pragma once

#include <core/theme.h>
#include <eui/dsl.h>

#include <string>

namespace ui_kit {

// 卡片底板：表面色矩形 + 大圆角 + 1px 描边，铺在卡根 stack 的 (0,0)。
//
// 节点 id 与拆分前逐字一致：`<id>.bg`（settings 侧 id 形如
// settings.card.appearance → settings.card.appearance.bg），单测按该 id 定位，
// 不得改。位置恒为 (0,0)、尺寸恒为 (w,h)——几何由调用方（卡根 stack）决定。
//
// 卡内容内边距口径（2026-09-28 系统性修复，全站统一）：卡片内边距四周 ≥16px
// （16px 网格；settings 卡惯用左右 28 = kDialogInsetX 语系、上下 16）。调用方
// 负责内容留白（本底只铺色）：底部内容底沿 + 16 ≤ 卡高，顶部首元素 y ≥ 16。
void cardFrame(eui::Ui& ui, const std::string& id, float w, float h,
               const core::theme::Tokens& t);

} // namespace ui_kit
