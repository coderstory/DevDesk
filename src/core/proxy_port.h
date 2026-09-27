// src/core/proxy_port.h
// Claude 代理监听端口常量（任务 D / R0，审计 D7「端口常量单一来源」）
//
// 为什么落在 core：这个值有两个消费方——module_claude_proxy（ClaudeService
// 的钳制范围 / 默认端口 / startProxy 的默认实参）与 module_settings（设置页
// 数字输入的范围提示 + config.json 落盘钳制），而 module_* 之间不能互相
// include（防循环，见 CLAUDE.md「架构要点」）。此前两处各自定义同值常量、靠
// 注释约定「保持字面一致」——改一处漏一处就是静默的行为分叉。R0 把取值收到
// 这里，两侧都从本头取（现取值漂移只可能来自这一处）。
//
// 形态：纯常量头（inline constexpr，无 .cpp、无依赖、不牵任何运行时），
// 不引入模块边、不违反 R3「设计数值收口」的方向。
#pragma once

namespace core {

// Claude 代理端口：默认值与合法闭区间（产品审计 2026-09-23 [14]）
inline constexpr int kDefaultProxyPort = 8888;
inline constexpr int kMinProxyPort = 1;
inline constexpr int kMaxProxyPort = 65535;

} // namespace core
