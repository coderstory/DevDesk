// src/ui_kit/empty_state.h
// presentation 能力层：空状态（spec §4.12）。
// W2d 新增：极简文字（wechat.textSecondary 灰 #666666）+ 可选就近按钮
// （W2b button helper Secondary 级，浅灰底 + 深灰字）。
//
// 防 §4.12 禁止项：
//   - 插画式空状态（本 helper 不画任何插画，只画一行极简文字）
//   - 情绪化文案（message 由调用方传，spec §6.2「克制命名」纪律在调用方；
//     helper 不加任何情绪化装饰，纯文字 + 可选按钮）
//   - 空状态无操作入口（§5.6「按钮就近原则」：action 由调用方传 label+onClick，
//     helper 在文字下方就近画按钮，按钮直接触发对应创建操作）
//
// 色值纪律（D-03 / §8 #11）：文字走 wechat.textSecondary；按钮走 W2b button
// helper Secondary 级（内部走 wechat.secondaryButtonBg + wechat.textPrimary）。
#pragma once

#include <core/theme.h>
#include <eui/dsl.h>

#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace ui_kit {

// §4.12 空状态：极简文字居中 + 可选就近按钮。
//  - action = {label, onClick}：若有，按钮走 W2b button helper Secondary 级
//    （§4.1 Secondary 浅灰底 + 深灰字），就近触发对应创建操作（§5.6）
//  - action = nullopt：只画极简文字（§4.12「全场景允许」——无操作入口的空状态
//    如「无搜索结果」纯提示也合法）
// 几何：(x, y, w, h) 由调用方给；文字居中于上半区，按钮居中于下半区。
// 节点 id = `<id>.message`（text）/ `<id>.action`（W2b button stack，含
// `<id>.action.bg` + `<id>.action.text`），单测按此定位。
void emptyState(eui::Ui& ui, const std::string& id, const std::string& message,
                const std::optional<std::pair<std::string, std::function<void()>>>& action,
                float x, float y, float w, float h,
                const core::theme::Tokens& t);

} // namespace ui_kit
