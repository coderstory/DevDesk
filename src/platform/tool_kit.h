// LLM 工具执行器公共脚手架（need:380 批C）：各 module_* 工具文件的重复副本收敛。
// 五件套语义来自 todo_tools/agent_runs_tool 等原副本，逐字段等价，不改行为。
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/mcp_tool.h"
#include "core/result.h"
#include "core/tool.h"
#include "platform/json.h"

namespace platform::tool_kit {

// 工具级错误：Result ok + isError + {"error":"<中文>"}（错误回喂 LLM）
[[nodiscard]] core::Result<core::ToolResult> toolError(const std::string& message);

// 参数解析：空串视为 {}（LLM 的 input 可能为空对象省略）；其余必须解析成 object
[[nodiscard]] core::Result<platform::json::JsonObject> parseArgs(const core::ToolArgs& args);

// 可选字符串字段：缺失 / 非字符串回空串
[[nodiscard]] std::string strField(const platform::json::JsonObject& obj, const char* key);

// 可选整数字段：缺失 / 非数字回 nullopt
[[nodiscard]] std::optional<int64_t> intField(const platform::json::JsonObject& obj, const char* key);

// JSON 数组组装：platform::json 无通用数组追加 API，逐元素 stringify 后文本
// 拼接，再 parse 校验（todo_tools 同款口径）
[[nodiscard]] core::Result<std::string> arrayOfJsonTexts(const std::vector<std::string>& items);

// ── MCP 对外适配（need394 D-1）──
// 智能体面工具名 → MCP 对外名：点号/短横换下划线（todo.list→todo_list），
// 起点号是为 MCP 官方规范的全 snake_case 示例对齐（存量 MCP 面全下划线）。
[[nodiscard]] std::string mcpToolName(const std::string& agentToolName);

// 把单个智能体工具（core::ITool）适配成 MCP 工具（core::mcp::IMcpTool）：
// 后端 100% 复用同一份实现（execute 直通，args/result 语义零漂移），
// 仅在边界做名/schema/文本三项换形。schemaJson() 在构造期一次性拆成
// description + input_schema（tools/list 每帧调用不再解析）。
class McpToolAdapter final : public core::mcp::IMcpTool {
public:
    McpToolAdapter(std::shared_ptr<core::ITool> tool, std::string mcpName);

    std::string name() const override { return mcpName_; }
    std::string description() const override { return description_; }
    std::string inputSchemaJson() const override { return inputSchema_; }
    core::Result<core::mcp::ToolResult> execute(
        const std::string& argumentsJson) override;

private:
    std::shared_ptr<core::ITool> tool_;
    std::string mcpName_;
    std::string description_;
    std::string inputSchema_;
};

// 一批智能体工具整批登入 MCP 对外面（module onStart 侧；单条失败记日志不中断，
// 与 ToolRegistry 面 registerAll 同口径）。工具实例由 adapter shared 持有，
// 在途 execute 不随模块停用悬垂。
void registerToolsAsMcp(
    core::mcp::McpToolRegistry& registry,
    const std::vector<std::shared_ptr<core::ITool>>& tools);

// 整批注销（名字由同一映射推出；对未注册名是 no-op，可安全重复调用）
void unregisterToolsAsMcp(
    core::mcp::McpToolRegistry& registry,
    const std::vector<std::shared_ptr<core::ITool>>& tools);

}  // namespace platform::tool_kit
