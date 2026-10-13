// Claude 代理监听端口常量的单一来源。消费方：module_claude_proxy（钳制范围 / 默认端口）
// 与 module_settings（输入范围提示 + 落库钳制）——module_* 之间不可互相 include，
// 故两边共用本头，不各自定义。
// 纯常量头（inline constexpr，无 .cpp、无运行时依赖），不引入模块边。
#pragma once

namespace core {

// Claude 代理端口：默认值与合法闭区间
inline constexpr int kDefaultProxyPort = 8888;
inline constexpr int kMinProxyPort = 1;
inline constexpr int kMaxProxyPort = 65535;

} // namespace core
