// src/memory_mcp_bridge.h
// app 层记忆 MCP 桥：函数指针缝 + 默认桩 + static_assert 签名哨兵（模式同
// mcp_service_bridge）。module_mcp 的 MCP 页只经此桥读开关态 / 拨开关，不 include
// module_memory；实现 TU 只依赖 core（Result）与标准库，tests / probe / live 编译
// 它时不产生模块边。真实现由 main.cpp 装配段注入（转发 module::memory::mcp）。
#pragma once

#include <type_traits>

#include "core/result.h"

namespace app::memory_mcp {

struct LiveStatus {
    bool switchOn = false;  // memory_search 当前是否已注册（注册 ⟺ 开关 不变式）
};

using StatusHandler = LiveStatus (*)();
using SetEnabledHandler = core::Result<void> (*)(bool);

// 每个参数传 nullptr = 该槽恢复默认桩；两槽一起安装（装配期一次调用）。
void setHandlers(StatusHandler status, SetEnabledHandler setEnabled);

LiveStatus status();
core::Result<void> setEnabled(bool on);

}  // namespace app::memory_mcp

static_assert(std::is_same_v<decltype(&app::memory_mcp::status),
                             app::memory_mcp::StatusHandler>,
              "memory_mcp_bridge: status 签名漂移");
static_assert(std::is_same_v<decltype(&app::memory_mcp::setEnabled),
                             app::memory_mcp::SetEnabledHandler>,
              "memory_mcp_bridge: setEnabled 签名漂移");
