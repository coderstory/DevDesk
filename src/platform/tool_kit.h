// LLM 工具执行器公共脚手架（need:380 批C）：各 module_* 工具文件的重复副本收敛。
// 五件套语义来自 todo_tools/agent_runs_tool 等原副本，逐字段等价，不改行为。
#pragma once

#include <optional>
#include <string>
#include <vector>

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

// ── MCP 对外命名（debt#416 单一注册源裁决）──
// 智能体面工具名 → MCP 对外名：点号/短横换下划线（todo.list→todo_list）。唯一
// 消费点 = module_mcp::ToolMirror（镜像注册/注销两侧都必须过它，防两处各改一半
// 留孤儿注册）；起点号是为 MCP 官方规范的全 snake_case 示例对齐（存量 MCP 面全下划线）。
[[nodiscard]] std::string mcpToolName(const std::string& agentToolName);

}  // namespace platform::tool_kit
