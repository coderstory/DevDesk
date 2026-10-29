// src/module_memory/mcp/search_tool.h
// memory_search MCP 工具（core::mcp 接缝的 module_memory 实现）：把本机长期记忆
// 检索以单工具形态开放给 MCP 客户端。检索只走 IMemoryRetriever 缝、不直连 telemem；
// 开关注册态唯一事实源 = panel_settings.json 的 mcpSearchEnabled，setSearchEnabled
// 是「写盘 + 注册/注销」的单点维护处（main.cpp 启动装配与 app::memory_mcp 桥都只转发）。
// 模块隔离：本目录只 include core 与 module_memory 自有头。
#pragma once

#include <memory>
#include <string>

#include "core/mcp_tool.h"
#include "core/result.h"
#include "module_memory/gateway/telemem_client.h"

namespace module::memory::mcp {

inline constexpr char kMemorySearchToolName[] = "memory_search";

// 检索器注入缝：生产 = TelememHttpRetriever（registerSearchTool 构造，随注册表
// 存活）；测试注入假实现观测入参与降级分支（WebFetchMarkdownTool 的 Fetcher 先例）。
class MemorySearchTool final : public core::mcp::IMcpTool {
public:
    explicit MemorySearchTool(
        std::shared_ptr<gateway::IMemoryRetriever> retriever);

    std::string name() const override;
    std::string description() const override;
    std::string inputSchemaJson() const override;
    core::Result<core::mcp::ToolResult> execute(
        const std::string& argumentsJson) override;

private:
    std::shared_ptr<gateway::IMemoryRetriever> retriever_;
};

// 重名 fail 原样上抛（调用方 LOG_ERROR 即可，注册表既有语义；快速连拨/启动重放
// 的重复注册由 setSearchEnabled 幂等吸收）。
core::Result<void> registerSearchTool(core::mcp::McpToolRegistry& registry);

// 名字不存在 = no-op（unregisterTool 既有语义），不崩不报。
void unregisterSearchTool(core::mcp::McpToolRegistry& registry);

// 开关单点：先写 panel_settings.json（写盘失败 → fail 且注册态不动，UI 回弹 +
// 红字的依据），成功后按新值注册/注销。注册失败只 LOG_ERROR 不致命（重名/冲突
// 是理论态，服务与其余工具不受影响）。
core::Result<void> setSearchEnabled(core::mcp::McpToolRegistry& registry, bool on);

// 读 panel_settings.json 的 mcpSearchEnabled（旧文件缺键 = 默认 true）。
// 只适合启动装配等一次性读；每帧查询请走 app::memory_mcp::status()（读注册表）。
bool searchEnabledBySetting();

}  // namespace module::memory::mcp
