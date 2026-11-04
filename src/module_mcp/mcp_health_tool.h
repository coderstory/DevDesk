// mcp.health：本机 MCP hub 只读诊断工具（need 219 F-05）。
// 状态源 = app::mcp 桥（settings 同款函数指针缝，模块层零 McpService 依赖）；
// 实现与工厂定义落 mcp_page_module.cpp——探针 / ui_driver / github_sync_live
// 直编该 TU 且链接集没有 devdesk_module_mcp 库，跨 TU 引用即断链（2026-11-05
// 实测）。注册跟随 MCP 页模块启停（onStart/onStop）。
#pragma once

#include <memory>

#include "core/tool.h"

namespace module::mcp {

std::shared_ptr<core::ITool> makeMcpHealthTool();

} // namespace module::mcp
