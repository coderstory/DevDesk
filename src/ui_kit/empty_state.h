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

// 空态内容包（2026-11-01 新增，R3）。字段全可选：只给 message 即退化为旧形态。
struct EmptyStateOptions {
    // 主句：说明「这里为什么是空的」。
    std::string message;
    // 引导句（可选）：告诉用户下一步做什么，比主句更具体。
    std::string guide;
    // 图标（可选）：FontAwesome 码点，画在 40×40 主题色圆角底上。
    std::optional<unsigned int> icon;
    // 行动按钮（可选）：{标签, 回调}，Secondary 级。
    std::optional<std::pair<std::string, std::function<void()>>> action;
};

// 2026-11-01：空态扩为全站唯一形态——图标 + 主句 + 引导句 + 行动按钮，
// 四者作为一组**整体垂直居中**于给定盒内。
//
// 此前各模块自行拼装出四种形态：① release 列表＝大白卡包裹 ② release 配置＝裸文字
// 两行 ③ agent＝占满剩余高度的大白卡但内容顶部对齐（下方 70% 空白，像坏了）
// ④ todo/mcp＝单行文字。统一后一律不再外套大白卡。
//
// 节点 id = `<id>.icon.bg` / `<id>.icon` / `<id>.message` / `<id>.guide` /
// `<id>.action`（含 `.action.bg` / `.action.text`），单测按此定位。
void emptyState(eui::Ui& ui, const std::string& id,
                const EmptyStateOptions& options,
                float x, float y, float w, float h,
                const core::theme::Tokens& t);

// 兼容入口：等价于只填 message 的旧形态（原有调用方零改动）。
void emptyState(eui::Ui& ui, const std::string& id, const std::string& message,
                const std::optional<std::pair<std::string, std::function<void()>>>& action,
                float x, float y, float w, float h,
                const core::theme::Tokens& t);

} // namespace ui_kit
