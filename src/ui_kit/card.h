// src/ui_kit/card.h
// presentation 能力层：卡片底板绘制。本层被 module_* 与 app 层链接，core / platform
// 不得反向依赖；被 settings 各卡共用。
#pragma once

#include <core/theme.h>
#include <eui/dsl.h>
#include <ui_kit/glass.h>
#include <ui_kit/metrics.h>

#include <functional>
#include <string>

namespace ui_kit {

// 卡片表面色：玻璃三件套里的「填充」一件（glassFill 单一来源，含半透 alpha + 微绿染）
inline eui::Color cardSurface(const core::theme::Tokens& t) {
    return glassFill(t);
}

// 卡片底板：表面色矩形 + 大圆角 + 1px 描边，铺在卡根 stack 的 (0,0)；几何由调用方决定。
// 节点 id = `<id>.bg`（单测按此定位，不得改）。内边距四周 ≥16px，调用方负责内容留白。
void cardFrame(eui::Ui& ui, const std::string& id, float w, float h,
               const core::theme::Tokens& t);

// 卡片底板（绝对定位版）：memory 等模块直接在页面坐标系里画卡、不套 stack 根，
// 用它保证全站卡片同材质 + 同描边 + 同阴影。此前记忆页卡片只有 1px 浅描边、无阴影，
// 在近白页面底上边界几乎不可辨——截图里「只见文字不见容器」（2026-11-01 F-02/F-07）。
// id 契约 = 调用方原 id（不追加 .bg），单测定位面不变。
// onClick：可选点击回调（运行历史卡片点选行用；为空 = 纯展示卡）。
void cardPanel(eui::Ui& ui, const std::string& id, float x, float y, float w,
               float h, float radius, const core::theme::Tokens& t,
               std::function<void()> onClick = {});

// 卡片底板 + 底色覆盖：弹窗内的卡片不能用默认玻璃面——弹窗底本身是 surface
// （浅色=纯白），白卡片压在白底上会「融」（描边 #E5E7EB 太淡，边界不可辨）。
// 调用方按主题给一档「比弹窗底可区分」的底色，描边与阴影仍走全站统一。
void cardPanelColored(eui::Ui& ui, const std::string& id, float x, float y, float w,
                      float h, float radius, const core::theme::Tokens& t,
                      const eui::Color& surface,
                      std::function<void()> onClick = {});

} // namespace ui_kit
