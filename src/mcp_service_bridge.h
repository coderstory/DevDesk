// src/mcp_service_bridge.h
// app 层 MCP 服务桥：函数指针缝 + 默认桩 + static_assert 签名哨兵（模式同
// module_toggle_bridge）。settings 模块只经此桥读状态 / 发指令，不 include
// module_mcp；实现 TU 只依赖 core（Result）与标准库，settings 与 probe 编译它
// 时不产生模块边。
#pragma once

#include <string>
#include <type_traits>

#include "core/result.h"

namespace app::mcp {

struct LiveStatus {
    bool running = false;
    int port = 0;
    std::string lastError;
};

using StartHandler = core::Result<void> (*)(int port);
using StopHandler = void (*)();
using ApplyPortHandler = core::Result<void> (*)(int port);
using StatusHandler = LiveStatus (*)();
using AllowDestructiveHandler = bool (*)();
using SetAllowDestructiveHandler = core::Result<void> (*)(bool);

// 每个参数传 nullptr = 该槽恢复默认桩；四个槽一起安装（装配期一次调用）。
void setHandlers(StartHandler start, StopHandler stop,
                 ApplyPortHandler applyPort, StatusHandler status);

// 「破坏性工具」开关槽：独立于四参 setHandlers，避免所有既有调用点改签名。
// 默认 getter = false、setter = fail「MCP 服务未装配」（fail-closed）。
void setDestructiveHandlers(AllowDestructiveHandler get,
                            SetAllowDestructiveHandler set);

core::Result<void> start(int port);
void stop();
core::Result<void> applyPort(int port);
LiveStatus status();
bool allowDestructive();
core::Result<void> setAllowDestructive(bool on);

}  // namespace app::mcp

static_assert(std::is_same_v<decltype(&app::mcp::start),
                             app::mcp::StartHandler>,
              "mcp_service_bridge: start 签名漂移");
static_assert(std::is_same_v<decltype(&app::mcp::stop),
                             app::mcp::StopHandler>,
              "mcp_service_bridge: stop 签名漂移");
static_assert(std::is_same_v<decltype(&app::mcp::applyPort),
                             app::mcp::ApplyPortHandler>,
              "mcp_service_bridge: applyPort 签名漂移");
static_assert(std::is_same_v<decltype(&app::mcp::status),
                             app::mcp::StatusHandler>,
              "mcp_service_bridge: status 签名漂移");
static_assert(std::is_same_v<decltype(&app::mcp::allowDestructive),
                             app::mcp::AllowDestructiveHandler>,
              "mcp_service_bridge: allowDestructive 签名漂移");
static_assert(std::is_same_v<decltype(&app::mcp::setAllowDestructive),
                             app::mcp::SetAllowDestructiveHandler>,
              "mcp_service_bridge: setAllowDestructive 签名漂移");
