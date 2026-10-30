// src/module_settings/project_info_tool.h
// 需-11 project_info MCP 工具：把项目卡档案以单工具形态开放给 MCP 客户端
//（Claude Code 等）按名查询。薄封装 project_card_store 的 SQL；接线与
// memory_search 同款——实现归所属模块（module_settings），main.cpp 装配段注册，
// 无引擎依赖故恒注册（不设开关，也就不需要 memory_mcp_bridge 那类状态桥）。
// 模块隔离：只 include core 与本模块头；db 经构造注入（生产 = appDatabase()，
// 测试 = 临时库 / nullptr）。
#pragma once

#include <memory>
#include <string>

#include "core/mcp_tool.h"
#include "core/result.h"
#include "core/sqlite_db.h"

namespace module::settings {

inline constexpr char kProjectInfoToolName[] = "project_info";

class ProjectInfoTool final : public core::mcp::IMcpTool {
public:
    // db 允许 nullptr（装配期库未就绪）：execute 返回明确 isError 文案，不崩。
    explicit ProjectInfoTool(core::data::SqliteDb* db);

    std::string name() const override;
    std::string description() const override;
    std::string inputSchemaJson() const override;
    core::Result<core::mcp::ToolResult> execute(
        const std::string& argumentsJson) override;

private:
    core::data::SqliteDb* db_;
};

// 注册进 registry（重名 fail 原样上抛，调用方 LOG_ERROR 即可——注册表既有语义）。
core::Result<void> registerProjectInfoTool(core::mcp::McpToolRegistry& registry,
                                           core::data::SqliteDb* db);

// 名字不存在 = no-op（unregisterTool 既有语义）。
void unregisterProjectInfoTool(core::mcp::McpToolRegistry& registry);

}  // namespace module::settings
