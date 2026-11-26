// src/mcp_service_bridge.cpp
// MCP 桥实现缝。独立 TU：只依赖 core（Result）与标准库，不链接 module_mcp——
// devdesk_tests / probe / live 目标编译本文件时不产生模块边；真实现由 main.cpp
// 装配段注入（转发 globalMcpService）。
#include "mcp_service_bridge.h"

namespace app::mcp {

namespace {

StartHandler g_start = nullptr;
StopHandler g_stop = nullptr;
ApplyPortHandler g_applyPort = nullptr;
StatusHandler g_status = nullptr;
AllowDestructiveHandler g_allowDestructive = nullptr;
SetAllowDestructiveHandler g_setAllowDestructive = nullptr;

} // namespace

void setHandlers(StartHandler start, StopHandler stop,
                 ApplyPortHandler applyPort, StatusHandler status) {
    g_start = start;
    g_stop = stop;
    g_applyPort = applyPort;
    g_status = status;
}

void setDestructiveHandlers(AllowDestructiveHandler get,
                            SetAllowDestructiveHandler set) {
    g_allowDestructive = get;
    g_setAllowDestructive = set;
}

core::Result<void> start(int port) {
    if (g_start == nullptr) {
        return core::Result<void>::fail(core::ErrorCode::Internal,
                                        "MCP 服务未装配");
    }
    return g_start(port);
}

void stop() {
    if (g_stop != nullptr) {
        g_stop();
    }
}

core::Result<void> applyPort(int port) {
    if (g_applyPort == nullptr) {
        return core::Result<void>::fail(core::ErrorCode::Internal,
                                        "MCP 服务未装配");
    }
    return g_applyPort(port);
}

LiveStatus status() {
    if (g_status == nullptr) {
        return LiveStatus{};  // 未装配 = 全空（running=false / port=0 / 无错误）
    }
    return g_status();
}

bool allowDestructive() {
    if (g_allowDestructive == nullptr) return false;  // 未装配 = 关（fail-closed）
    return g_allowDestructive();
}

core::Result<void> setAllowDestructive(bool on) {
    if (g_setAllowDestructive == nullptr) {
        return core::Result<void>::fail(core::ErrorCode::Internal,
                                        "MCP 服务未装配");
    }
    return g_setAllowDestructive(on);
}

}  // namespace app::mcp
