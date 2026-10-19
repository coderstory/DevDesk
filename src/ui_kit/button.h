// src/ui_kit/button.h
// presentation 能力层：微信风格四级按钮 helper（spec §4.1）。
// 本层被 module_* 与 app 层链接，core / platform 不得反向依赖；被各页面共用。
//
// W2b 新增：与既有 button 渲染并存（spec D-03「新旧并存、逐页切换」），
// 既有页面 button 调用不动，W5-W10 逐页改调本 helper。色值走 wechat 令牌
//（core::theme::current().colors.wechat.*），禁裸 hex（spec D-03 / §8 #11）。
#pragma once

#include <core/theme.h>
#include <eui/dsl.h>
#include <eui/types.h>

#include <functional>
#include <string>

namespace ui_kit {

// 四级按钮（spec §4.1）：操作权重递减。同一视口主按钮唯一（规范自增）。
//   Primary    — 微信绿 #07C160 填充 + 白字：核心确认（新建/保存/启用/确认）
//   Secondary  — 浅灰底 #F2F2F2 + 深灰字：次要操作（取消/编辑/测试/导入）
//   Text       — 透明底 + 绿字（默认）：辅助操作（复制/查看详情/更多）
//   Danger     — 警示红 #FA5151 + 白字：删除/高危操作确认
enum class ButtonLevel {
    Primary,
    Secondary,
    Text,
    Danger,
};

// 统一尺寸（spec §4.1 D-12 校准）：四级按钮共用同一档，避免同视口高低错落。
// 钉死常量保 W2c/d 一致；圆角读 t.sizes.radiusWeChat（D-12 统一 6px）。
constexpr float kButtonHeight = 34.0f;     // 高度（区间 32-36 取中）
constexpr float kButtonPaddingX = 10.0f;   // 左右 padding（区间 8-12 取中）
constexpr float kButtonFontSize = 14.0f;   // 字号（§4.1 / §3.3 主文字）

// 按钮可选配置：onClick 回调 + 文字色覆盖。
// Text 级按语义选绿（wechat.primary，链接/可点）或灰（wechat.textSecondary，
// 弱化辅助）；其他级一般不覆盖文字色，走 level 默认配色。
struct ButtonOptions {
    std::function<void()> onClick;          // 点击回调（注入 rect.onClick）
    bool hasTextColorOverride = false;      // 是否使用 textColorOverride
    eui::Color textColorOverride;           // 文字色覆盖（主要给 Text 级）
};

// 渲染单按钮：stack(id) 内含 rect.bg（三态 hover + onClick + radius）+ 居中文字。
// 节点 id = `<id>`（stack）/ `<id>.bg`（rect）/ `<id>.text`（text），单测按此定位。
// 几何：(x, y) 左上角，(w, h) 尺寸；高度建议 kButtonHeight（34px）。
// spec §4.1 禁止项：阴影、边框（填充按钮）、圆角>8——本 helper 不加 border/shadow，
// radius 用 t.sizes.radiusWeChat（6px）。
void button(eui::Ui& ui, const std::string& id, const std::string& label,
            ButtonLevel level, float x, float y, float w, float h,
            const core::theme::Tokens& t,
            const ButtonOptions& options = {});

} // namespace ui_kit
