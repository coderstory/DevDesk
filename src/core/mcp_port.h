// MCP 服务监听端口常量的单一来源。消费方：module_mcp（默认端口 / 钳制范围）与
// module_settings（输入范围提示 + 落库钳制）——module_* 之间不可互相 include，
// 故两边共用本头，不各自定义。
// 纯常量头（inline constexpr，无 .cpp、无运行时依赖），不引入模块边。
#pragma once

namespace core {

// MCP 服务端口：默认值与合法闭区间
inline constexpr int kDefaultMcpPort = 8931;
inline constexpr int kMinMcpPort = 1;
inline constexpr int kMaxMcpPort = 65535;

} // namespace core