// src/memory_mcp_bridge.cpp
// 记忆 MCP 桥实现缝。独立 TU：只依赖 core（Result）与标准库，不链接
// module_memory——devdesk_tests / probe / live 目标编译本文件时不产生模块边；
// 真实现由 main.cpp 装配段注入（转发 module::memory::mcp 注册 API）。
#include "memory_mcp_bridge.h"

namespace app::memory_mcp {

namespace {

StatusHandler g_status = nullptr;
SetEnabledHandler g_setEnabled = nullptr;

} // namespace

void setHandlers(StatusHandler status, SetEnabledHandler setEnabled) {
    g_status = status;
    g_setEnabled = setEnabled;
}

LiveStatus status() {
    if (g_status == nullptr) {
        return LiveStatus{};  // 未装配 = 开关关（未注册）
    }
    return g_status();
}

core::Result<void> setEnabled(bool on) {
    if (g_setEnabled == nullptr) {
        return core::Result<void>::fail(core::ErrorCode::Internal,
                                        "记忆 MCP 桥未装配");
    }
    return g_setEnabled(on);
}

}  // namespace app::memory_mcp
